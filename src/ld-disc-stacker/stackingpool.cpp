/************************************************************************

    stackingpool.cpp

    ld-disc-stacker - Disc stacking for ld-decode
    Copyright (C) 2020-2025 Simon Inns

    This file is part of ld-decode-tools.

    ld-disc-stacker is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "stackingpool.h"
#include "vbidecoder.h"
#include "discnumbering.h"
#include "stackframe.h"
#include "tbc/logging.h"
#include "ldxcapture.h"

StackingPool::StackingPool(QString _outputFilename, QString _outputMetadataFilename,
                             qint32 _maxThreads, QVector<LdDecodeMetaData *> &_ldDecodeMetaData, QVector<SourceVideo *> &_sourceVideos,
                             qint32 _mode, qint32 _smartThreshold, bool _reverse, bool _noDiffDod, bool _passThrough, bool _integrityCheck, bool _verbose, QObject *parent)
    : QObject(parent), outputFilename(_outputFilename), outputMetadataFilename(_outputMetadataFilename),
      maxThreads(_maxThreads), mode(_mode), smartThreshold(_smartThreshold), reverse(_reverse), noDiffDod(_noDiffDod), passThrough(_passThrough), integrityCheck(_integrityCheck), verbose(_verbose),
      abort(false), ldDecodeMetaData(_ldDecodeMetaData), sourceVideos(_sourceVideos)
{
}

void StackingPool::setOffsets(const QVector<qint32> &sourceOffsets)
{
    offsets = sourceOffsets;
    useOffsets = true;
}

bool StackingPool::process()
{
    qInfo() << "Performing final sanity checks...";
    // Open the target video (a CVBS output keeps the input's sample encoding)
    const LdDecodeMetaData::VideoParameters &outputParameters = ldDecodeMetaData[0]->getVideoParameters();
    targetVideo.setLdxVideo(outputParameters.fieldWidth, outputParameters.fieldHeight, outputParameters.sampleRate, QString(),
                            historyWith(LdxCapture::historyEntry("process", "stacking of " + QString::number(sourceVideos.size()) + " sources")));
    targetVideo.setLdxCvbs(ldxCvbs);
    if (ldxStream) targetVideo.setLdxStream(ldDecodeMetaData[0]->parametersJson());
    if (!targetVideo.open(outputFilename, ldDecodeMetaData[0]->getVideoParameters().system,
                          ldDecodeMetaData[0]->getCvbsFileMetadata().sampleEncoding)) {
        return false;
    }

    // In a stream the metadata of each field go with it, so the ones that do not depend on the stacking are made now
    if (ldxStream) correctPhaseIDs();

    // If there is a leading field in the TBC which is out of field order, we need to copy it
    // to ensure the metadata files match up
    qInfo() << "Verifying leading fields match...";
    qint32 firstFieldNumber = ldDecodeMetaData[0]->getFirstFieldNumber(1);
    qint32 secondFieldNumber = ldDecodeMetaData[0]->getSecondFieldNumber(1);

    if (firstFieldNumber != 1 && secondFieldNumber != 1) {
        SourceVideo::Data sourceField = sourceVideos[0]->getVideoField(1);
        if (!writeOutputField(sourceField)) {
            // Could not write to target TBC file
            qInfo() << "Writing first field to the output TBC file failed";
            targetVideo.close();
            return false;
        }
    }

    qInfo() << "Scanning source videos for VBI frame number ranges...";
    // Get the VBI frame range for all sources (not needed when the sources are lined up by offsets)
    if (!useOffsets && !setMinAndMaxVbiFrames()) {
        qInfo() << "It was not possible to determine the VBI frame number range for the source video - cannot continue!";
        return false;
    }

    // Show some information for the user
    qInfo() << "Using" << maxThreads << "threads to process" << ldDecodeMetaData[0]->getNumberOfFrames() << "frames";

    // Initialise processing state
    inputFrameNumber = 1;
    outputFrameNumber = 1;
    lastFrameNumber = ldDecodeMetaData[0]->getNumberOfFrames();
    skippedFrame = 0;
    totalTimer.start();

    // Start a vector of decoding threads to process the video
    qInfo() << "Beginning multi-threaded disc stacking process...";
    QVector<QThread *> threads;
    threads.resize(maxThreads);
    for (qint32 i = 0; i < maxThreads; i++) {
        threads[i] = new Stacker(abort, *this);
        threads[i]->start(QThread::LowPriority);
    }

    // Wait for the workers to finish
    for (qint32 i = 0; i < maxThreads; i++) {
        threads[i]->wait();
        delete threads[i];
    }

    // Did any of the threads abort?
    if (abort) {
        targetVideo.close();
        return false;
    }

    // Show the processing speed to the user
    const double totalSecs = (static_cast<double>(totalTimer.elapsed()) / 1000.0);
    qInfo() << "Disc stacking complete -" << lastFrameNumber << "frames in" << totalSecs << "seconds (" <<
               lastFrameNumber / totalSecs << "FPS )";
    if(integrityCheck)
    {
        qInfo() << "Stacking found " << skippedFrame << "corrupted frame";
    }
    // Close the target video (an LDX output is finished before its metadata are added to it)
    if (!targetVideo.close()) return false;

    // (a stream has sent its metadata with the fields)
    if (!ldxStream) {
        qInfo() << "Creating metadata file for stacked TBC...";
        if (!correctMetaData().write(outputMetadataFilename)) return false;
    }

    return true;
}

// Get the next frame that needs processing from the input.
//
// Returns true if a frame was returned, false if the end of the input has been
// reached.
bool StackingPool::getInputFrame(qint32& frameNumber,
                                  QVector<qint32>& firstFieldNumber, QVector<SourceVideo::Data>& firstFieldVideoData, QVector<LdDecodeMetaData::Field>& firstFieldMetadata,
                                  QVector<qint32>& secondFieldNumber, QVector<SourceVideo::Data>& secondFieldVideoData, QVector<LdDecodeMetaData::Field>& secondFieldMetadata,
                                  QVector<LdDecodeMetaData::VideoParameters>& videoParameters,
                                  qint32& _mode, qint32& _smartThreshold, bool& _reverse, bool& _noDiffDod, bool& _passThrough,
                                  bool& _verbose, QVector<qint32>& availableSourcesForFrame)
{
    QMutexLocker locker(&inputMutex);

    if (inputFrameNumber > lastFrameNumber) {
        // No more input frames
        return false;
    }

    frameNumber = inputFrameNumber;
    inputFrameNumber++;

    // Determine the number of sources available (included padded sources)
    qint32 numberOfSources = sourceVideos.size();

    if(verbose){tbcDebugStream().nospace() << "Processing sequential frame number #" <<
                          frameNumber << " from " << numberOfSources << " possible source(s)";}

    // Prepare the vectors
    firstFieldNumber.resize(numberOfSources);
    firstFieldVideoData.resize(numberOfSources);
    firstFieldMetadata.resize(numberOfSources);
    secondFieldNumber.resize(numberOfSources);
    secondFieldVideoData.resize(numberOfSources);
    secondFieldMetadata.resize(numberOfSources);
    videoParameters.resize(numberOfSources);

    // Get the current VBI frame number based on the first source
    qint32 currentVbiFrame = -1;
    if (numberOfSources > 1 && !useOffsets) currentVbiFrame = convertSequentialFrameNumberToVbi(frameNumber, 0);

    for (qint32 sourceNo = 0; sourceNo < numberOfSources; sourceNo++) {
        // Determine the fields for the input frame
        firstFieldNumber[sourceNo] = -1;
        secondFieldNumber[sourceNo] = -1;

        if (sourceNo == 0) {
            // No need to perform VBI frame number mapping on the first source
            firstFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(frameNumber);
            secondFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(frameNumber);
            if(verbose){tbcDebugStream().nospace() << "Source #0 fields are " <<
                                  firstFieldNumber[sourceNo] << "/" << secondFieldNumber[sourceNo];}
        } else if (useOffsets) {
            // Lined up by offset: the frame of this source that falls on the reference's frame
            const qint32 ownFrame = sourceFrameFor(frameNumber, sourceNo, -1);
            if (ownFrame >= 1) {
                firstFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(ownFrame);
                secondFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(ownFrame);
            } else if (verbose) {
                tbcDebugStream().nospace() << "Source #" << sourceNo << " has no frame at offset " << offsets.value(sourceNo, 0);
            }
        } else if (ranges.contains(currentVbiFrame, sourceNo)) {
            // Use VBI frame number mapping to get the same frame from the
            // current additional source
            qint32 currentSourceFrameNumber = convertVbiFrameNumberToSequential(currentVbiFrame, sourceNo);

            // Check the current source contains the frame
            if (ldDecodeMetaData[sourceNo]->getNumberOfFrames() < currentSourceFrameNumber) {
                firstFieldNumber[sourceNo] = -1;
                secondFieldNumber[sourceNo] = -1;

                if(verbose){tbcDebugStream().nospace() << "Source #" << sourceNo << " does not contain VBI frame number " << currentVbiFrame;}
            } else {
                firstFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(currentSourceFrameNumber);
                secondFieldNumber[sourceNo] = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(currentSourceFrameNumber);

                if(verbose){tbcDebugStream().nospace() << "Source #" << sourceNo << " has VBI frame number " << currentVbiFrame <<
                            " and fields " << firstFieldNumber[sourceNo] << "/" << secondFieldNumber[sourceNo];}
            }
        } else if(verbose){
            tbcDebugStream().nospace() << "Source #" << sourceNo << " does not contain a usable frame";
        }

        // If the field numbers are valid - get the rest of the required data
        if (firstFieldNumber[sourceNo] != -1 && secondFieldNumber[sourceNo] != -1) {
            // Fetch the input data (get the fields in TBC sequence order to save seeking)
            if (firstFieldNumber[sourceNo] < secondFieldNumber[sourceNo]) {
                firstFieldVideoData[sourceNo] = sourceVideos[sourceNo]->getVideoField(firstFieldNumber[sourceNo]);
                secondFieldVideoData[sourceNo] = sourceVideos[sourceNo]->getVideoField(secondFieldNumber[sourceNo]);
            } else {
                secondFieldVideoData[sourceNo] = sourceVideos[sourceNo]->getVideoField(secondFieldNumber[sourceNo]);
                firstFieldVideoData[sourceNo] = sourceVideos[sourceNo]->getVideoField(firstFieldNumber[sourceNo]);
            }

            firstFieldMetadata[sourceNo] = ldDecodeMetaData[sourceNo]->getField(firstFieldNumber[sourceNo]);
            secondFieldMetadata[sourceNo] = ldDecodeMetaData[sourceNo]->getField(secondFieldNumber[sourceNo]);
            videoParameters[sourceNo] = ldDecodeMetaData[sourceNo]->getVideoParameters();
        }
    }

    // Figure out which of the available sources can be used to correct the current frame
    availableSourcesForFrame.clear();
    if (useOffsets) {
        availableSourcesForFrame = getAvailableSourcesByOffset(frameNumber);
    } else if (numberOfSources > 1) {
        availableSourcesForFrame = getAvailableSourcesForFrame(currentVbiFrame);
    } else {
        availableSourcesForFrame.append(0);
    }

    if(integrityCheck)
    {
        const QVector<qint32> availableSourcesForFrameTmp = availableSourcesForFrame;
        const int size = availableSourcesForFrameTmp.size();
        for(int i = 0; i < size;i++)
        {

            if(!isIntegrityOk(firstFieldVideoData[availableSourcesForFrameTmp[i]],videoParameters[0]))
            {
                availableSourcesForFrame.remove(i);
                skippedFrame++;
                qInfo() << "found corrupted data at output frame : " << frameNumber << " from source (" << availableSourcesForFrameTmp[i] << ") field 1";
            }
            else if(!isIntegrityOk(secondFieldVideoData[availableSourcesForFrameTmp[i]],videoParameters[0]))
            {
                availableSourcesForFrame.remove(i);
                skippedFrame++;
                qInfo() << "found corrupted data at output frame : " << frameNumber << " from source (" << availableSourcesForFrameTmp[i] << ") field 2";
            }
        }
    }

    // Set the mode used for this frame (auto goes by how many sources there are)
    _mode = mode != -1 ? mode : StackerCore::autoMode(numberOfSources);

    // Set the other miscellaneous parameters
    _smartThreshold = smartThreshold;
    _reverse = reverse;
    _noDiffDod = noDiffDod;
    _passThrough = passThrough;
    _verbose = verbose;

    return true;
}

// Put a corrected frame into the output stream.
//
// The worker threads will complete frames in an arbitrary order, so we can't
// just write the frames to the output file directly. Instead, we keep a map of
// frames that haven't yet been written; when a new frame comes in, we check
// whether we can now write some of them out.
//
// Returns true on success, false on failure.
bool StackingPool::setOutputFrame(qint32 frameNumber,
                                   SourceVideo::Data firstTargetFieldData, SourceVideo::Data secondTargetFieldData,
                                   qint32 firstFieldSeqNo, qint32 secondFieldSeqNo,
                                   DropOuts firstTargetFieldDropOuts, DropOuts secondTargetFieldDropouts)
{
    QMutexLocker locker(&outputMutex);

    // Put the output frame into the map
    OutputFrame pendingFrame;
    pendingFrame.firstTargetFieldData = firstTargetFieldData;
    pendingFrame.secondTargetFieldData = secondTargetFieldData;
    pendingFrame.firstFieldSeqNo = firstFieldSeqNo;
    pendingFrame.secondFieldSeqNo = secondFieldSeqNo;
    pendingFrame.firstTargetFieldDropOuts = firstTargetFieldDropOuts;
    pendingFrame.secondTargetFieldDropOuts = secondTargetFieldDropouts;

    pendingOutputFrames[frameNumber] = pendingFrame;

    // Write out as many frames as possible
    while (pendingOutputFrames.contains(outputFrameNumber)) {
        const OutputFrame &outputFrame = pendingOutputFrames.value(outputFrameNumber);

        // Clear any existing dropout data
        ldDecodeMetaData[0]->clearFieldDropOuts(outputFrame.firstFieldSeqNo);
        ldDecodeMetaData[0]->clearFieldDropOuts(outputFrame.secondFieldSeqNo);

        // Write the new dropout data into the LdDecodeMetaData output
        ldDecodeMetaData[0]->updateFieldDropOuts(outputFrame.firstTargetFieldDropOuts, outputFrame.firstFieldSeqNo);
        ldDecodeMetaData[0]->updateFieldDropOuts(outputFrame.secondTargetFieldDropOuts, outputFrame.secondFieldSeqNo);

        // In a stream the metadata of the two fields are final now, and go with them
        if (ldxStream) {
            replaceFieldMetaData<1>(outputFrameNumber);
            replaceFieldMetaData<2>(outputFrameNumber);
        }

        // Save the frame data to the output file (with the fields in the correct order)
        bool writeFail = false;
        if (outputFrame.firstFieldSeqNo < outputFrame.secondFieldSeqNo) {
            // Save the first field and then second field to the output file
            if (!writeOutputField(outputFrame.firstTargetFieldData)) writeFail = true;
            if (!writeOutputField(outputFrame.secondTargetFieldData)) writeFail = true;
        } else {
            // Save the second field and then first field to the output file
            if (!writeOutputField(outputFrame.secondTargetFieldData)) writeFail = true;
            if (!writeOutputField(outputFrame.firstTargetFieldData)) writeFail = true;
        }

        // Was the write successful?
        if (writeFail) {
            // Could not write to target TBC file
            qCritical() << "Writing fields to the output TBC file failed";
            targetVideo.close();
            return false;
        }

        // Show debug
        tbcDebugStream().nospace() << "Processed frame " << outputFrameNumber;

        if (outputFrameNumber % 100 == 0) {
            qInfo() << "Processed and written frame" << outputFrameNumber;
        }

        pendingOutputFrames.remove(outputFrameNumber);
        outputFrameNumber++;
    }

    return true;
}

// Determine the minimum and maximum VBI frame numbers for all sources
// Expects sourceVideos[] and ldDecodeMetaData[] to be populated
// Note: This function returns frame number even if the disc is CLV - conversion
// from timecodes is performed automatically.
bool StackingPool::setMinAndMaxVbiFrames()
{
    return ranges.scan(ldDecodeMetaData);
}

// Method to convert the first source sequential frame number to a VBI frame number
qint32 StackingPool::convertSequentialFrameNumberToVbi(qint32 sequentialFrameNumber, qint32 sourceNumber)
{
    return ranges.toDisc(sequentialFrameNumber, sourceNumber);
}

// Method to convert a VBI frame number to a sequential frame number
qint32 StackingPool::convertVbiFrameNumberToSequential(qint32 vbiFrameNumber, qint32 sourceNumber)
{
    // Offset the VBI frame number to get the sequential source frame number
    return ranges.toSequential(vbiFrameNumber, sourceNumber);
}

bool StackingPool::isIntegrityOk(const SourceVideo::Data& inputFields,const LdDecodeMetaData::VideoParameters& videoParameters)
{
    qint32 count = 0;
    for (qint32 y = 0; y < videoParameters.fieldHeight; y++)
    {
        if(inputFields[(videoParameters.fieldWidth * y) + 4] > (videoParameters.black16bIre - (10 * 256)))
        {
            count++;
        }
        else
        {
            count = 0;
        }
        if(count == 3)
        {
            return false;
        }
    }
    return true;
}

// The frame of a source that falls on a frame of the reference (source 0) when the sources are lined up by
// offsets, or -1 if the source has none there
qint32 StackingPool::sourceFrameFor(qint32 frameNumber, qint32 sourceNo, qint32 /*currentVbiFrame*/)
{
    return StackFrame::ownFrame(frameNumber, sourceNo == 0 ? 0 : offsets.value(sourceNo, 0), ldDecodeMetaData[sourceNo]->getNumberOfFrames());
}

