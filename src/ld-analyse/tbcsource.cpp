/******************************************************************************
 * tbcsource.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2021-2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <limits>

#include "tbcsource.h"
#include "stackproject.h"
#include "ldxcapture.h"
#include "ldxstream.h"
#include "cvbs.h"
#include "tbcfiles.h"
#include "tbc/logging.h"

#include "sourcefield.h"

TbcSource::TbcSource(QObject *parent) : QObject(parent)
{
    resetState();

    // Configure the chroma decoder
    palConfiguration = palColour.getConfiguration();
    palConfiguration.chromaFilter = PalColour::transform2DFilter;
    ntscConfiguration = ntscColour.getConfiguration();
    outputConfiguration.pixelFormat = OutputWriter::PixelFormat::RGB48;
    outputConfiguration.paddingAmount = 1;
}

// Public methods -----------------------------------------------------------------------------------------------------

// Method to load a TBC source file
void TbcSource::loadSource(QString sourceFilename)
{
    resetState();

    // Set the current file name
    QFileInfo inFileInfo(sourceFilename);
    currentSourceFilename = inFileInfo.fileName();
    tbcDebugStream() << "TbcSource::loadSource(): Opening TBC source file:" << currentSourceFilename;

    // Set up and fire-off background loading thread
    tbcDebugStream() << "TbcSource::loadSource(): Setting up background loader thread";
    disconnect(&watcher, &QFutureWatcher<bool>::finished, nullptr, nullptr);
    connect(&watcher, &QFutureWatcher<bool>::finished, this, &TbcSource::finishBackgroundLoad);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    future = QtConcurrent::run(this, &TbcSource::startBackgroundLoad, sourceFilename);
#else
    future = QtConcurrent::run(&TbcSource::startBackgroundLoad, this, sourceFilename);
#endif
    watcher.setFuture(future);
}

// Method to unload a TBC source file
void TbcSource::unloadSource()
{
    liveSnr.reset();
    fileSnr.reset();
    sourceVideo.close();
    if (sourceMode != ONE_SOURCE) chromaSourceVideo.close();
    resetState();
}

// Start saving the metadata file for the current source
void TbcSource::saveSourceMetadata()
{
    // Start a background saving thread
    tbcDebugStream() << "TbcSource::saveSourceMetadata(): Starting background save thread";
    disconnect(&watcher, &QFutureWatcher<bool>::finished, nullptr, nullptr);
    connect(&watcher, &QFutureWatcher<bool>::finished, this, &TbcSource::finishBackgroundSave);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    future = QtConcurrent::run(this, &TbcSource::startBackgroundSave, currentMetadataFilename);
#else
    future = QtConcurrent::run(&TbcSource::startBackgroundSave, this, currentMetadataFilename);
#endif
    watcher.setFuture(future);
}

// Method returns true is a TBC source is loaded
bool TbcSource::getIsSourceLoaded()
{
    return sourceReady;
}

// Method returns true if the source is a stacking session
bool TbcSource::getIsStack()
{
    return sourceReady && StackProject::isSession(currentMetadataFilename);
}

// What a stacking session is made of
QString TbcSource::getStackDescription()
{
    if (!getIsStack()) return QString();
    StackProject session;
    QString error;
    if (!session.load(currentMetadataFilename, error)) return QString();
    qint32 on = 0;
    for (const StackSource &source: session.sources) {
        if (source.enabled) on++;
    }
    const bool cvbs = Cvbs::isDataFileName(StackProject::referenceFile(currentMetadataFilename));
    return QObject::tr("%1 sources, %2").arg(on).arg(cvbs ? "CVBS" : "TBC");
}

// Method returns the filename of the current TBC source
QString TbcSource::getCurrentSourceFilename()
{
    if (!sourceReady) return QString();

    return currentSourceFilename;
}

// Method returns the filename of the metadata loaded with the current source
QString TbcSource::getCurrentMetadataFilename()
{
    if (!sourceReady) return QString();

    return currentMetadataFilename;
}

// Method returns the format of the loaded metadata, for display
QString TbcSource::getMetadataFormatName()
{
    if (!sourceReady) return QString();

    // A stacking session: the stack has the metadata of its first source
    if (StackProject::isSession(currentMetadataFilename)) return QStringLiteral("of the first source of the stack");
    if (LdxCapture::isLdxName(currentMetadataFilename)) return QStringLiteral("of the capture, in its LDX file");

    const QString format = ldDecodeMetaData.getFormat() == LdDecodeMetaData::MetadataFormat::Json ? QStringLiteral("JSON")
                                                                                                   : QStringLiteral("SQLite");
    if (!ldDecodeMetaData.isCvbs()) return format;

    // A CVBS capture: its core metadata alone, or with the companion
    if (currentMetadataFilename.endsWith(".meta", Qt::CaseInsensitive)) return QStringLiteral("CVBS");
    return QStringLiteral("CVBS + ") + format;
}

// Return a description of the last IO error
QString TbcSource::getLastIOError()
{
    return lastIOError;
}

// Copy the settings that decide what an image looks like
TbcSource::RenderSettings TbcSource::getRenderSettings()
{
    RenderSettings settings;
    settings.sourceFilename = currentSourceFilename;
    settings.chromaFilename = currentChromaFilename;
    settings.metadataFilename = currentMetadataFilename;
    settings.palConfiguration = palConfiguration;
    settings.ntscConfiguration = ntscConfiguration;
    settings.monoConfiguration = monoConfiguration;
    settings.outputConfiguration = outputConfiguration;
    settings.videoParameters = ldDecodeMetaData.getVideoParameters();
    settings.chromaOn = chromaOn;
    settings.dropoutsOn = dropoutsOn;
    settings.reverseFoOn = reverseFoOn;
    settings.stretchFieldOn = stretchFieldOn;
    settings.combine = combine;
    settings.viewMode = viewMode;
    settings.isLive = isLive;
    settings.streamInput = streamInput;
    settings.sourceMode = sourceMode;
    return settings;
}

// Open the same files as another TbcSource, to render images beside it. This reads the
// metadata again, so it is slow, and is meant to be called once, from the thread that
// will do the rendering.
bool TbcSource::openForRendering(const RenderSettings &settings, const TbcSource *liveParent)
{
    if (settings.isLive) {
        // A stream: made-up metadata as the source has, and the same stream to read
        if (!liveParent) return false;
        streamInput = settings.streamInput;
        ldDecodeMetaData.initialiseDefault(streamInput.system, streamInput.blackLevel, streamInput.whiteLevel,
                                           streamInput.isSubcarrierLocked);
    if (LdxStream::instance()) LdxStream::instance()->provideFields(ldDecodeMetaData);
        ldDecodeMetaData.setVideoParameters(settings.videoParameters);
        if (!sourceVideo.shareStreamWith(liveParent->sourceVideo)) return false;
        isLive = true;
        currentSourceFilename = settings.sourceFilename;
        currentChromaFilename.clear();
        currentMetadataFilename.clear();
        sourceReady = true;
        applyRenderSettings(settings);
        return true;
    }

    if (!ldDecodeMetaData.read(settings.metadataFilename)) return false;
    ldDecodeMetaData.setVideoParameters(settings.videoParameters);

    const LdDecodeMetaData::VideoParameters &videoParameters = ldDecodeMetaData.getVideoParameters();
    const qint32 fieldLength = videoParameters.fieldWidth * videoParameters.fieldHeight;
    if (!sourceVideo.open(settings.sourceFilename, fieldLength)) return false;
    if (!settings.chromaFilename.isEmpty() && !chromaSourceVideo.open(settings.chromaFilename, fieldLength)) {
        sourceVideo.close();
        return false;
    }

    currentSourceFilename = settings.sourceFilename;
    currentChromaFilename = settings.chromaFilename;
    currentMetadataFilename = settings.metadataFilename;
    sourceReady = true;
    applyRenderSettings(settings);
    return true;
}

// Make this TbcSource render images as another one does
void TbcSource::applyRenderSettings(const RenderSettings &settings)
{
    palConfiguration = settings.palConfiguration;
    ntscConfiguration = settings.ntscConfiguration;
    monoConfiguration = settings.monoConfiguration;
    outputConfiguration = settings.outputConfiguration;
    chromaOn = settings.chromaOn;
    dropoutsOn = settings.dropoutsOn;
    reverseFoOn = settings.reverseFoOn;
    stretchFieldOn = settings.stretchFieldOn;
    combine = settings.combine;
    viewMode = settings.viewMode;
    sourceMode = settings.sourceMode;

    ldDecodeMetaData.setVideoParameters(settings.videoParameters);
    applyFieldOrder();
    configureChromaDecoder();

    // Whatever was loaded was loaded with other settings
    loadedFrameNumber = -1;
    invalidateImageCache();
}

void TbcSource::setPrerenderedImage(qint32 position, const QImage &image)
{
    prerenderedImage = image;
    prerenderedPosition = position;
    prerenderedGeneration = settingsGeneration;
}

// Method to set the highlight dropouts mode (true = dropouts highlighted)
void TbcSource::setHighlightDropouts(bool _state)
{
    settingsGeneration++;
    invalidateImageCache();
    dropoutsOn = _state;
}

// Method to set the chroma decoder mode (true = on)
void TbcSource::setChromaDecoder(bool _state)
{
    settingsGeneration++;
    invalidateImageCache();
    chromaOn = _state;
}

// Method to set the view mode
void TbcSource::setViewMode(ViewMode _viewMode)
{
    settingsGeneration++;
    invalidateImageCache();
    viewMode = _viewMode;
}

// Method to set stretch field mode (true = on)
void TbcSource::setStretchField(bool _stretch)
{
    settingsGeneration++;
    invalidateImageCache();
    stretchFieldOn = _stretch;
}

// Method to set the field order (true = reversed, false = normal)
void TbcSource::setFieldOrder(bool _state)
{
    settingsGeneration++;
    invalidateImageCache();
    reverseFoOn = _state;

    applyFieldOrder();
}

// Put the field order in the metadata. A live input has fields made up (alternating first and second),
// for which reversing is pairing them one off; a file has its own flags, and the order is the expectation.
void TbcSource::applyFieldOrder()
{
    if (ldDecodeMetaData.isGenerated()) {
        ldDecodeMetaData.setGeneratedFieldOrder(reverseFoOn);
        ldDecodeMetaData.setIsFirstFieldFirst(true);
    } else {
        ldDecodeMetaData.setIsFirstFieldFirst(!reverseFoOn);
    }
}

// Method to set if we combine source
void TbcSource::setCombine(bool _state)
{
    settingsGeneration++;
	combine = _state;
}

// Method to get the state of the highlight dropouts mode
bool TbcSource::getHighlightDropouts()
{
    return dropoutsOn;
}

// Method to get the state of the chroma decoder mode
bool TbcSource::getChromaDecoder()
{
    return chromaOn;
}

// Method to get the view mode
TbcSource::ViewMode TbcSource::getViewMode()
{
    return viewMode;
}

// Method to determine if frame view is enabled
bool TbcSource::getFrameViewEnabled()
{
    return viewMode == ViewMode::FRAME_VIEW;
}

// Method to determine if field view is enabled
bool TbcSource::getFieldViewEnabled()
{
    return viewMode == ViewMode::FIELD_VIEW;
}

// Method to determine if split view is enabled
bool TbcSource::getSplitViewEnabled()
{
    return viewMode == ViewMode::SPLIT_VIEW;
}

// Method to get the state of the stretch field mode
bool TbcSource::getStretchField()
{
    return stretchFieldOn;
}

// Method to get the field order
bool TbcSource::getFieldOrder()
{
    return reverseFoOn;
}

// Return the source mode
TbcSource::SourceMode TbcSource::getSourceMode()
{
    return sourceMode;
}

// Set the source mode
void TbcSource::setSourceMode(TbcSource::SourceMode _sourceMode)
{
    settingsGeneration++;
    if (sourceMode == ONE_SOURCE) return;

    invalidateImageCache();
    sourceMode = _sourceMode;
}

// Load the metadata for a field/frame
void TbcSource::load(qint32 frameNumber, qint32 fieldNumber)
{
    loadedFieldNumber = fieldNumber;

    // If there's no source, or we've already loaded that frame, nothing to do
    if (!sourceReady || loadedFrameNumber == frameNumber) return;

    // A stream has fields made up for the frames it holds
    if (isLive) ldDecodeMetaData.ensureFields(ldDecodeMetaData.fieldsForFrame(frameNumber));
    loadedFrameNumber = frameNumber;
    inputFieldsValid = false;
    invalidateImageCache();

    // Get the required field numbers
    firstFieldNumber = ldDecodeMetaData.getFirstFieldNumber(frameNumber);
    secondFieldNumber = ldDecodeMetaData.getSecondFieldNumber(frameNumber);

    // Make sure we have a valid response from the frame determination
    if (firstFieldNumber == -1 || secondFieldNumber == -1) {
        qCritical() << "Could not determine field numbers!";

        // Jump back one frame
        if (frameNumber != 1) {
            frameNumber--;

            firstFieldNumber = ldDecodeMetaData.getFirstFieldNumber(frameNumber);
            secondFieldNumber = ldDecodeMetaData.getSecondFieldNumber(frameNumber);
        }
        tbcDebugStream() << "TbcSource::load(): Jumping back one frame due to error";
    }

    // A live input has no VBI or VITS metadata: work it out from the fields
    if (liveAnalyse) analyseLiveFrame(frameNumber);

    // Get the field metadata
    firstField = ldDecodeMetaData.getField(firstFieldNumber);
    secondField = ldDecodeMetaData.getField(secondFieldNumber);
}

// Method to get a QImage from a field or frame number
QImage TbcSource::getImage()
{
    if ((getFieldViewEnabled() ? loadedFieldNumber : loadedFrameNumber) == -1) return QImage();
    // Rendered ahead by playback, in another thread, with the settings now in force
    if (prerenderedPosition == (getFieldViewEnabled() ? loadedFieldNumber : loadedFrameNumber)
        && prerenderedGeneration == settingsGeneration && !prerenderedImage.isNull()) {
        if (!getFieldViewEnabled()) {
            cache = prerenderedImage;
            cacheValid = true;
        }
        return prerenderedImage;
    }


    // Check cached QImage
    if (!getFieldViewEnabled() && cacheValid) {
        return cache;
    }

    // Get a QImage for the output
    auto outputImage = generateQImage();

    // Highlight dropouts
    if (dropoutsOn) {
        // Create a painter object
        QPainter imagePainter(&outputImage);

        // Get the metadata for the video parameters
        LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();

        // Calculate the frame height
        const auto frameHeight = (videoParameters.fieldHeight * 2) - 1;
        const auto fieldN = getFieldViewEnabled() ? 1 : 2;

        // This will run once for field view and twice for frame/split view
        for (auto i = 0; i < fieldN; i++) {
            // Set current field based on loop iteration or field number
            auto isFirstField = getViewMode() == ViewMode::FIELD_VIEW ? loadedFieldNumber % 2 != 0 : i == 0;
            auto currentField = isFirstField ? &firstField : &secondField;
            imagePainter.setPen(isFirstField ? Qt::red : Qt::blue);

            // Draw the drop out data for the current field
            for (auto dropOutIndex = 0; dropOutIndex < currentField->dropOuts.size(); dropOutIndex++) {
                const auto startx = currentField->dropOuts.startx(dropOutIndex);
                const auto endx = currentField->dropOuts.endx(dropOutIndex);
                const auto fieldLine = currentField->dropOuts.fieldLine(dropOutIndex);

                switch (getViewMode()) {
                    case ViewMode::FRAME_VIEW: {
                        qint32 lineY;
                        if (isFirstField) {
                            lineY = (fieldLine - 1) * 2;
                        } else {
                            lineY = (fieldLine * 2) - 1;
                        }

                        imagePainter.drawLine(startx, lineY, endx, lineY);
                        break;
                    }

                    case ViewMode::SPLIT_VIEW: {
                        qint32 lineY;
                        if (isFirstField) {
                            lineY = fieldLine - 1;
                        } else {
                            lineY = fieldLine + (frameHeight / 2);
                        }

                        imagePainter.drawLine(startx, lineY, endx, lineY);
                        break;
                    }

                    case ViewMode::FIELD_VIEW: {
                        // Draw line off-center if 1:1, else double lines
                        if (getStretchField()) {
                            qint32 lineY = fieldLine - 1;

                            imagePainter.drawLine(startx, lineY * 2, endx, lineY * 2);
                            imagePainter.drawLine(startx, lineY * 2 + 1, endx, lineY * 2 + 1);
                        } else {
                            qint32 lineY = fieldLine - 1 + (frameHeight / 4);

                            imagePainter.drawLine(startx, lineY, endx, lineY);
                        }
                        break;
                    }
                }
            }
        }

        // End the painter object
        imagePainter.end();
    }

    cache = outputImage;
    cacheValid = true;

    return outputImage;
}

// Method to get the number of available frames
qint32 TbcSource::getNumberOfFrames()
{
    if (!sourceReady) return 0;
    if (isLive) return liveFrozen ? frozenLastFrame : liveFrames;
    return ldDecodeMetaData.getNumberOfFrames();
}

// Method to get the number of available fields
qint32 TbcSource::getNumberOfFields()
{
    if (!sourceReady) return 0;
    if (isLive) return 2 * (liveFrozen ? frozenLastFrame : liveFrames);
    return ldDecodeMetaData.getNumberOfFields();
}

// Method returns true if the TBC source is anamorphic (false for 4:3)
bool TbcSource::getIsWidescreen()
{
    if (!sourceReady) return false;
    return ldDecodeMetaData.getVideoParameters().isWidescreen;
}

// Return the source's VideoSystem
VideoSystem TbcSource::getSystem()
{
    if (!sourceReady) return NTSC;
    return ldDecodeMetaData.getVideoParameters().system;
}

// Return the source's VideoSystem description
QString TbcSource::getSystemDescription()
{
    if (!sourceReady) return "None";
    return ldDecodeMetaData.getVideoSystemDescription();
}

// Method to get the frame height in scanlines
qint32 TbcSource::getFrameHeight()
{
    if (!sourceReady) return 0;

    // Get the metadata for the fields
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();

    // Calculate the frame height
    return (videoParameters.fieldHeight * 2) - 1;
}

// Method to get the frame width in dots
qint32 TbcSource::getFrameWidth()
{
    if (!sourceReady) return 0;

    // Get the metadata for the fields
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();

    // Return the frame width
    return (videoParameters.fieldWidth);
}

// Get black SNR data for graphing
QVector<double> TbcSource::getBlackSnrGraphData()
{
    return blackSnrGraphData;
}

// Get white SNR data for graphing
QVector<double> TbcSource::getWhiteSnrGraphData()
{
    return whiteSnrGraphData;
}

// Get dropout data for graphing
QVector<double> TbcSource::getDropOutGraphData()
{
    return dropoutGraphData;
}

// Get visible dropout data for graphing
QVector<double> TbcSource::getVisibleDropOutGraphData()
{
    return visibleDropoutGraphData;
}

// Method to get the size of the graphing data
qint32 TbcSource::getGraphDataSize()
{
    // All data vectors are the same size, just return the size on one
    return dropoutGraphData.size();
}

// Method returns true if frame contains dropouts
bool TbcSource::getIsDropoutPresent()
{
    if (loadedFrameNumber == -1) return false;

    if (firstField.dropOuts.size() > 0) return true;
    if (secondField.dropOuts.size() > 0) return true;
    return false;
}

// Get the decoded ComponentFrame for the current frame
const ComponentFrame &TbcSource::getComponentFrame()
{
    // Load and decode SourceFields for the current frame
    loadInputFields();
    decodeFrame();

    return componentFrames[0];
}

// Get the VideoParameters for the current source
const LdDecodeMetaData::VideoParameters &TbcSource::getVideoParameters()
{
    return ldDecodeMetaData.getVideoParameters();
}

// Update the VideoParameters for the current source
void TbcSource::setVideoParameters(const LdDecodeMetaData::VideoParameters &videoParameters)
{
    settingsGeneration++;
    invalidateImageCache();

    // Update the metadata
    ldDecodeMetaData.setVideoParameters(videoParameters);

    // (the SNR is not measured again: it is against the levels of the capture, see snrParameters)
    if (liveAnalyse) {
        // The VBI is decoded again, as its decoders use the levels
        liveAnalysedFrames.clear();
        loadedFrameNumber = -1;
    }

    // Reconfigure the chroma decoder
    configureChromaDecoder();
}

// Get scan line data from the field/frame
TbcSource::ScanLineData TbcSource::getScanLineData(qint32 scanLine)
{
    if (loadedFrameNumber == -1) return ScanLineData();

    ScanLineData scanLineData;
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();
    auto frameLine = 0;
    bool isFirstField = true;

    switch (getViewMode()) {
        case ViewMode::FRAME_VIEW: {
            frameLine = scanLine;
            isFirstField = (scanLine % 2) == 0;
            break;
        }

        case ViewMode::SPLIT_VIEW: {
            if (scanLine <= videoParameters.fieldHeight) {
                isFirstField = true;
                frameLine = scanLine;

                // Offset for LineNumber
                scanLine = (scanLine * 2) - 1;
            } else {
                isFirstField = false;
                frameLine = scanLine - videoParameters.fieldHeight;

                // Offset for LineNumber
                scanLine = (scanLine - videoParameters.fieldHeight) * 2;
            }
            break;
        }

        case ViewMode::FIELD_VIEW: {
            isFirstField = loadedFieldNumber % 2 != 0;

            // Ensure frameLine accounts for fields and duplicated lines
            if (getStretchField()) {
                frameLine = (scanLine + 1) / 2;

                // Offset for LineNumber
                if (scanLine % 2 == 0 && isFirstField && scanLine > 1) scanLine--;
                if (scanLine % 2 != 0 && !isFirstField) scanLine++;
            } else {
                // Return if coords in unused area
                const auto startOffset = getFrameHeight() / 4;

                if (scanLine <= startOffset || scanLine > startOffset + videoParameters.fieldHeight) {
                    return ScanLineData();
                }

                frameLine = scanLine - startOffset;

                // Offset for LineNumber
                if (isFirstField) scanLine = (frameLine * 2) - 1;
                if (!isFirstField) scanLine = (frameLine * 2);
            }

            break;
        }
    }

    // Set the system and line number
    scanLineData.systemDescription = ldDecodeMetaData.getVideoSystemDescription();
    scanLineData.lineNumber = LineNumber::fromFrame1(scanLine, videoParameters.system);
    const LineNumber &lineNumber = scanLineData.lineNumber;

    // Set the video parameters
    scanLineData.blackIre = videoParameters.black16bIre;
    scanLineData.whiteIre = videoParameters.white16bIre;
    scanLineData.fieldWidth = videoParameters.fieldWidth;
    scanLineData.colourBurstStart = videoParameters.colourBurstStart;
    scanLineData.colourBurstEnd = videoParameters.colourBurstEnd;
    scanLineData.activeVideoStart = videoParameters.activeVideoStart;
    scanLineData.activeVideoEnd = videoParameters.activeVideoEnd;

    // Is this line part of the active region?
    scanLineData.isActiveLine = (frameLine - 1) >= videoParameters.firstActiveFrameLine
                                && (frameLine -1) < videoParameters.lastActiveFrameLine;

    // Get the field video and dropout data
    // Put chroma in "composite" field if using Y/C input.
    const SourceVideo::Data &fieldData = isFirstField ? inputFields[inputStartIndex].data
                            : inputFields[inputStartIndex + 1].data;
    const ComponentFrame &componentFrame = getComponentFrame();
    DropOuts &dropouts = isFirstField ? firstField.dropOuts : secondField.dropOuts;

    scanLineData.composite.resize(videoParameters.fieldWidth);
    scanLineData.luma.resize(videoParameters.fieldWidth);
    scanLineData.isDropout.resize(videoParameters.fieldWidth);


    for (qint32 xPosition = 0; xPosition < videoParameters.fieldWidth; xPosition++) {
        // Get the 16-bit composite value for the current pixel (frame data is numbered 0-624 or 0-524)
        scanLineData.composite[xPosition] = fieldData[(lineNumber.field0() * videoParameters.fieldWidth) + xPosition];

        // Get the decoded luma value for the current pixel (only computed in the active region)
        scanLineData.luma[xPosition] = static_cast<qint32>(componentFrame.y(frameLine - 1)[xPosition]);

        scanLineData.isDropout[xPosition] = false;
        for (qint32 doCount = 0; doCount < dropouts.size(); doCount++) {
            if (dropouts.fieldLine(doCount) == lineNumber.field1()) {
                if (xPosition >= dropouts.startx(doCount) && xPosition <= dropouts.endx(doCount)) scanLineData.isDropout[xPosition] = true;
            }
        }
    }

    if (sourceMode == BOTH_SOURCES) {
        const auto &chromaFieldData = isFirstField ? chromaInputFields[inputStartIndex].data
                     : chromaInputFields[inputStartIndex + 1].data;
        // We need to offset by half a word since the input is 16-bit unsigned.
        const short offset = std::numeric_limits<int16_t>::min();
        scanLineData.chroma.resize(videoParameters.fieldWidth);
        for (qint32 xPosition = 0; xPosition < videoParameters.fieldWidth; xPosition++) {
            scanLineData.chroma[xPosition] = chromaFieldData[(lineNumber.field0() * videoParameters.fieldWidth) + xPosition] + offset;
            // Combine Y and C to "simulate" composite for the scope.
            scanLineData.composite[xPosition] += scanLineData.chroma[xPosition];
        }

    }

    return scanLineData;
}

// Method to return the decoded VBI data for the frame
VbiDecoder::Vbi TbcSource::getFrameVbi()
{
    if (loadedFrameNumber == -1) return VbiDecoder::Vbi();

    return vbiDecoder.decodeFrame(firstField.vbi.vbiData[0], firstField.vbi.vbiData[1], firstField.vbi.vbiData[2],
                                  secondField.vbi.vbiData[0], secondField.vbi.vbiData[1], secondField.vbi.vbiData[2]);
}

// Method returns true if the VBI is valid for the frame
bool TbcSource::getIsFrameVbiValid()
{
    if (loadedFrameNumber == -1) return false;

    if (firstField.vbi.vbiData[0] == -1 || firstField.vbi.vbiData[1] == -1 || firstField.vbi.vbiData[2] == -1) return false;
    if (secondField.vbi.vbiData[0] == -1 || secondField.vbi.vbiData[1] == -1 || secondField.vbi.vbiData[2] == -1) return false;

    return true;
}

// Method to return the decoded VIDEO ID data for the frame
VideoIdDecoder::VideoId TbcSource::getFrameVideoId()
{
    if (loadedFrameNumber == -1) return VideoIdDecoder::VideoId();

    return videoIdDecoder.decodeFrame(firstField.ntsc.videoIdData, secondField.ntsc.videoIdData);
}

// Method returns true if the VIDEO ID is present for the frame
bool TbcSource::getIsFrameVideoIdValid()
{
    if (loadedFrameNumber == -1) return false;

    if (!firstField.ntsc.isVideoIdDataValid || !secondField.ntsc.isVideoIdDataValid) return false;

    return true;
}

// Method to return the decoded VITC data for the frame
VitcDecoder::Vitc TbcSource::getFrameVitc()
{
    if (loadedFrameNumber == -1) return VitcDecoder::Vitc();

    const VideoSystem system = ldDecodeMetaData.getVideoParameters().system;
    if (firstField.vitc.inUse) return vitcDecoder.decode(firstField.vitc.vitcData, system);
    if (secondField.vitc.inUse) return vitcDecoder.decode(secondField.vitc.vitcData, system);

    return VitcDecoder::Vitc();
}

// Method returns true if the VITC is valid for the frame
bool TbcSource::getIsFrameVitcValid()
{
    if (loadedFrameNumber == -1) return false;

    return firstField.vitc.inUse || secondField.vitc.inUse;
}

// Method to get the field number of the first field of the frame
qint32 TbcSource::getFirstFieldNumber()
{
    if (loadedFrameNumber == -1) return 0;
    return firstFieldNumber;
}

// Method to get the field number of the second field of the frame
qint32 TbcSource::getSecondFieldNumber()
{
    if (loadedFrameNumber == -1) return 0;
    return secondFieldNumber;
}

qint32 TbcSource::getCcData0()
{
    if (loadedFrameNumber == -1) return 0;

    if (firstField.closedCaption.data0 != -1) return firstField.closedCaption.data0;
    return secondField.closedCaption.data0;
}

qint32 TbcSource::getCcData1()
{
    if (loadedFrameNumber == -1) return 0;

    if (firstField.closedCaption.data1 != -1) return firstField.closedCaption.data1;
    return secondField.closedCaption.data1;
}

void TbcSource::setChromaConfiguration(const PalColour::Configuration &_palConfiguration,
                                       const Comb::Configuration &_ntscConfiguration,
                                       const OutputWriter::Configuration &_outputConfiguration)
{
    settingsGeneration++;
    invalidateImageCache();

    palConfiguration = _palConfiguration;
    ntscConfiguration = _ntscConfiguration;
    outputConfiguration = _outputConfiguration;

    configureChromaDecoder();
}

const PalColour::Configuration &TbcSource::getPalConfiguration()
{
    return palConfiguration;
}

const Comb::Configuration &TbcSource::getNtscConfiguration()
{
    return ntscConfiguration;
}

const MonoDecoder::MonoConfiguration &TbcSource::getMonoConfiguration()
{
    return monoConfiguration;
}

const OutputWriter::Configuration &TbcSource::getOutputConfiguration()
{
    return outputConfiguration;
}

// Return the frame number of the start of the next chapter
qint32 TbcSource::startOfNextChapter(qint32 currentFrameNumber)
{
    // Do we have a chapter map?
    if (chapterMap.size() == 0) return getNumberOfFrames();

    qint32 mapLocation = -1;
    for (qint32 i = 0; i < chapterMap.size(); i++) {
        if (chapterMap[i] > currentFrameNumber) {
            mapLocation = i;
            break;
        }
    }

    // Found?
    if (mapLocation != -1) {
        return chapterMap[mapLocation];
    }

    return getNumberOfFrames();
}

// Return the frame number of the start of the current chapter
qint32 TbcSource::startOfChapter(qint32 currentFrameNumber)
{
    // Do we have a chapter map?
    if (chapterMap.size() == 0) return 1;

    qint32 mapLocation = -1;
    for (qint32 i = chapterMap.size() - 1; i >= 0; i--) {
        if (chapterMap[i] < currentFrameNumber) {
            mapLocation = i;
            break;
        }
    }

    // Found?
    if (mapLocation != -1) {
        return chapterMap[mapLocation];
    }

    return 1;
}


// Private methods ----------------------------------------------------------------------------------------------------

// Re-initialise state for a new source video
void TbcSource::resetState()
{
    liveSnr.reset();
    fileSnr.reset();
    snrParameters = LdDecodeMetaData::VideoParameters();
    // Default frame image options
    chromaOn = false;
    dropoutsOn = false;
    viewMode = ViewMode::FRAME_VIEW;
    reverseFoOn = false;
    sourceReady = false;
    sourceMode = ONE_SOURCE;
    liveFrozen = false;
    liveProtected = false;
    liveAnalyse = false;
    isLive = false;
    liveFrames = 0;
    liveFirstFrame = 1;
    liveEnded = false;
    currentChromaFilename.clear();
    prerenderedPosition = -1;

    // Cache state
    loadedFrameNumber = -1;
    loadedFieldNumber = -1;
    inputFieldsValid = false;
    decodedFrameValid = false;
    cacheValid = false;
}

// Mark any cached data for the current field/frame as invalid
void TbcSource::invalidateImageCache()
{
    // Note this includes the input fields, because the number of fields we
    // load depends on the decoder parameters
    inputFieldsValid = false;
    decodedFrameValid = false;
    cacheValid = false;
}

// Configure the chroma decoder for its settings and the VideoParameters
void TbcSource::configureChromaDecoder()
{
    // Configure the chroma decoder
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters(); 
	
    if (videoParameters.system == PAL || videoParameters.system == PAL_M) {
		monoConfiguration.yNRLevel = palConfiguration.yNRLevel;
        palColour.updateConfiguration(videoParameters, palConfiguration);
    } else {
		monoConfiguration.yNRLevel = ntscConfiguration.yNRLevel;
        ntscColour.updateConfiguration(videoParameters, ntscConfiguration);
    }
	monoDecoder.updateConfiguration(videoParameters, monoConfiguration);

    // Configure the OutputWriter.
    // Because we have padding disabled, this won't change the VideoParameters.
    outputWriter.updateConfiguration(videoParameters, outputConfiguration);
}

// Ensure the SourceFields for the current frame are loaded
void TbcSource::loadInputFields()
{
    if (inputFieldsValid) return;

    // Work out how many frames ahead/behind we need to fetch
    qint32 lookBehind, lookAhead;
    if (getSystem() == PAL || getSystem() == PAL_M) {
        lookBehind = palConfiguration.getLookBehind();
        lookAhead = palConfiguration.getLookAhead();
    } else {
        lookBehind = ntscConfiguration.getLookBehind();
        lookAhead = ntscConfiguration.getLookAhead();
    }

    if (sourceMode == CHROMA_SOURCE) {
        // Load chroma directly into inputFields
        SourceField::loadFields(chromaSourceVideo, ldDecodeMetaData,
                                loadedFrameNumber, 1, lookBehind, lookAhead,
                                inputFields, inputStartIndex, inputEndIndex);
    } else {
        // Load the only source, or luma, into inputFields
        SourceField::loadFields(sourceVideo, ldDecodeMetaData,
                                loadedFrameNumber, 1, lookBehind, lookAhead,
                                inputFields, inputStartIndex, inputEndIndex);
    }

    if (sourceMode == BOTH_SOURCES) {
        // Load chroma into chromaInputFields
        SourceField::loadFields(chromaSourceVideo, ldDecodeMetaData,
                                loadedFrameNumber, 1, lookBehind, lookAhead,
                                chromaInputFields, inputStartIndex, inputEndIndex);
		if(combine)
		{
			// Separate chroma is offset (see chroma_to_u16 in vhsdecode/chroma.py)
			static constexpr qint32 CHROMA_OFFSET = 32767;

			// Add chroma to luma, removing the offset
			for (qint32 fieldIndex = inputStartIndex; fieldIndex < inputEndIndex; fieldIndex++) {
				auto &sourceData = inputFields[fieldIndex].data;
				const auto &chromaData = chromaInputFields[fieldIndex].data;

				for (qint32 i = 0; i < sourceData.size(); i++) {
					qint32 sum = static_cast<qint32>(sourceData[i]) + static_cast<qint32>(chromaData[i]) - CHROMA_OFFSET;
					sourceData[i] = static_cast<quint16>(qBound(0, sum, 65535));
				}
			}
		}
    }

    inputFieldsValid = true;
}

// Ensure the current frame has been decoded
void TbcSource::decodeFrame()
{
    if (decodedFrameValid) return;

    loadInputFields();
	
	bool isMono = false;

    // Decode the current frame to components
    componentFrames.resize(1);
    yFrames.resize(1);
    cFrames.resize(1);
	if(!combine && sourceMode == BOTH_SOURCES)
	{
		monoDecoder.decodeFrames(inputFields, inputStartIndex, inputEndIndex, yFrames);
		if ((getSystem() == PAL || getSystem() == PAL_M) && palConfiguration.chromaFilter != PalColour::mono) {
			// PAL source
			palColour.decodeFrames(chromaInputFields, inputStartIndex, inputEndIndex, cFrames);
		} else if(getSystem() == NTSC && ntscConfiguration.dimensions != 0){
			// NTSC source
			ntscColour.decodeFrames(chromaInputFields, inputStartIndex, inputEndIndex, cFrames);
		}
		else
		{
			isMono = true;
		}
		
		for (qint32 fieldIndex = inputStartIndex, frameIndex = 0; fieldIndex < inputEndIndex; fieldIndex += 2, frameIndex++)
		{
			//isMono ? cFrames[frameIndex].init(ldDecodeMetaData.getVideoParameters(), false);
			componentFrames[frameIndex].init(ldDecodeMetaData.getVideoParameters(), false);
			componentFrames[frameIndex].setY(*yFrames[frameIndex].getY());
			if(!isMono){	
				componentFrames[frameIndex].setU(*cFrames[frameIndex].getU());
				componentFrames[frameIndex].setV(*cFrames[frameIndex].getV());
			}
		}
	}
	else
	{
		if (getSystem() == PAL || getSystem() == PAL_M) {
			if(palConfiguration.chromaFilter == palColour.ChromaFilterMode::mono){
				monoDecoder.decodeFrames(inputFields, inputStartIndex, inputEndIndex, componentFrames);
			} else {
				// PAL source
				palColour.decodeFrames(inputFields, inputStartIndex, inputEndIndex, componentFrames);
			}
		} else {
			if(ntscConfiguration.dimensions == 0){
				monoDecoder.decodeFrames(inputFields, inputStartIndex, inputEndIndex, componentFrames);
			} else {
				// NTSC source
				ntscColour.decodeFrames(inputFields, inputStartIndex, inputEndIndex, componentFrames);
			}
		}
	}

    decodedFrameValid = true;
}

// Method to create a QImage for a source video frame
QImage TbcSource::generateQImage()
{
    // Get the metadata for the video parameters
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();

    // Calculate the frame height
    const qint32 frameHeight = (videoParameters.fieldHeight * 2) - 1;
    const qint32 frameWidth = videoParameters.fieldWidth;

    // Set the frame image
    auto outputImage = QImage(frameWidth, frameHeight, QImage::Format_RGB32);

    // Fill the QImage with black
    outputImage.fill(Qt::black);

    // Create RGB32 data and set h/w + offsets
    QVector<QRgb> rgbData;
    qint32 inputHeight, inputWidth, fieldHeight, inputOffset, outputOffset;

    if (chromaOn) {
        // Show debug information
        if (getFieldViewEnabled()) {
            tbcDebugStream() << "TbcSource::generateQImage(): Generating a chroma image from field"
                             << loadedFieldNumber << "(" << videoParameters.fieldWidth << "x" << videoParameters.fieldHeight << ")";
        } else {
            tbcDebugStream() << "TbcSource::generateQImage(): Generating a chroma image from frame"
                             << loadedFrameNumber << "(" << videoParameters.fieldWidth << "x" << frameHeight << ")";
        }

        inputHeight = videoParameters.lastActiveFrameLine - videoParameters.firstActiveFrameLine;
        inputWidth = videoParameters.activeVideoEnd - videoParameters.activeVideoStart;
        fieldHeight = inputHeight / 2;
        inputOffset = videoParameters.firstActiveFrameLine;
        outputOffset = videoParameters.activeVideoStart;

        // Chroma decode the current frame
        decodeFrame();

        // Convert component video to RGB
        OutputFrame outputFrame;
        outputWriter.convert(componentFrames[0], outputFrame);

        const auto rgb48Ptr = reinterpret_cast<quint16 *>(outputFrame.data());
        rgbData.reserve(inputHeight * inputWidth * 3);

        // Create RGB32 from RGB48
        for (auto i = 0; i < inputHeight * inputWidth * 3; i += 3) {
            rgbData.push_back(qRgb(static_cast<qint32>(rgb48Ptr[i + 0] / 256),
                                   static_cast<qint32>(rgb48Ptr[i + 1] / 256),
                                   static_cast<qint32>(rgb48Ptr[i + 2] / 256)));
        }
    } else {
        // Show debug information
        if (getFieldViewEnabled()) {
            tbcDebugStream() << "TbcSource::generateQImage(): Generating a source image from field"
                             << loadedFieldNumber << "(" << videoParameters.fieldWidth << "x" << videoParameters.fieldHeight << ")";
        } else {
            tbcDebugStream() << "TbcSource::generateQImage(): Generating a source image from frame"
                             << loadedFrameNumber << "(" << videoParameters.fieldWidth << "x" << frameHeight << ")";
        }

        inputHeight = frameHeight;
        inputWidth = frameWidth;
        fieldHeight = videoParameters.fieldHeight;
        inputOffset = 0;
        outputOffset = 0;

        // Load SourceFields for the current frame
        loadInputFields();

        // Get pointers to the 16-bit greyscale data
        const quint16 *firstFieldPointer = inputFields[inputStartIndex].data.data();
        const quint16 *secondFieldPointer = inputFields[inputStartIndex + 1].data.data();
        rgbData.reserve(inputHeight * inputWidth * 3);

        // Create RGB32 from Gray16
        for (auto y = 0; y < fieldHeight * 2; y++) {
            auto *ptr = y % 2 == 0 ? firstFieldPointer : secondFieldPointer;

            for (auto x = 0; x < inputWidth; x++) {
                auto value = static_cast<qint32>(ptr[((y / 2) * inputWidth) + x] / 256);
                rgbData.push_back(qRgb(value, value, value));
            }
        }
    }

    // Copy RGB data to QImage
    switch (getViewMode()) {
        case ViewMode::FRAME_VIEW: {
            for (auto y = 0; y < inputHeight; y++) {
                auto *outputLine = reinterpret_cast<QRgb*>(outputImage.scanLine(y + inputOffset));
                std::copy_n(&rgbData[y * inputWidth], inputWidth, &outputLine[outputOffset]);
            }
            break;
        }

        case ViewMode::SPLIT_VIEW: {
            for (auto y = 0; y < inputHeight; y++) {
                const auto startOffset = (inputOffset / 2) + (y % 2 == 0 ? 0 : (frameHeight / 2) + 1);
                auto *outputLine = reinterpret_cast<QRgb*>(outputImage.scanLine((y / 2) + startOffset));
                std::copy_n(&rgbData[y * inputWidth], inputWidth, &outputLine[outputOffset]);
            }
            break;
        }

        case ViewMode::FIELD_VIEW: {
            auto startOffset = getStretchField() ? inputOffset : (frameHeight / 4) + (inputOffset / 2);
            auto height = getStretchField() ? inputHeight : fieldHeight;
            auto fieldY = loadedFieldNumber % 2 ? 0 : 1;

            for (auto y = 0; y < height; y++) {
                auto *outputLine = reinterpret_cast<QRgb*>(outputImage.scanLine(y + startOffset));
                std::copy_n(&rgbData[fieldY * inputWidth], inputWidth, &outputLine[outputOffset]);

                // Only increment fieldY every other iteration, or if field stretch disabled
                if (!getStretchField() || y % 2 != 0) {
                    fieldY += 2;
                }
            }
            break;
        }
    }

    return outputImage;
}

// Generate the data points for the Drop-out and SNR analysis graphs, and the chapter map.
// We do these all at the same time to reduce calls to the metadata.
void TbcSource::generateData()
{
    dropoutGraphData.clear();
    visibleDropoutGraphData.clear();
    blackSnrGraphData.clear();
    whiteSnrGraphData.clear();

    dropoutGraphData.resize(ldDecodeMetaData.getNumberOfFrames());
    visibleDropoutGraphData.resize(ldDecodeMetaData.getNumberOfFrames());
    blackSnrGraphData.resize(ldDecodeMetaData.getNumberOfFrames());
    whiteSnrGraphData.resize(ldDecodeMetaData.getNumberOfFrames());

    bool ignoreChapters = false;
    qint32 lastChapter = -1;
    qint32 giveUpCounter = 0;
    chapterMap.clear();

    const qint32 numFrames = ldDecodeMetaData.getNumberOfFrames();
    for (qint32 frameNumber = 0; frameNumber < numFrames; frameNumber++) {
        double doLength = 0;
        double visibleDoLength = 0;
        double blackSnrTotal = 0;
        double whiteSnrTotal = 0;

        // SNR data may be missing in some fields, so we count the points to prevent
        // the frame average from being thrown-off by missing data
        double blackSnrPoints = 0;
        double whiteSnrPoints = 0;

        const LdDecodeMetaData::Field &firstField = ldDecodeMetaData.getField(ldDecodeMetaData.getFirstFieldNumber(frameNumber + 1));
        const LdDecodeMetaData::Field &secondField = ldDecodeMetaData.getField(ldDecodeMetaData.getSecondFieldNumber(frameNumber + 1));

        // Get the first field DOs
        if (firstField.dropOuts.size() > 0) {
            // Calculate the total length of the dropouts
            for (qint32 i = 0; i < firstField.dropOuts.size(); i++) {
                doLength += static_cast<double>(firstField.dropOuts.endx(i) - firstField.dropOuts.startx(i));
            }
        }

        // Get the second field DOs
        if (secondField.dropOuts.size() > 0) {
            // Calculate the total length of the dropouts
            for (qint32 i = 0; i < secondField.dropOuts.size(); i++) {
                doLength += static_cast<double>(secondField.dropOuts.endx(i) - secondField.dropOuts.startx(i));
            }
        }

        // Get the first field visible DOs
        const LdDecodeMetaData::VideoParameters &videoParameters = ldDecodeMetaData.getVideoParameters();

        if (firstField.dropOuts.size() > 0) {
            // Calculate the total length of the visible dropouts
            for (qint32 i = 0; i < firstField.dropOuts.size(); i++) {
                // Does the drop out start in the visible area?
                if ((firstField.dropOuts.fieldLine(i) >= videoParameters.firstActiveFieldLine) &&
                    (firstField.dropOuts.fieldLine(i) <= videoParameters.lastActiveFieldLine)) {
                    // Check if dropout overlaps with active video area
                    qint32 dropStartx = firstField.dropOuts.startx(i);
                    qint32 dropEndx = firstField.dropOuts.endx(i);
                    
                    if (dropStartx < videoParameters.activeVideoEnd && dropEndx > videoParameters.activeVideoStart) {
                        // Clamp to active video boundaries
                        qint32 startx = qMax(dropStartx, videoParameters.activeVideoStart);
                        qint32 endx = qMin(dropEndx, videoParameters.activeVideoEnd);
                        
                        if (endx > startx) {
                            visibleDoLength += static_cast<double>(endx - startx);
                        }
                    }
                }
            }
        }

        // Get the second field visible DOs
        if (secondField.dropOuts.size() > 0) {
            // Calculate the total length of the visible dropouts
            for (qint32 i = 0; i < secondField.dropOuts.size(); i++) {
                // Does the drop out start in the visible area?
                if ((secondField.dropOuts.fieldLine(i) >= videoParameters.firstActiveFieldLine) &&
                    (secondField.dropOuts.fieldLine(i) <= videoParameters.lastActiveFieldLine)) {
                    // Check if dropout overlaps with active video area
                    qint32 dropStartx = secondField.dropOuts.startx(i);
                    qint32 dropEndx = secondField.dropOuts.endx(i);
                    
                    if (dropStartx < videoParameters.activeVideoEnd && dropEndx > videoParameters.activeVideoStart) {
                        // Clamp to active video boundaries
                        qint32 startx = qMax(dropStartx, videoParameters.activeVideoStart);
                        qint32 endx = qMin(dropEndx, videoParameters.activeVideoEnd);
                        
                        if (endx > startx) {
                            visibleDoLength += static_cast<double>(endx - startx);
                        }
                    }
                }
            }
        }

        // Get the first field SNRs
        if (firstField.vitsMetrics.inUse) {
            if (firstField.vitsMetrics.bPSNR > 0) {
                blackSnrTotal += firstField.vitsMetrics.bPSNR;
                blackSnrPoints++;
            }
            if (firstField.vitsMetrics.wSNR > 0) {
                whiteSnrTotal += firstField.vitsMetrics.wSNR;
                whiteSnrPoints++;
            }
        }

        // Get the second field SNRs
        if (secondField.vitsMetrics.inUse) {
            if (secondField.vitsMetrics.bPSNR > 0) {
                blackSnrTotal += secondField.vitsMetrics.bPSNR;
                blackSnrPoints++;
            }
            if (secondField.vitsMetrics.wSNR > 0) {
                whiteSnrTotal += secondField.vitsMetrics.wSNR;
                whiteSnrPoints++;
            }
        }

        // Add the result to the vectors
        dropoutGraphData[frameNumber] = doLength;
        visibleDropoutGraphData[frameNumber] = visibleDoLength;
        blackSnrGraphData[frameNumber] = blackSnrTotal / blackSnrPoints; // Calc average for frame
        whiteSnrGraphData[frameNumber] = whiteSnrTotal / whiteSnrPoints; // Calc average for frame

        if (ignoreChapters) continue;

        // Decode the VBI
        VbiDecoder::Vbi vbi = vbiDecoder.decodeFrame(
            firstField.vbi.vbiData[0], firstField.vbi.vbiData[1], firstField.vbi.vbiData[2],
            secondField.vbi.vbiData[0], secondField.vbi.vbiData[1], secondField.vbi.vbiData[2]);

        // Get the chapter number
        qint32 currentChapter = vbi.chNo;
        if (currentChapter != -1) {
            if (currentChapter != lastChapter) {
                lastChapter = currentChapter;
                chapterMap.append(frameNumber);
            } else giveUpCounter++;
        }

        if (frameNumber == 100 && giveUpCounter < 50) {
            tbcDebugStream() << "Not seeing valid chapter numbers, giving up chapter mapping";
            ignoreChapters = true;
        }
    }
}

bool TbcSource::startBackgroundLoad(QString sourceFilename)
{
    // Open the TBC metadata file
    tbcDebugStream() << "TbcSource::startBackgroundLoad(): Processing metadata...";
    emit busy("Processing metadata...");

    const bool isCvbs = Cvbs::isDataFileName(sourceFilename);
    const bool isChromaTbc = isCvbs ? Cvbs::channel(sourceFilename) == Cvbs::Channel::Chroma
                                    : TbcFiles::isChromaFile(sourceFilename);
    if (isChromaTbc) {
        // The user specified a chroma file (.cvbsc, .tbcc or _chroma.tbc). The pair
        // shares its metadata with the luma file, so open both, defaulting to the chroma
        // view -- unless the chroma file has metadata of its own
        const QString lumaFilename = isCvbs ? Cvbs::pairedDataFileName(sourceFilename)
                                            : TbcFiles::lumaFileName(sourceFilename);
        const bool hasOwnMetadata = !isCvbs && !LdxCapture::isLdxName(sourceFilename) && !TbcFiles::isOrcYcFile(sourceFilename)
                                    && (QFileInfo::exists(LdDecodeMetaData::metadataFileName(sourceFilename, LdDecodeMetaData::MetadataFormat::Sqlite))
                                        || QFileInfo::exists(LdDecodeMetaData::metadataFileName(sourceFilename, LdDecodeMetaData::MetadataFormat::Json)));
        if (!hasOwnMetadata && (QFileInfo::exists(lumaFilename) || LdxCapture::isLdxName(lumaFilename))) sourceFilename = lumaFilename;
    }

    // SQLite (.tbc.db), or JSON (.tbc.json) when that is all there is. For decode-orc's
    // .tbcy and .tbcc files it is that of the .tbc, and a chroma file with no metadata of
    // its own has that of its luma file.
    QString metadataFileName = LdDecodeMetaData::findMetadataFile(sourceFilename);

    if (!ldDecodeMetaData.read(metadataFileName)) {
        // Open failed
        qWarning() << "Open TBC metadata failed for filename" << metadataFileName;
        currentSourceFilename.clear();

        // Show an error to the user and give up
        lastIOError = "Could not load the metadata file";
        return false;
    }

    // Get the video parameters from the metadata
    LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();
    snrParameters = videoParameters;

    // Open the new source video
    tbcDebugStream() << "TbcSource::startBackgroundLoad(): Loading TBC file...";
    emit busy("Loading video file...");
    if (!sourceVideo.open(sourceFilename, videoParameters.fieldWidth * videoParameters.fieldHeight)) {
        // Open failed
        qWarning() << "Open TBC file failed for filename" << sourceFilename;
        currentSourceFilename.clear();

        // Show an error to the user and give up
        lastIOError = "Could not open the video file";
        return false;
    }

    // Is there a separate chroma file: _chroma.tbc beside a .tbc, .tbcc beside a .tbcy,
    // or .cvbsc beside a .cvbsy? (A chroma file opened alone has none to go with it.)
    QString chromaSourceFilename = isCvbs ? Cvbs::pairedDataFileName(sourceFilename)
                                          : TbcFiles::chromaFileName(sourceFilename);
    if (chromaSourceFilename == sourceFilename) chromaSourceFilename.clear();
    if (!chromaSourceFilename.isEmpty() && (QFileInfo::exists(chromaSourceFilename) || LdxCapture::isLdxName(chromaSourceFilename))) {
        // Yes! Open it.
        tbcDebugStream() << "TbcSource::startBackgroundLoad(): Loading chroma TBC file...";
        emit busy("Loading chroma file...");
        if (!chromaSourceVideo.open(chromaSourceFilename, videoParameters.fieldWidth * videoParameters.fieldHeight)) {
            // Open failed
            qWarning() << "Open chroma TBC file failed for filename" << chromaSourceFilename;
            currentSourceFilename.clear();
            sourceVideo.close();

            // Show an error to the user and give up
            lastIOError = "Could not open the chroma file";
            return false;
        }

        currentChromaFilename = chromaSourceFilename;
        sourceMode = isChromaTbc ? CHROMA_SOURCE : BOTH_SOURCES;
    }

    // Both the video and metadata files are now open
    sourceReady = true;
    currentSourceFilename = sourceFilename;
    currentMetadataFilename = metadataFileName;

    // Configure the chroma decoder
    if (videoParameters.system == PAL || videoParameters.system == PAL_M) {
        palColour.updateConfiguration(videoParameters, palConfiguration);
    } else {
        if (isChromaTbc || sourceMode != ONE_SOURCE) {
            // Enable phase compensation by default, since this is probably a videotape source
            ntscConfiguration.phaseCompensation = true;
        }
        ntscColour.updateConfiguration(videoParameters, ntscConfiguration);
    }

    // Analyse the metadata
    emit busy("Generating graph data and chapter map...");
    generateData();

    return true;
}

void TbcSource::finishBackgroundLoad()
{
    // Send a finished loading message to the main window
    emit finishedLoading(future.result());
}

bool TbcSource::startBackgroundSave(QString metadataFilename)
{
    if (StackProject::isSession(metadataFilename)) {
        lastIOError = "A stacking session has no metadata of its own to save: it is read from its sources";
        return false;
    }
    if (LdxCapture::isLdxName(metadataFilename)) {
        lastIOError = "The metadata of a capture kept in an LDX file are in the file, and are not saved from here";
        return false;
    }

    tbcDebugStream() << "TbcSource::startBackgroundSave(): Saving to" << metadataFilename;
    emit busy("Saving metadata...");

    // The general idea here is that decoding takes a long time -- so we want
    // to be careful not to destroy the user's only copy of their metadata file if
    // something goes wrong!

    // A CVBS capture: the field metadata goes to the companion file, which is
    // written along with <base>.meta and <base>.dropouts.meta (decode-orc's
    // files, which must keep their own format), so save to the companion
    if (ldDecodeMetaData.isCvbs()) {
        const QString companionFilename = LdDecodeMetaData::metadataFileName(currentSourceFilename,
                                                                             ldDecodeMetaData.getFormat());
        if (QFile::exists(companionFilename) && !QFile::exists(companionFilename + ".bup")
            && !QFile::copy(companionFilename, companionFilename + ".bup")) {
            lastIOError = "Could not back up the existing metadata file";
            return false;
        }
        if (!ldDecodeMetaData.write(companionFilename)) {
            lastIOError = "Could not write the metadata files";
            return false;
        }
        currentMetadataFilename = companionFilename;

        tbcDebugStream() << "TbcSource::startBackgroundSave(): Save complete";
        return true;
    }

    // Write the metadata out to a new temporary file
    QString newMetadataFilename = metadataFilename + ".new";
    if (!ldDecodeMetaData.write(newMetadataFilename)) {
        // Writing failed
        lastIOError = "Could not write to new metadata file";
        return false;
    }

    // If there isn't already a .bup backup file, rename the existing file to that name
    // (matching the behaviour of ld-process-vbi)
    QString backupFilename = metadataFilename + ".bup";
    if (!QFile::exists(backupFilename)) {
        if (!QFile::rename(metadataFilename, metadataFilename + ".bup")) {
            // Renaming failed
            lastIOError = "Could not rename existing metadata file to backup";
            return false;
        }
    } else {
        // There is a backup, so it's safe to remove the existing file
        if (!QFile::remove(metadataFilename)) {
            // Deleting failed
            lastIOError = "Could not remove existing metadata file";
            return false;
        }
    }

    // Rename the new file to the target name
    if (!QFile::rename(newMetadataFilename, metadataFilename)) {
        // Renaming failed
        lastIOError = "Could not rename new metadata file to target name";
        return false;
    }

    tbcDebugStream() << "TbcSource::startBackgroundSave(): Save complete";
    return true;
}

void TbcSource::finishBackgroundSave()
{
    // Send a finished saving message to the main window
    emit finishedSaving(future.result());
}

// Video from a stream -----------------------------------------------------------------------------------------------

// Read video from standard input or a named pipe. The stream may never end, so the
// number of frames is whatever has arrived (see updateLive), and only the last few
// frames are still there to be shown again.
void TbcSource::loadStream(const StreamInput &input)
{
    resetState();
    streamInput = input;
    isLive = true;

    disconnect(&watcher, &QFutureWatcher<bool>::finished, nullptr, nullptr);
    connect(&watcher, &QFutureWatcher<bool>::finished, this, &TbcSource::finishBackgroundLoad);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    future = QtConcurrent::run(this, &TbcSource::startBackgroundStream);
#else
    future = QtConcurrent::run(&TbcSource::startBackgroundStream, this);
#endif
    watcher.setFuture(future);
}

bool TbcSource::startBackgroundStream()
{
    emit busy("Opening the input stream...");

    // No metadata: the video system is said, the levels have defaults, and the fields
    // are made up as the stream turns out to be long enough
    ldDecodeMetaData.initialiseDefault(streamInput.system, streamInput.blackLevel, streamInput.whiteLevel,
                                       streamInput.isSubcarrierLocked);
    if (LdxStream::instance()) LdxStream::instance()->provideFields(ldDecodeMetaData);
    const LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();
    snrParameters = videoParameters;

    StreamReader::Options options;
    options.cvbs = streamInput.cvbs;
    options.system = streamInput.system;
    options.encoding = streamInput.encoding;
    options.bufferFrames = streamInput.bufferFrames;
    options.ring = !streamInput.throttle;
    sourceVideo.setStreamOptions(options);
    if (!sourceVideo.open("-", videoParameters.fieldWidth * videoParameters.fieldHeight, videoParameters.fieldWidth)) {
        lastIOError = "Could not open the input stream";
        return false;
    }

    // The same decoder settings as for a file
    if (videoParameters.system == PAL || videoParameters.system == PAL_M) {
        palColour.updateConfiguration(videoParameters, palConfiguration);
    } else {
        ntscColour.updateConfiguration(videoParameters, ntscConfiguration);
    }

    // Wait for the first frame, and what the decoder looks ahead to
    emit busy("Waiting for video...");
    if (!sourceVideo.hasField(2 * (1 + getLookAheadFrames())) && sourceVideo.getFieldsReadSoFar() < 2) {
        sourceVideo.close();
        lastIOError = "The input ended before it held a frame of video";
        return false;
    }

    // A CVBS stream with more than 10 bits to a sample, read as if it had 10: the picture would be
    // washed out, and a pipe has no metadata to say so
    streamHint.clear();
    if (streamInput.cvbs && streamInput.encoding == Cvbs::SampleEncoding::U10_4FSC && sourceVideo.streamLooksWiderThan10Bit()) {
        streamHint = "The input has samples beyond 10 bits, so it is not u10 (the default): try --sample-encoding u16, s16 or tpg21";
        qWarning().noquote() << streamHint;
    }

    liveAnalyse = true;
    liveAnalysedFrames.clear();
    liveBlackSnr.clear();
    liveWhiteSnr.clear();
    // The SNR of every frame received, on a thread of its own (a throttled input is read at the pace
    // of the viewer, and this would read ahead of it: only the frames shown are measured then)
    if (!streamInput.throttle) {
        liveSnr = std::make_unique<LiveSnr>(sourceVideo);
        liveSnr->setVideoParameters(snrParameters);
        liveSnr->start(QThread::LowPriority);
    }
    sourceReady = true;
    currentSourceFilename = streamInput.cvbs ? "stdin.cvbs" : "stdin.tbc";
    currentMetadataFilename.clear();

    // There are no dropouts, SNR or chapters to graph
    dropoutGraphData.clear();
    visibleDropoutGraphData.clear();
    blackSnrGraphData.clear();
    whiteSnrGraphData.clear();
    chapterMap.clear();

    updateLive();
    return true;
}

bool TbcSource::getIsLive()
{
    return isLive;
}

bool TbcSource::getIsLiveEnded()
{
    return isLive && liveEnded;
}

// How many frames ahead the chroma decoder looks (the stream has to have them before a
// frame can be decoded, or the decoder would wait for them)
qint32 TbcSource::getLookAheadFrames()
{
    if (getSystem() == PAL || getSystem() == PAL_M) return palConfiguration.getLookAhead();
    return ntscConfiguration.getLookAhead();
}

// Take count of what the stream has delivered: the frames that can be shown are the ones
// the decoder's look-ahead has arrived for, and from the first that is still held (the
// decoder looks behind it too) -- unless the stream has ended, when the rest are blank
void TbcSource::updateLive()
{
    if (!isLive || !sourceVideo.isSourceValid()) return;

    const qint32 frames = sourceVideo.getFieldsReadSoFar() / 2;
    liveEnded = sourceVideo.getNumberOfAvailableFields() >= 0;
    liveFrames = liveEnded ? frames : qMax(0, frames - getLookAheadFrames());
    // Reversed, a frame is made of a field and the one after the one it would have started with: the
    // last frame needs a field more than it did
    if (reverseFoOn) liveFrames = qMax(0, liveFrames - 1);

    // The first frame with both its fields still in the buffer
    const qint32 firstHeldField = sourceVideo.getFirstFieldHeld();
    qint32 firstFrame = (firstHeldField + 2) / 2;
    if (firstHeldField > 1) {
        const qint32 lookBehind = (getSystem() == PAL || getSystem() == PAL_M) ? palConfiguration.getLookBehind()
                                                                               : ntscConfiguration.getLookBehind();
        firstFrame += lookBehind;
    }
    liveFirstFrame = qMin(firstFrame, qMax(1, liveFrames));
}

qint32 TbcSource::getLookBehindFrames()
{
    if (getSystem() == PAL || getSystem() == PAL_M) return palConfiguration.getLookBehind();
    return ntscConfiguration.getLookBehind();
}

QString TbcSource::getStreamHint()
{
    return streamHint;
}

qint32 TbcSource::getFramesReceived()
{
    return liveFrames;
}

bool TbcSource::getIsLiveFrozen()
{
    return isLive && liveFrozen;
}

// Pausing a live input: keep the frames around the one shown, and everything that has arrived
// since, safe from being overwritten, so that they can be gone through and then played on from.
// The input is not held up: it carries on into the ring, which overwrites what is not kept.
void TbcSource::freezeLive(qint32 aroundFrame)
{
    if (!isLive || streamInput.throttle) return;
    updateLive();

    const qint32 lookBehind = getLookBehindFrames();
    frozenLastFrame = liveFrames;
    frozenFirstFrame = qBound(1, aroundFrame - streamInput.bufferFrames, qMax(1, frozenLastFrame));
    frozenFirstFrame = qMax(frozenFirstFrame, liveFirstFrame);
    frozenFirstFrame = qMin(frozenFirstFrame, qMax(1, frozenLastFrame));

    // The frames the first one looks behind to, up to the newest field there is
    const qint32 protectFrom = qMax(1, frozenFirstFrame - lookBehind);
    sourceVideo.protectFields(2 * (protectFrom - 1) + 1, qMax(sourceVideo.getFieldsReadSoFar(), 2 * frozenLastFrame));
    liveProtected = true;
    liveFrozen = true;
}

void TbcSource::unfreezeLive()
{
    liveFrozen = false;
}

void TbcSource::dropLiveProtection()
{
    liveFrozen = false;
    if (liveProtected) {
        sourceVideo.clearProtection();
        liveProtected = false;
    }
}

void TbcSource::releaseLiveProtection(qint32 currentFrame)
{
    if (!liveProtected || liveFrozen) return;
    if (currentFrame > frozenLastFrame) {
        sourceVideo.clearProtection();
        liveProtected = false;
    }
}

bool TbcSource::isFrameHeld(qint32 frame)
{
    // (a throttled stream keeps what is behind the viewer, and nothing is overwritten)
    if (!isLive || streamInput.throttle) return true;
    return sourceVideo.isFieldHeld(2 * frame - 1) && sourceVideo.isFieldHeld(2 * frame);
}

qint32 TbcSource::nextHeldFrame(qint32 frame)
{
    if (!isLive) return frame;
    const qint32 field = sourceVideo.firstFieldHeldAtOrAfter(2 * frame - 1);
    if (field < 0) return -1;
    qint32 found = (field + 1) / 2;
    if (field % 2 == 0) found++;       // only the second field of that frame is held
    return found;
}

qint32 TbcSource::getFirstAvailableFrame()
{
    if (!isLive) return 1;
    return liveFrozen ? frozenFirstFrame : liveFirstFrame;
}

// Live input: decode the VBI and measure the SNR of the two fields of a frame from the video, which
// is what ld-process-vbi and ld-process-vits do for a file, and keep the results as the field
// metadata, where the rest of ld-analyse looks for them. The frame's SNR is kept for the graphs.
void TbcSource::analyseLiveFrame(qint32 frameNumber)
{
    if (liveAnalysedFrames.contains(frameNumber)) return;

    const qint32 fieldNumbers[2] = { ldDecodeMetaData.getFirstFieldNumber(frameNumber),
                                     ldDecodeMetaData.getSecondFieldNumber(frameNumber) };
    if (fieldNumbers[0] < 1 || fieldNumbers[1] < 1) return;

    // A field that the ring has overwritten has nothing to measure
    for (qint32 fieldNumber: fieldNumbers) {
        if (!sourceVideo.isFieldHeld(fieldNumber)) return;
    }

    const LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData.getVideoParameters();
    double blackTotal = 0, whiteTotal = 0;
    qint32 blackPoints = 0, whitePoints = 0;

    for (qint32 fieldNumber: fieldNumbers) {
        // Only the lines at the top of the field are needed
        const qint32 lastLine = qMax(VBI_END_FIELD_LINE, VITS_END_FIELD_LINE);
        const SourceVideo::Data lines = sourceVideo.getVideoField(fieldNumber, 1, lastLine);

        LdDecodeMetaData::Field field = ldDecodeMetaData.getField(fieldNumber);
        decodeFieldVbi(lines, 1, videoParameters, field);
        applyVitsLevels(measureVitsRaw(lines, 1, videoParameters), snrParameters.isValid ? snrParameters : videoParameters, field, vitsIreSpan(videoParameters.system));
        field.vitsMetrics.inUse = true;

        ldDecodeMetaData.updateFieldVbi(field.vbi, fieldNumber);
        ldDecodeMetaData.updateFieldNtsc(field.ntsc, fieldNumber);
        ldDecodeMetaData.updateFieldVitc(field.vitc, fieldNumber);
        ldDecodeMetaData.updateFieldClosedCaption(field.closedCaption, fieldNumber);
        ldDecodeMetaData.updateFieldVitsMetrics(field.vitsMetrics, fieldNumber);

        // The average for the frame, as for a file
        if (std::isfinite(field.vitsMetrics.bPSNR) && field.vitsMetrics.bPSNR > 0) {
            blackTotal += field.vitsMetrics.bPSNR;
            blackPoints++;
        }
        if (std::isfinite(field.vitsMetrics.wSNR) && field.vitsMetrics.wSNR > 0) {
            whiteTotal += field.vitsMetrics.wSNR;
            whitePoints++;
        }
    }

    if (!liveSnr) {
        if (blackPoints > 0) liveBlackSnr.insert(frameNumber, blackTotal / blackPoints);
        if (whitePoints > 0) liveWhiteSnr.insert(frameNumber, whiteTotal / whitePoints);
    }
    liveAnalysedFrames.insert(frameNumber);
}

QVector<QPointF> TbcSource::getLiveSnrHistory(bool black, qint32 maxPoints)
{
    if (liveSnr) return liveSnr->history(black, maxPoints);

    // The frames that were shown
    QVector<QPointF> points;
    const QMap<qint32, double> &map = black ? liveBlackSnr : liveWhiteSnr;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) points.append(QPointF(it.key(), it.value()));
    return points;
}

bool TbcSource::getLiveSnrIsWholeHistory()
{
    return liveSnr != nullptr;
}

bool TbcSource::getLiveSnrEnabled()
{
    return liveSnr ? liveSnr->isEnabled() : true;
}

void TbcSource::setLiveSnrEnabled(bool enabled)
{
    if (liveSnr) liveSnr->setEnabled(enabled);
}

void TbcSource::clearLiveSnr()
{
    if (liveSnr) {
        liveSnr->clear();
    } else {
        liveBlackSnr.clear();
        liveWhiteSnr.clear();
    }
}

// A file: measure the SNR of every frame from the video, for when the metadata has none or is not
// to be trusted. It is done on a thread of its own, so the window carries on meanwhile.
void TbcSource::startFileSnr()
{
    if (isLive || !sourceReady || fileSnr) return;

    QVector<QPair<qint32, qint32>> frameFields;
    const qint32 frames = ldDecodeMetaData.getNumberOfFrames();
    frameFields.reserve(frames);
    for (qint32 frame = 1; frame <= frames; frame++) {
        frameFields.append(qMakePair(ldDecodeMetaData.getFirstFieldNumber(frame), ldDecodeMetaData.getSecondFieldNumber(frame)));
    }

    const LdDecodeMetaData::VideoParameters videoParameters = snrParameters.isValid ? snrParameters : ldDecodeMetaData.getVideoParameters();
    fileSnr = std::make_unique<FileSnr>(currentSourceFilename, videoParameters.fieldWidth * videoParameters.fieldHeight,
                                        videoParameters.fieldWidth, frameFields);
    fileSnr->setVideoParameters(videoParameters);
    fileSnr->start(QThread::LowPriority);
}

void TbcSource::stopFileSnr()
{
    fileSnr.reset();
}

bool TbcSource::getFileSnrActive()
{
    return fileSnr != nullptr;
}

qint32 TbcSource::getFileSnrProgressPercent()
{
    if (!fileSnr || fileSnr->totalFrames() == 0) return -1;
    return static_cast<qint32>(100LL * fileSnr->measuredFrames() / fileSnr->totalFrames());
}

QVector<QPointF> TbcSource::getFileSnrHistory(bool black, qint32 maxPoints)
{
    return fileSnr ? fileSnr->history(black, maxPoints) : QVector<QPointF>();
}
