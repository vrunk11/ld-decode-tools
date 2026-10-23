/******************************************************************************
 * mappedmetadata.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mappedmetadata.h"

#include <QDebug>

#include <QFileInfo>

#include "cvbs.h"
#include "cvbsaudio.h"
#include "vbidecoder.h"

namespace MappedMetadata {

// Convert a frame number to the VBI hex frame number representation
// See the IEC specification for details of the VBI format
qint32 frameToVbi(qint32 discFrame)
{
    // Generate a string containing the required number
    QString number = "00F" + QString("%1").arg(discFrame, 5, 10, QChar('0'));
    bool ok;
    qint32 returnValue = number.toInt(&ok, 16);
    if (!ok) returnValue = 0;

    return returnValue;
}

// Convert a frame number to a VBI CLV picture number
// See the IEC specification for details of the VBI format
qint32 frameToClvPicNo(LdDecodeMetaData &metadata, qint32 discFrame)
{
    // Convert the frame number into a CLV timecode
    LdDecodeMetaData::ClvTimecode timecode = metadata.convertFrameNumberToClvTimecode(discFrame);

    // Generate the seconds
    qint32 secondsX1;
    if (timecode.seconds % 10 == 0) secondsX1 = timecode.seconds;
    else secondsX1 = (timecode.seconds - (timecode.seconds % 10));
    qint32 secondsX3 = timecode.seconds - secondsX1;
    secondsX1 = ((secondsX1 + 10) / 10) + 9;

    // Generate a string containing the required number
    QString number = "008" + QString("%1").arg(secondsX1, 1, 16, QChar('0')) + "E" +
            QString("%1").arg(secondsX3, 1, 10, QChar('0')) +
            QString("%1").arg(timecode.pictureNumber, 2, 10, QChar('0'));
    bool ok;
    qint32 returnValue = number.toInt(&ok, 16);
    if (!ok) returnValue = 0;

    return returnValue;
}

// Convert a frame number to a CLV programme time code
// See the IEC specification for details of the VBI format
qint32 frameToClvTimeCode(LdDecodeMetaData &metadata, qint32 discFrame)
{
    // Convert the frame number into a CLV timecode
    LdDecodeMetaData::ClvTimecode timecode = metadata.convertFrameNumberToClvTimecode(discFrame);

    // Generate a string containing the required number
    QString number = "00F" + QString("%1").arg(timecode.hours, 1, 10, QChar('0')) + "DD" +
            QString("%1").arg(timecode.minutes, 2, 10, QChar('0'));
    bool ok;
    qint32 returnValue = number.toInt(&ok, 16);
    if (!ok) returnValue = 0;

    return returnValue;
}

qint32 paddedAudioSamples(LdDecodeMetaData &capture, const QString &metadataFileName)
{
    const bool isPal = capture.getVideoParameters().system == PAL;
    // PAL: 44 100 samples a second, 50 fields a second; NTSC: 60000/1001 fields a second
    qint32 samples = isPal ? 882 : 736;

    // A CVBS capture's audio has the rate of its WAV file, not 44.1 kHz: a field's worth of it is the rate over the field rate
    if (capture.isCvbs()) {
        CvbsAudio::WavInfo wavInfo;
        const QString wavFileName = CvbsAudio::wavFileName(Cvbs::baseName(Cvbs::dataFileForMetadata(metadataFileName)), 0);
        if (QFileInfo::exists(wavFileName) && CvbsAudio::readWavInfo(wavFileName, wavInfo)) {
            samples = isPal ? qRound(wavInfo.sampleRate / 50.0) : qRound(wavInfo.sampleRate * 1001.0 / 60000.0);
        }
    }
    return samples;
}

std::shared_ptr<const QVector<qint32>> viewFields(LdDecodeMetaData &capture, const FrameMap &map, QString &error)
{
    capture.setIsFirstFieldFirst(!map.reverseFields);
    const qint32 frames = map.frameCount();
    auto table = std::make_shared<QVector<qint32>>(frames * 2, 0);
    for (qint32 place = 1; place <= frames; place++) {
        const MapFrame frame = map.frameAt(place);
        if (frame.captureFrame == 0) continue;
        const qint32 a = capture.getFirstFieldNumber(frame.captureFrame);
        const qint32 b = capture.getSecondFieldNumber(frame.captureFrame);
        if (a < 1 || b < 1) {
            error = QString("the map asks for frame %1 of the capture, which has no two fields there").arg(frame.captureFrame);
            return nullptr;
        }
        // (the copy keeps the order of the capture: the earlier field first)
        (*table)[(place - 1) * 2] = qMin(a, b);
        (*table)[(place - 1) * 2 + 1] = qMax(a, b);
    }
    return table;
}

std::shared_ptr<const QVector<qint32>> applyEmbeddedMap(LdDecodeMetaData &metadata, const QString &metadataFileName, const QString &videoFileName,
                                                       QString &error, bool measuredOnly, QVector<FieldAudio::Range> *audioRanges,
                                                       QVector<qint32> *captureFrames)
{
    if (!metadata.hasDiscMap()) return nullptr;
    const FrameMap map = metadata.getDiscMap();

    // The map is good for the capture it was made for
    if (map.captureSize > 0 && QFileInfo::exists(videoFileName) && QFileInfo(videoFileName).isFile() && QFileInfo(videoFileName).size() != map.captureSize) {
        error = QString("the map in the metadata of %1 was made for another capture (it is not of the same size)").arg(videoFileName);
        return nullptr;
    }

    LdDecodeMetaData view;
    if (!build(metadata, map, paddedAudioSamples(metadata, metadataFileName), view, error, measuredOnly)) return nullptr;
    const auto table = viewFields(metadata, map, error);
    if (!table) return nullptr;
    if (audioRanges) *audioRanges = FieldAudio::rangesOfView(metadata, *table, paddedAudioSamples(metadata, metadataFileName));
    if (captureFrames) {
        captureFrames->resize(map.frameCount());
        for (qint32 place = 1; place <= map.frameCount(); place++) (*captureFrames)[place - 1] = map.frameAt(place).captureFrame;
    }
    metadata.adoptView(view);
    return table;
}

bool build(LdDecodeMetaData &capture, const FrameMap &map, qint32 paddedAudioSamples, LdDecodeMetaData &target, QString &error, bool measuredOnly)
{
    const bool isDiscCav = map.discType != "CLV";
    capture.setIsFirstFieldFirst(!map.reverseFields);

    LdDecodeMetaData::VideoParameters videoParameters = capture.getVideoParameters();

    // Indicate that the source has been mapped
    videoParameters.isMapped = true;
    target.setVideoParameters(videoParameters);

    // Store the PCM audio parameters
    if (capture.hasPcmAudioParameters()) {
        target.setPcmAudioParameters(capture.getPcmAudioParameters());
    }

    // Set the number of sequential fields
    const qint32 frameCount = map.frameCount();
    target.setNumberOfFields(frameCount * 2);

    // Make a VBI Decoder object for verifying generated VBI
    VbiDecoder vbiDecoder;

    for (qint32 place = 1; place <= frameCount; place++) {
        const MapFrame frame = map.frameAt(place);
        if (frame.captureFrame != 0) {
            // Normal frame - get the data from the source video
            const qint32 firstFieldNumber = capture.getFirstFieldNumber(frame.captureFrame);
            const qint32 secondFieldNumber = capture.getSecondFieldNumber(frame.captureFrame);
            if (firstFieldNumber < 1 || secondFieldNumber < 1) {
                error = QString("the map asks for frame %1 of the capture, which has no two fields there").arg(frame.captureFrame);
                return false;
            }

            // Get the source metadata for the fields
            LdDecodeMetaData::Field firstSourceMetadata = capture.getField(firstFieldNumber);
            LdDecodeMetaData::Field secondSourceMetadata = capture.getField(secondFieldNumber);

            // Generate new VBI data for the frame
            if (isDiscCav) {
                // Disc is CAV - add a frame number
                // The frame number is hex 0xF12345 (where 1,2,3,4,5 are hex digits 0-9)
                // inserted into VBI lines 17 and 18 of the first field
                if (!firstSourceMetadata.vbi.inUse) {
                    firstSourceMetadata.vbi.inUse = true;
                    firstSourceMetadata.vbi.vbiData[0] = 0;
                }

                firstSourceMetadata.vbi.vbiData[1] = frameToVbi(frame.discFrame);
                firstSourceMetadata.vbi.vbiData[2] = frameToVbi(frame.discFrame);

                // Note: Because only 2 lines of VBI are replaced here, its possible that corruption
                // in the unmodified line causes the resulting VBI to be invalid - so we need to check
                // for that here
                VbiDecoder::Vbi vbi = vbiDecoder.decodeFrame(
                        firstSourceMetadata.vbi.vbiData[0], firstSourceMetadata.vbi.vbiData[1], firstSourceMetadata.vbi.vbiData[2],
                        secondSourceMetadata.vbi.vbiData[0], secondSourceMetadata.vbi.vbiData[1], secondSourceMetadata.vbi.vbiData[2]);

                if (vbi.picNo != frame.discFrame) {
                    qInfo() << "Warning: Updated VBI frame number for frame" << frame.discFrame << "has been corrupted by existing VBI data - overwriting all VBI for frame";
                    firstSourceMetadata.vbi.vbiData[0] = 0;
                }
            } else {
                // Disc is CLV - add a timecode
                if (!firstSourceMetadata.vbi.inUse) {
                    firstSourceMetadata.vbi.inUse = true;
                }
                firstSourceMetadata.vbi.vbiData[0] = frameToClvPicNo(capture, frame.discFrame);
                firstSourceMetadata.vbi.vbiData[1] = frameToClvTimeCode(capture, frame.discFrame);
                firstSourceMetadata.vbi.vbiData[2] = frameToClvTimeCode(capture, frame.discFrame);
            }

            // A frame that could not be numbered has a dropout over the whole of each field: the stack leaves it out, as it does a dropout
            const bool notMeasured = measuredOnly && (frame.basis == MapBasis::Unconfirmed || frame.basis == MapBasis::Sequence || frame.basis == MapBasis::Guessed);
            if (frame.unusable || notMeasured) {
                for (LdDecodeMetaData::Field *field: { &firstSourceMetadata, &secondSourceMetadata }) {
                    field->dropOuts.clear();
                    for (qint32 line = 1; line <= videoParameters.fieldHeight; line++) field->dropOuts.append(0, videoParameters.fieldWidth, line);
                }
            }

            // Append the fields to the metadata
            target.appendField(firstSourceMetadata);
            target.appendField(secondSourceMetadata);
        } else {
            // Padded frame metadata

            // Generate dummy target field metadata
            LdDecodeMetaData::Field firstSourceMetadata;
            LdDecodeMetaData::Field secondSourceMetadata;
            firstSourceMetadata.isFirstField = true;
            secondSourceMetadata.isFirstField = false;
            firstSourceMetadata.pad = true;
            secondSourceMetadata.pad = true;

            // Generate VBI data for the padded (dummy) output frame
            // Also add the padded size of the audio sample data
            firstSourceMetadata.vbi.inUse = true;
            if (isDiscCav) {
                firstSourceMetadata.vbi.vbiData[0] = 0;
                firstSourceMetadata.vbi.vbiData[1] = frameToVbi(frame.discFrame);
                firstSourceMetadata.vbi.vbiData[2] = frameToVbi(frame.discFrame);
            } else {
                firstSourceMetadata.vbi.vbiData[0] = frameToClvPicNo(capture, frame.discFrame);
                firstSourceMetadata.vbi.vbiData[1] = frameToClvTimeCode(capture, frame.discFrame);
                firstSourceMetadata.vbi.vbiData[2] = frameToClvTimeCode(capture, frame.discFrame);
            }
            firstSourceMetadata.audioSamples = paddedAudioSamples;

            secondSourceMetadata.vbi.inUse = true;
            secondSourceMetadata.vbi.vbiData[0] = 0;
            secondSourceMetadata.vbi.vbiData[1] = 0;
            secondSourceMetadata.vbi.vbiData[2] = 0;
            secondSourceMetadata.audioSamples = paddedAudioSamples;

            // Append the fields to the metadata
            target.appendField(firstSourceMetadata);
            target.appendField(secondSourceMetadata);
        }
    }

    return true;
}

}