// The sources that have a frame (not a padded one) at a frame of the reference, lined up by offsets
QVector<qint32> StackingPool::getAvailableSourcesByOffset(qint32 frameNumber)
{
    QVector<qint32> availableSourcesForFrame;
    for (qint32 sourceNo = 0; sourceNo < sourceVideos.size(); sourceNo++) {
        const qint32 ownFrame = sourceFrameFor(frameNumber, sourceNo, -1);
        if (ownFrame < 1) continue;
        const qint32 firstFieldNumber = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(ownFrame);
        const qint32 secondFieldNumber = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(ownFrame);
        if (firstFieldNumber < 1 || secondFieldNumber < 1) continue;
        if (!ldDecodeMetaData[sourceNo]->getField(firstFieldNumber).pad && !ldDecodeMetaData[sourceNo]->getField(secondFieldNumber).pad) {
            availableSourcesForFrame.append(sourceNo);
        }
    }
    return availableSourcesForFrame;
}

// Method that returns a vector of the sources that contain data for the required VBI frame number
QVector<qint32> StackingPool::getAvailableSourcesForFrame(qint32 vbiFrameNumber)
{
    QVector<qint32> availableSourcesForFrame;
    for (qint32 sourceNo = 0; sourceNo < sourceVideos.size(); sourceNo++) {
        if (ranges.contains(vbiFrameNumber, sourceNo)) {
            // Get the field numbers for the frame - THIS CRASHES
            qint32 sequentialFrameNumber = convertVbiFrameNumberToSequential(vbiFrameNumber, sourceNo);

            // Check the source contains enough frames to have the required sequential frame
            if (ldDecodeMetaData[sourceNo]->getNumberOfFrames() < sequentialFrameNumber)
            {
                // Sequential frame is out of bounds
                tbcDebugStream() << "VBI Frame number" << vbiFrameNumber << "is out of bounds for source " << sourceNo;
            } else {
                // Sequential frame is in bounds
                qint32 firstFieldNumber = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(sequentialFrameNumber);
                qint32 secondFieldNumber = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(sequentialFrameNumber);

                // Ensure the frame is not a padded field (i.e. missing)
                if (ldDecodeMetaData[sourceNo]->getField(firstFieldNumber).pad == false && ldDecodeMetaData[sourceNo]->getField(secondFieldNumber).pad == false) {
                    availableSourcesForFrame.append(sourceNo);
                } else if(verbose){
                    if (ldDecodeMetaData[sourceNo]->getField(firstFieldNumber).pad == true) tbcDebugStream() << "First field number" << firstFieldNumber << "of source" << sourceNo << "is padded";
                    if (ldDecodeMetaData[sourceNo]->getField(secondFieldNumber).pad == true) tbcDebugStream() << "Second field number" << firstFieldNumber << "of source" << sourceNo << "is padded";
                }
            }
        }
    }

    if (availableSourcesForFrame.size() != sourceVideos.size() && verbose) {
        if (availableSourcesForFrame.size() > 0) {
            tbcDebugStream() << "VBI Frame number" << vbiFrameNumber << "has only" << availableSourcesForFrame.size() << "available sources";
        } else {
            qInfo() << "Warning: VBI Frame number" << vbiFrameNumber << "has ZERO available sources (all sources padded?)";
        }
    }

    return availableSourcesForFrame;
}

// Write a field to the output file.
// Returns true on success, false on failure.
bool StackingPool::writeOutputField(const SourceVideo::Data &fieldData)
{
    if (ldxStream) {
        // The metadata of the field that goes in this place of the output
        const qint32 position = ++streamFieldCount;
        if (position <= ldDecodeMetaData[0]->getNumberOfFields()) {
            LdDecodeMetaData::Field field = ldDecodeMetaData[0]->getField(position);
            field.seqNo = position;
            return targetVideo.writeField(fieldData, LdDecodeMetaData::fieldToJson(field));
        }
    }
    return targetVideo.writeField(fieldData);
}

QJsonArray StackingPool::historyWith(const QJsonObject &entry) const
{
    QJsonArray history = sourceHistory;
    history.append(entry);
    return history;
}

void StackingPool::correctPhaseIDs()
{
    StackFrame::repairFieldPhases(*ldDecodeMetaData[0]);
}

template<int field>
void StackingPool::replaceFieldMetaData(qint32 frameNumber)
{
    const qint32 currentVbiFrame = useOffsets ? -1 : convertSequentialFrameNumberToVbi(frameNumber, 0);

    qint32 fieldNumber = 0;
    if constexpr (field == 1) {
        fieldNumber = ldDecodeMetaData[0]->getFirstFieldNumber(frameNumber);
    } else {
        fieldNumber = ldDecodeMetaData[0]->getSecondFieldNumber(frameNumber);
    }

    const LdDecodeMetaData::Field &currentField = ldDecodeMetaData[0]->getField(fieldNumber);
    if (currentField.pad) {
        for (int sourceNo = 1; sourceNo < ldDecodeMetaData.size(); ++sourceNo) {
            // The frame of the other source that goes with this one
            qint32 otherFrame = frameNumber;
            if (useOffsets) {
                otherFrame = sourceFrameFor(frameNumber, sourceNo, -1);
                if (otherFrame < 1) continue;
            } else {
                if (!ranges.contains(currentVbiFrame, sourceNo)) {
                    continue;
                }
                const qint32 currentSourceFrameNumber = convertVbiFrameNumberToSequential(currentVbiFrame, sourceNo);
                if (ldDecodeMetaData[sourceNo]->getNumberOfFrames() < currentSourceFrameNumber) {
                    continue;
                }
            }
            qint32 otherFieldNumber = 0;
            if constexpr (field == 1) {
                otherFieldNumber = ldDecodeMetaData[sourceNo]->getFirstFieldNumber(otherFrame);
            } else {
                otherFieldNumber = ldDecodeMetaData[sourceNo]->getSecondFieldNumber(otherFrame);
            }
            if (ldDecodeMetaData[sourceNo]->getField(otherFieldNumber).pad) {
                continue;
            }
            LdDecodeMetaData::Field potentialField = ldDecodeMetaData[sourceNo]->getField(otherFieldNumber);
            potentialField.seqNo = currentField.seqNo;
            potentialField.fieldPhaseID = currentField.fieldPhaseID;
            potentialField.dropOuts = currentField.dropOuts;
            ldDecodeMetaData[0]->updateField(potentialField, fieldNumber);
            break;
        }
    }
}

LdDecodeMetaData &StackingPool::correctMetaData()
{
    correctPhaseIDs();
    const qint32 frameCount = ldDecodeMetaData[0]->getNumberOfFrames();
    for (qint32 frameNumber = 1; frameNumber <= frameCount; ++frameNumber) {
        replaceFieldMetaData<1>(frameNumber);
        replaceFieldMetaData<2>(frameNumber);
    }
    return *ldDecodeMetaData[0];
}
