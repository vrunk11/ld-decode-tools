/******************************************************************************
 * framebackend.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "framebackend.h"
#include "stackframe.h"
#include "dropoutdrawing.h"

#include "stackedvideo.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include "ldxcapture.h"
#include "vbidecoder.h"
#include <QThread>
#include <atomic>
#include <cmath>

#include "stackercore.h"
#include "tbc/logging.h"
#include "vitsmetrics.h"

// The lines of a field that the SNR is measured from: the test signals are on the first 22 lines (VITS_END_FIELD_LINE), a couple more for the neighbours
static constexpr qint32 snrLines = 24;

FrameBackend::FrameBackend(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<SourceInfo>("SourceInfo");
    qRegisterMetaType<FrameRequest>("FrameRequest");
    qRegisterMetaType<FrameResult>("FrameResult");
    qRegisterMetaType<StackSnrPoint>("StackSnrPoint");
    qRegisterMetaType<QVector<StackSnrPoint>>("QVector<StackSnrPoint>");
    qRegisterMetaType<PalColour::Configuration>("PalColour::Configuration");
    qRegisterMetaType<Comb::Configuration>("Comb::Configuration");

    // As ld-analyse sets them: 2D decoders, which need only the frame itself, and 16-bit colour
    palConfiguration = palColour.getConfiguration();
    palConfiguration.chromaFilter = PalColour::transform2DFilter;
    ntscConfiguration = ntscColour.getConfiguration();
    ntscConfiguration.dimensions = 2;
    outputConfiguration.pixelFormat = OutputWriter::PixelFormat::RGB48;
    outputConfiguration.paddingAmount = 1;
}

void FrameBackend::addSource(const QString &id, const QString &file, bool reverseFields)
{
    SourceInfo info;
    info.id = id;
    info.file = file;
    info.cvbs = Cvbs::isDataFileName(StackProject::referenceFile(file));

    auto loaded = std::make_shared<Loaded>();
    loaded->file = file;

    const QString metadataFile = LdDecodeMetaData::findMetadataFile(file);
    info.metadataFile = metadataFile;
    if (metadataFile.isEmpty() || !loaded->metadata.read(metadataFile)) {
        info.error = tr("Could not load the metadata of %1").arg(QFileInfo(file).fileName());
        emit sourceLoaded(info);
        return;
    }

    const LdDecodeMetaData::VideoParameters &videoParameters = loaded->metadata.getVideoParameters();
    loaded->video.setLenient(true);
    if (!loaded->video.open(file, videoParameters.fieldWidth * videoParameters.fieldHeight, videoParameters.fieldWidth)) {
        info.error = tr("Could not open %1").arg(QFileInfo(file).fileName());
        emit sourceLoaded(info);
        return;
    }
    loaded->metadata.setIsFirstFieldFirst(!reverseFields);
    loaded->ok = true;

    info.ok = true;
    info.system = videoParameters.system;
    info.widescreen = videoParameters.isWidescreen;
    info.fieldWidth = videoParameters.fieldWidth;
    info.fieldHeight = videoParameters.fieldHeight;
    info.frames = loaded->metadata.getNumberOfFrames();
    info.whiteSnr.resize(info.frames);
    info.blackSnr.resize(info.frames);
    info.dropoutLength.resize(info.frames);
    info.metadataFormat = info.cvbs ? QStringLiteral("CVBS") : (metadataFile.endsWith(".json", Qt::CaseInsensitive) ? QStringLiteral("JSON") : QStringLiteral("SQLite"));
    info.isMapped = videoParameters.isMapped;
    info.sampleRate = videoParameters.sampleRate;
    info.fsc = videoParameters.fSC;
    info.blackIre = videoParameters.black16bIre;
    info.whiteIre = videoParameters.white16bIre;
    info.tapeFormat = videoParameters.tapeFormat;
    info.gitCommit = videoParameters.gitCommit;
    info.fields = loaded->metadata.getNumberOfFields();
    info.history = LdxCapture::historyOf(file);
    VbiDecoder vbiDecoder;
    QSet<qint32> chapterNumbers;
    qint32 previousPicture = -1, previousSeconds = -1;
    for (qint32 frame = 1; frame <= info.frames; frame++) {
        const qint32 firstNumber = loaded->metadata.getFirstFieldNumber(frame);
        const qint32 secondNumber = loaded->metadata.getSecondFieldNumber(frame);
        info.whiteSnr[frame - 1] = std::nanf("");
        info.blackSnr[frame - 1] = std::nanf("");
        info.dropoutLength[frame - 1] = std::nanf("");
        // (a frame whose fields cannot be found has no SNR)
        if (firstNumber < 1 || secondNumber < 1) {
            info.paddedFrames++;
            continue;
        }
        const LdDecodeMetaData::Field &first = loaded->metadata.getField(firstNumber);
        const LdDecodeMetaData::Field &second = loaded->metadata.getField(secondNumber);
        if (first.pad || second.pad) info.paddedFrames++;
        if (!first.pad && !second.pad && !isWholeDropout(first.dropOuts, videoParameters.fieldWidth, videoParameters.fieldHeight)
            && !isWholeDropout(second.dropOuts, videoParameters.fieldWidth, videoParameters.fieldHeight)) {
            const qint64 length = first.dropOuts.totalLength() + second.dropOuts.totalLength();
            info.dropoutLength[frame - 1] = static_cast<float>(length);
        }

        // What the VBI says: the type of the disc, the first and the last picture or time, the chapters, and where the numbering jumps
        const VbiDecoder::Vbi vbi = vbiDecoder.decodeFrame(first.vbi.vbiData[0], first.vbi.vbiData[1], first.vbi.vbiData[2],
                                                           second.vbi.vbiData[0], second.vbi.vbiData[1], second.vbi.vbiData[2]);
        if (vbi.picNo != -1 || vbi.clvHr != -1) info.vbiFrames++;
        if (vbi.chNo != -1) chapterNumbers.insert(vbi.chNo);
        if (vbi.picNo != -1) {
            if (info.discType == 0) info.discType = 2;
            if (info.firstPicture < 0) info.firstPicture = vbi.picNo;
            info.lastPicture = vbi.picNo;
            if (previousPicture != -1) {
                if (vbi.picNo == previousPicture) info.duplicates++;
                else if (vbi.picNo != previousPicture + 1) info.jumps++;
            }
            previousPicture = vbi.picNo;
        } else if (vbi.clvHr != -1) {
            if (info.discType == 0) info.discType = 1;
            const qint32 seconds = vbi.clvHr * 3600 + vbi.clvMin * 60 + vbi.clvSec;
            if (info.firstSeconds < 0) info.firstSeconds = seconds;
            info.lastSeconds = seconds;
            // (a time code lasts a second of frames: it is a jump when it goes back or skips a second)
            if (previousSeconds != -1 && (seconds < previousSeconds || seconds > previousSeconds + 1)) info.jumps++;
            previousSeconds = seconds;
        }
        SnrAverage snr;
        snr.add(first.vitsMetrics);
        snr.add(second.vitsMetrics);
        info.whiteSnr[frame - 1] = static_cast<float>(snr.white());
        info.blackSnr[frame - 1] = static_cast<float>(snr.black());
    }

    info.chapters = chapterNumbers.size();
    info.originDisc = StackedVideo::discOrigin(loaded->metadata);
    sources.insert(id, loaded);
    emit sourceLoaded(info);
}

void FrameBackend::removeSource(const QString &id)
{
    // (the handles of the threads that measure the SNR are on the files too: a file that is open cannot be replaced on Windows)
    snrReaders.clear();
    snrReadersGeneration = 0;
    sourceReaders.clear();
    sourceReadersGeneration = 0;
    sources.remove(id);
}

void FrameBackend::setReverseFields(bool reverseFields)
{
    for (const std::shared_ptr<Loaded> &source: sources) source->metadata.setIsFirstFieldFirst(!reverseFields);
}

qint32 FrameBackend::sourceFrame(Loaded &source, qint32 frame, qint32 offset)
{
    return StackFrame::ownFrame(frame, offset, source.metadata.getNumberOfFrames());
}

// A frame with a padded (missing) field is not one the stack can use
bool FrameBackend::isPadded(Loaded &source, qint32 ownFrame)
{
    return StackFrame::isPadded(source.metadata, ownFrame);
}

// The picture of a frame as ld-analyse shows a field (its lines doubled) or both fields (split: the first above
// the second): the rows of the first field are the even ones of the frame, those of the second the odd ones
QImage FrameBackend::applyView(const QImage &frame, qint32 viewMode, qint32 fieldIndex)
{
    if (viewMode == 0 || frame.isNull()) return frame;

    const qint32 height = frame.height();
    QImage result(frame.width(), height, frame.format());
    for (qint32 y = 0; y < height; y++) {
        qint32 source;
        if (viewMode == 1) {
            source = (y / 2) * 2 + (fieldIndex ? 1 : 0);
        } else {
            const qint32 top = (height + 1) / 2;      // the rows of the first field
            source = y < top ? 2 * y : 2 * (y - top) + 1;
        }
        source = qBound(0, source, height - 1);
        memcpy(result.scanLine(y), frame.constScanLine(source), static_cast<size_t>(frame.bytesPerLine()));
    }
    return result;
}

// A field that is a dropout over its whole width on every line (a frame that ld-discmap could not number is written so): the stack leaves it out, and it is
// not drawn, since it would colour the whole picture
bool FrameBackend::isWholeDropout(const DropOuts &dropOuts, qint32 fieldWidth, qint32 fieldHeight)
{
    return dropOuts.coversWholeField(fieldWidth, fieldHeight);
}

// The dropouts of one field, on the rows it has in the frame. The first field is drawn in the colour, the second a little darker so that the two can be told apart.
void FrameBackend::drawDropouts(QImage &image, const DropOuts &dropOuts, bool secondField, const QColor &color)
{
    DropoutDrawing::draw(image, dropOuts, secondField, secondField ? color.darker(130) : color.lighter(135));
}

// The dropouts that the capture has for a frame of it, unless the whole frame is marked as a dropout
void FrameBackend::drawSourceDropouts(QImage &image, Loaded &source, qint32 ownFrame, const QColor &color)
{
    const qint32 first = source.metadata.getFirstFieldNumber(ownFrame);
    const qint32 second = source.metadata.getSecondFieldNumber(ownFrame);
    if (first < 1 || second < 1) return;
    const LdDecodeMetaData::VideoParameters &parameters = source.metadata.getVideoParameters();
    const DropOuts firstDropOuts = source.metadata.getFieldDropOuts(first);
    const DropOuts secondDropOuts = source.metadata.getFieldDropOuts(second);
    if (isWholeDropout(firstDropOuts, parameters.fieldWidth, parameters.fieldHeight)
        || isWholeDropout(secondDropOuts, parameters.fieldWidth, parameters.fieldHeight)) return;
    drawDropouts(image, firstDropOuts, false, color);
    drawDropouts(image, secondDropOuts, true, color);
}

// The two fields of a frame as a grey picture, as ld-analyse shows a source
QImage FrameBackend::weave(const SourceVideo::Data &first, const SourceVideo::Data &second, qint32 width, qint32 fieldHeight)
{
    const qint32 height = fieldHeight * 2 - 1;
    QImage image(width, height, QImage::Format_RGB32);
    for (qint32 y = 0; y < height; y++) {
        const quint16 *line = (y % 2 == 0 ? first : second).constData() + static_cast<qsizetype>(y / 2) * width;
        QRgb *out = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (qint32 x = 0; x < width; x++) {
            const qint32 value = line[x] / 256;
            out[x] = qRgb(value, value, value);
        }
    }
    return image;
}

QImage FrameBackend::blankPicture(const FrameRequest &request) const
{
    for (const FrameRequest::Entry &entry: request.entries) {
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok) continue;
        const LdDecodeMetaData::VideoParameters &videoParameters = (*found)->metadata.getVideoParameters();
        QImage image(videoParameters.fieldWidth, videoParameters.fieldHeight * 2 - 1, QImage::Format_RGB32);
        image.fill(Qt::black);
        return applyView(image, request.viewMode, request.fieldIndex);
    }
    return QImage();
}

void FrameBackend::setChromaConfiguration(const PalColour::Configuration &pal, const Comb::Configuration &ntsc)
{
    palConfiguration = pal;
    ntscConfiguration = ntsc;
    decoderKey.clear();   // the decoders are set up again for the next picture
}

// How many frames before and after the one decoded the decoder looks at (0 for the 2D ones)
void FrameBackend::lookRange(VideoSystem system, qint32 &lookBehind, qint32 &lookAhead) const
{
    const bool isPal = system == PAL || system == PAL_M;
    lookBehind = isPal ? palConfiguration.getLookBehind() : ntscConfiguration.getLookBehind();
    lookAhead = isPal ? palConfiguration.getLookAhead() : ntscConfiguration.getLookAhead();
}

void FrameBackend::configureDecoders(LdDecodeMetaData::VideoParameters videoParameters)
{
    // Set up again only when the video is not what the decoders were set up for
    const QString key = QString("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11").arg(videoParameters.system).arg(videoParameters.fieldWidth)
        .arg(videoParameters.fieldHeight).arg(videoParameters.black16bIre).arg(videoParameters.white16bIre)
        .arg(videoParameters.colourBurstStart).arg(videoParameters.colourBurstEnd).arg(videoParameters.activeVideoStart)
        .arg(videoParameters.activeVideoEnd).arg(videoParameters.firstActiveFrameLine).arg(videoParameters.lastActiveFrameLine);
    if (key == decoderKey) return;
    decoderKey = key;

    updateColourConfiguration(videoParameters, palColour, palConfiguration, ntscColour, ntscConfiguration);
    outputWriter.updateConfiguration(videoParameters, outputConfiguration);
}

// The picture of a frame as ld-analyse shows it with the chroma decoder on: the active picture in
// colour, in a frame that is black around it
QImage FrameBackend::chromaPicture(Loaded &source, qint32 ownFrame, const QVector<StackedFrame> *around, qint32 lookBehind)
{
    const LdDecodeMetaData::VideoParameters videoParameters = source.metadata.getVideoParameters();
    configureDecoders(videoParameters);
    const bool isPal = videoParameters.system == PAL || videoParameters.system == PAL_M;

    qint32 lookBehindFrames, lookAheadFrames;
    lookRange(videoParameters.system, lookBehindFrames, lookAheadFrames);

    QVector<SourceField> fields;
    qint32 startIndex, endIndex;
    SourceField::loadFields(source.video, source.metadata, ownFrame, 1, lookBehindFrames, lookAheadFrames, fields, startIndex, endIndex);

    // For the stack, the fields of the source are replaced by the stacked ones, for the frame and for those
    // around it that the decoder looks at
    if (around) {
        for (qint32 j = 0; j < around->size(); j++) {
            const StackedFrame &stacked = (*around)[j];
            if (!stacked.ok) continue;
            const qint32 at = startIndex - 2 * lookBehind + 2 * j;
            if (at < 0 || at + 1 >= fields.size()) continue;
            fields[at].data = stacked.first;
            fields[at + 1].data = stacked.second;
            if (isPal && videoParameters.isSubcarrierLocked) {
                // As the fields are read for the decoder: 4fSC PAL has the second field two samples out
                fields[at + 1].data.remove(0, 2);
                for (qint32 i = 0; i < 2; i++) fields[at + 1].data.append(videoParameters.black16bIre);
            }
        }
    }

    QVector<ComponentFrame> componentFrames(1);
    if (isPal) palColour.decodeFrames(fields, startIndex, endIndex, componentFrames);
    else ntscColour.decodeFrames(fields, startIndex, endIndex, componentFrames);

    OutputFrame outputFrame;
    outputWriter.convert(componentFrames[0], outputFrame);
    const quint16 *rgb48 = reinterpret_cast<const quint16 *>(outputFrame.data());

    const qint32 frameWidth = videoParameters.fieldWidth;
    const qint32 frameHeight = videoParameters.fieldHeight * 2 - 1;
    const qint32 inputHeight = videoParameters.lastActiveFrameLine - videoParameters.firstActiveFrameLine;
    const qint32 inputWidth = videoParameters.activeVideoEnd - videoParameters.activeVideoStart;

    QImage image(frameWidth, frameHeight, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (qint32 y = 0; y < inputHeight; y++) {
        const qint32 row = y + videoParameters.firstActiveFrameLine;
        if (row < 0 || row >= frameHeight) continue;
        QRgb *out = reinterpret_cast<QRgb *>(image.scanLine(row)) + videoParameters.activeVideoStart;
        const quint16 *in = rgb48 + static_cast<qsizetype>(y) * inputWidth * 3;
        for (qint32 x = 0; x < inputWidth; x++) {
            out[x] = qRgb(in[x * 3] / 256, in[x * 3 + 1] / 256, in[x * 3 + 2] / 256);
        }
    }
    return image;
}

void FrameBackend::render(const FrameRequest &request)
{
    // Overtaken by a newer request: not worth the time
    if (request.serial < latest[request.channel & 1].load()) return;

    FrameResult result;
    result.serial = request.serial;
    result.channel = request.channel;
    result.frame = request.frame;
    result.view = request.view;
    result.stackWhiteSnr = std::nan("");
    result.stackBlackSnr = std::nan("");

    // The sources that are on, and which of them have a frame here
    const qint32 total = request.entries.size();
    QVector<qint32> frames(total, -1);
    for (qint32 i = 0; i < total; i++) {
        const FrameRequest::Entry &entry = request.entries[i];
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok) continue;
        if (entry.enabled) result.inStack++;
        frames[i] = sourceFrame(**found, request.frame, entry.offset);
    }

    if (request.view != "stack") {
        // One source: its frame, whether it is on or off
        for (qint32 i = 0; i < total; i++) {
            if (request.entries[i].id != request.view) continue;
            const auto found = sources.constFind(request.view);
            if (found == sources.constEnd() || !(*found)->ok) {
                result.message = tr("This source is not loaded");
            } else if (frames[i] < 0) {
                result.message = tr("This source has no frame here");
                result.image = blankPicture(request);
            } else {
                Loaded &source = **found;
                const qint32 first = source.metadata.getFirstFieldNumber(frames[i]);
                const qint32 second = source.metadata.getSecondFieldNumber(frames[i]);
                const qint32 availableFields = source.video.getNumberOfAvailableFields();
                if (first < 1 || second < 1 || first > availableFields || second > availableFields) {
                    // (the metadata of a capture can describe frames that its file does not hold, as the original of a mapping can)
                    result.message = tr("This frame is not in the file of the source");
                    result.image = blankPicture(request);
                    emit frameReady(result);
                    return;
                }
                const LdDecodeMetaData::VideoParameters &videoParameters = source.metadata.getVideoParameters();
                SourceVideo::Data firstData, secondData;
                if (first < second) {
                    firstData = source.video.getVideoField(first);
                    secondData = source.video.getVideoField(second);
                } else {
                    secondData = source.video.getVideoField(second);
                    firstData = source.video.getVideoField(first);
                }
                result.image = request.chroma ? chromaPicture(source, frames[i], nullptr, 0)
                                              : weave(firstData, secondData, videoParameters.fieldWidth, videoParameters.fieldHeight);
                if (request.entries[i].showDropouts) drawSourceDropouts(result.image, source, frames[i], QColor(request.entries[i].color));
                result.image = applyView(result.image, request.viewMode, request.fieldIndex);
                // (the fields are whole here: the SNR of the frame is known, and a measure of the source that goes by the frames that are seeked to takes it)
                {
                    SnrAverage snr;
                    snr.add(firstData, 1, videoParameters);
                    snr.add(secondData, 1, videoParameters);
                    if (snr.hasWhite()) result.sourceWhiteSnr = snr.white();
                    if (snr.hasBlack()) result.sourceBlackSnr = snr.black();
                }
            }
        }
        emit frameReady(result);
        return;
    }

    // The stack
    StackedFrame main;
    if (!stackFrame(request, request.frame, result.inStack, main)) {
        result.message = result.inStack ? tr("No source has a frame here") : tr("No source is on");
        // (a blank frame where nothing is: the timeline goes from the first frame of any source to the last of any)
        result.image = blankPicture(request);
        emit frameReady(result);
        return;
    }
    result.modeUsed = main.mode;
    result.contributing = main.usable.size();

    if (request.chroma) {
        // A 3D decoder looks at the frames either side, so those are stacked too
        Loaded &referenceSource = *sources.value(main.sourceId);
        qint32 lookBehind, lookAhead;
        lookRange(main.videoParameters.system, lookBehind, lookAhead);
        QVector<StackedFrame> around(lookBehind + 1 + lookAhead);
        for (qint32 j = 0; j < around.size(); j++) {
            if (j == lookBehind) {
                around[j] = main;
            } else {
                stackFrame(request, request.frame - lookBehind + j, result.inStack, around[j]);
            }
        }
        result.image = chromaPicture(referenceSource, main.ownFrame, &around, lookBehind);
    } else {
        result.image = weave(main.first, main.second, main.videoParameters.fieldWidth, main.videoParameters.fieldHeight);
    }
    // The dropouts of the captures that are shown, each in the colour of its source, and those that the stacking left (in red)
    for (qint32 i = 0; i < total; i++) {
        const FrameRequest::Entry &entry = request.entries[i];
        if (!entry.showDropouts || !entry.enabled || frames[i] < 0) continue;
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok || isPadded(**found, frames[i])) continue;
        drawSourceDropouts(result.image, **found, frames[i], QColor(entry.color));
    }
    if (request.residualDropouts) {
        drawDropouts(result.image, main.firstDropOuts, false, QColor(request.residualColor));
        drawDropouts(result.image, main.secondDropOuts, true, QColor(request.residualColor));
    }
    result.image = applyView(result.image, request.viewMode, request.fieldIndex);

    // The SNR of the stack is not in any metadata: it is measured from the stacked fields
    SnrAverage stackSnr;
    stackSnr.add(main.first, 1, main.videoParameters);
    stackSnr.add(main.second, 1, main.videoParameters);
    if (stackSnr.hasWhite()) result.stackWhiteSnr = stackSnr.white();
    if (stackSnr.hasBlack()) result.stackBlackSnr = stackSnr.black();
    // (and the dropouts it is left with: its fields are whole here, so they are known)
    if (!isWholeDropout(main.firstDropOuts, main.videoParameters.fieldWidth, main.videoParameters.fieldHeight)
        && !isWholeDropout(main.secondDropOuts, main.videoParameters.fieldWidth, main.videoParameters.fieldHeight)) {
        const qint64 length = main.firstDropOuts.totalLength() + main.secondDropOuts.totalLength();
        result.stackResidual = static_cast<double>(length);
    }

    emit frameReady(result);
}

// Stack a frame of the timeline from the sources that are on and have it
bool FrameBackend::stackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, StackedFrame &out, qint32 lines,
                             const QMap<QString, SourceVideo *> *videos)
{
    const qint32 total = request.entries.size();
    QVector<qint32> frames(total, -1);
    QVector<StackFrame::Input> inputs(total);
    for (qint32 i = 0; i < total; i++) {
        const FrameRequest::Entry &entry = request.entries[i];
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok || !entry.enabled) continue;
        frames[i] = sourceFrame(**found, frame, entry.offset);
        // (a thread that has handles of its own reads with them, and a source it has none for is left out: the one of the source is not its to use)
        SourceVideo *video = videos ? videos->value(entry.id, nullptr) : &(*found)->video;
        inputs[i].metadata = &(*found)->metadata;
        inputs[i].video = video;
        inputs[i].frame = frames[i];
    }

    // The sources that have the frame, are not padded, are alike in size and have their fields in the file
    StackFrame::Fields fields;
    if (!StackFrame::read(inputs, StackFrame::BothFields, lines, fields)) return false;

    // The mode: as ld-disc-stacker, auto goes by how many sources there are
    qint32 mode = request.settings.mode;
    if (mode == -1) mode = StackerCore::autoMode(inStack);

    StackerCore core;
    StackFrame::Result result;
    StackFrame::stack(core, fields, StackFrame::BothFields, frame, mode, request.settings.smartThreshold, request.settings.diffDod,
                      request.settings.passthrough, result);

    const qint32 reference = fields.usable[0];
    out.first = result.first;
    out.second = result.second;
    out.firstDropOuts = result.firstDropOuts;
    out.secondDropOuts = result.secondDropOuts;
    out.ok = true;
    out.usable = fields.usable;
    out.ownFrame = frames[reference];
    out.sourceId = request.entries[reference].id;
    out.videoParameters = fields.videoParameters;
    out.mode = mode;
    return true;
}

// The SNR of one frame of the stack, measured from the first lines of the stacked fields (the test signals are on the first 22 lines; a couple
// more for the neighbours). videos: the handles on the files that this thread has (null: those of the sources, for the thread of the backend)
StackSnrPoint FrameBackend::measureStackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, const QMap<QString, SourceVideo *> *videos)
{
    StackSnrPoint point;
    point.frame = frame;
    point.white = std::nanf("");
    point.residual = std::nanf("");
    StackedFrame stacked;
    // (the dropouts of the stack are on every line of the field, so they need the whole fields, not only the lines of the test signals)
    if (stackFrame(request, frame, inStack, stacked, request.measureResidual ? -1 : snrLines, videos)) {
        SnrAverage snr;
        snr.add(stacked.first, 1, stacked.videoParameters);
        snr.add(stacked.second, 1, stacked.videoParameters);
        if (snr.hasWhite()) point.white = static_cast<float>(snr.white());
        if (snr.hasBlack()) point.black = static_cast<float>(snr.black());
        if (request.measureResidual && !isWholeDropout(stacked.firstDropOuts, stacked.videoParameters.fieldWidth, stacked.videoParameters.fieldHeight)
            && !isWholeDropout(stacked.secondDropOuts, stacked.videoParameters.fieldWidth, stacked.videoParameters.fieldHeight)) {
            const qint64 length = stacked.firstDropOuts.totalLength() + stacked.secondDropOuts.totalLength();
            point.residual = static_cast<float>(length);
        }
    }
    return point;
}

// How many threads measure the SNR of the stack: all but one of the machine's (the one that is left is for the pictures and the window), up to eight
qint32 FrameBackend::snrThreadCount()
{
    // (half of the machine, not all but one: the measure is in the background, and a slow machine has to be left room to show the pictures)
    return qBound(1, QThread::idealThreadCount() / 2, 6);
}

// Each thread reads the files with a handle of its own (a SourceVideo is not for two threads): opened for a job, and let go of when it is done
void FrameBackend::openSnrReaders(const FrameRequest &request, qint32 threads, quint64 generation)
{
    if (snrReadersGeneration == generation && snrReaders.size() == threads) return;
    snrReaders.clear();
    snrReaders.resize(threads);
    for (qint32 t = 0; t < threads; t++) {
        for (const FrameRequest::Entry &entry: request.entries) {
            const auto found = sources.constFind(entry.id);
            if (found == sources.constEnd() || !(*found)->ok || !entry.enabled) continue;
            const LdDecodeMetaData::VideoParameters &videoParameters = (*found)->metadata.getVideoParameters();
            auto video = std::make_shared<SourceVideo>();
            video->setLenient(true);
            if (video->open((*found)->file, videoParameters.fieldWidth * videoParameters.fieldHeight, videoParameters.fieldWidth)) {
                snrReaders[t].insert(entry.id, video);
            }
        }
    }
    snrReadersGeneration = generation;
}

// The measure in the background waits for the person to be done navigating (a picture asked for in the last moments), unless it was stopped meanwhile
// Run count jobs on the given number of threads of the lowest priority, and wait for them: each thread takes the next job number until none is left or
// the generation has changed; job(thread, number) does the job on the thread that takes it
template <typename Job>
void FrameBackend::runSnrJobs(qint32 threads, qint32 count, const std::atomic<quint64> &generationNow, quint64 generation, Job job) const
{
    std::atomic<qint32> next{0};
    QVector<QThread *> workers;
    for (qint32 t = 0; t < threads; t++) {
        QThread *worker = QThread::create([this, &next, &generationNow, generation, count, t, &job]() {
            for (;;) {
                const qint32 k = next.fetch_add(1);
                if (k >= count || generation != generationNow.load()) return;
                yieldToInteraction(generationNow, generation);
                if (generation != generationNow.load()) return;
                job(t, k);
            }
        });
        workers.append(worker);
        worker->start(QThread::LowestPriority);
    }
    for (QThread *worker: workers) {
        worker->wait();
        delete worker;
    }
}

void FrameBackend::yieldToInteraction(const std::atomic<quint64> &generationNow, quint64 generation) const
{
    while (QDateTime::currentMSecsSinceEpoch() - lastInteraction.load() < 400 && generationNow.load() == generation) QThread::msleep(40);
}

// The SNR of the stack over the whole timeline.
//
// With one source the stack is that source: the window shows its SNR, and there is nothing to measure here.
//
// Otherwise the SNR of the stack is not in any metadata, so it is measured: for each frame the first lines of the fields of the sources are stacked,
// which is all the test signals need, so it costs a fraction of stacking the frames. The frames are shared between several threads, each with
// its own handles on the files. A chunk of frames at a time, so that pictures asked for in between are not kept waiting.
void FrameBackend::computeStackSnr(const FrameRequest &request, qint32 startFrame, quint64 generation)
{
    if (generation != snrGeneration.load()) return;

    // The timeline goes from the first frame of any source that is on to the last of any (a source begins where its offset puts it)
    qint32 total = 0, inStack = 0;
    for (const FrameRequest::Entry &entry: request.entries) {
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok || !entry.enabled) continue;
        inStack++;
        total = qMax(total, entry.offset + (*found)->metadata.getNumberOfFrames());
    }
    // One source: the stack is that source, whose SNR the window shows (from its metadata, or measured if that was asked for): nothing to do here
    if (total < 1 || inStack < 2) {
        snrReaders.clear();
        emit stackSnrReady(generation, QVector<StackSnrPoint>(), true);
        return;
    }

    QVector<StackSnrPoint> points;
    qint32 frame = startFrame;
    {
        const qint32 threads = snrThreadCount();
        openSnrReaders(request, threads, generation);
        // (a short chunk: the pictures asked for in between wait for it, as it is the thread of the backend that waits for its workers)
        const qint32 step = qMax(1, request.sampleStep);
        const qint32 chunk = qMax(8, threads * 4);
        const qint32 count = qMin(chunk, (total - startFrame) / step + 1);
        points.resize(count);
        runSnrJobs(threads, count, snrGeneration, generation, [&](qint32 t, qint32 k) {
            QMap<QString, SourceVideo *> videos;
            for (auto it = snrReaders[t].cbegin(); it != snrReaders[t].cend(); ++it) videos.insert(it.key(), it.value().get());
            points[k] = measureStackFrame(request, startFrame + k * step, inStack, &videos);
        });
        if (generation != snrGeneration.load()) return;
        frame = startFrame + count * step;
    }

    const bool finished = frame > total;
    emit stackSnrReady(generation, points, finished);
    if (finished) {
        snrReaders.clear();
        snrReadersGeneration = 0;
    } else {
        QMetaObject::invokeMethod(this, "computeStackSnr", Qt::QueuedConnection, Q_ARG(FrameRequest, request),
                                  Q_ARG(qint32, frame), Q_ARG(quint64, generation));
    }
}

// The SNR of one frame of a source, measured from the first lines of its own fields, with a handle on the file that the thread has
StackSnrPoint FrameBackend::measureSourceFrame(Loaded &source, SourceVideo &video, qint32 frame)
{
    StackSnrPoint point;
    point.frame = frame;
    point.white = std::nanf("");
    point.black = std::nanf("");
    const qint32 first = source.metadata.getFirstFieldNumber(frame);
    const qint32 second = source.metadata.getSecondFieldNumber(frame);
    const qint32 available = video.getNumberOfAvailableFields();
    if (first < 1 || second < 1 || first > available || second > available) return point;
    LdDecodeMetaData::VideoParameters videoParameters = source.metadata.getVideoParameters();
    const qint32 readLines = qMin(snrLines, videoParameters.fieldHeight);
    videoParameters.fieldHeight = readLines;
    const qint32 expectedSize = videoParameters.fieldWidth * readLines;
    // (in the order of the file, to save seeking)
    SourceVideo::Data firstData, secondData;
    if (first < second) {
        firstData = video.getVideoField(first, 1, readLines);
        secondData = video.getVideoField(second, 1, readLines);
    } else {
        secondData = video.getVideoField(second, 1, readLines);
        firstData = video.getVideoField(first, 1, readLines);
    }
    if (firstData.size() != expectedSize || secondData.size() != expectedSize) return point;
    SnrAverage snr;
    snr.add(firstData, 1, videoParameters);
    snr.add(secondData, 1, videoParameters);
    if (snr.hasWhite()) point.white = static_cast<float>(snr.white());
    if (snr.hasBlack()) point.black = static_cast<float>(snr.black());
    return point;
}

// The SNR of one source over its own frames, measured instead of read from the metadata: shared between several threads, each with a handle of its
// own on the file, a chunk of frames at a time (so that pictures asked for in between are not kept waiting)
void FrameBackend::computeSourceSnr(const QString &id, qint32 startFrame, quint64 generation, qint32 step)
{
    if (generation != sourceSnrGeneration.load()) return;
    const auto found = sources.constFind(id);
    if (found == sources.constEnd() || !(*found)->ok || (*found)->metadata.getNumberOfFrames() < startFrame) {
        sourceReaders.clear();
        emit sourceSnrReady(id, generation, QVector<StackSnrPoint>(), true);
        return;
    }
    Loaded &source = **found;
    const qint32 total = source.metadata.getNumberOfFrames();
    const qint32 threads = snrThreadCount();
    if (sourceReadersGeneration != generation || sourceReaders.size() != threads) {
        sourceReaders.clear();
        const LdDecodeMetaData::VideoParameters &videoParameters = source.metadata.getVideoParameters();
        for (qint32 t = 0; t < threads; t++) {
            auto video = std::make_shared<SourceVideo>();
            video->setLenient(true);
            if (!video->open(source.file, videoParameters.fieldWidth * videoParameters.fieldHeight, videoParameters.fieldWidth)) break;
            sourceReaders.append(video);
        }
        sourceReadersGeneration = generation;
        if (sourceReaders.isEmpty()) {
            emit sourceSnrReady(id, generation, QVector<StackSnrPoint>(), true);
            return;
        }
    }

    const qint32 workers = sourceReaders.size();
    step = qMax(1, step);
    const qint32 count = qMin(qMax(8, workers * 4), (total - startFrame) / step + 1);
    QVector<StackSnrPoint> points(count);
    runSnrJobs(workers, count, sourceSnrGeneration, generation, [&](qint32 t, qint32 k) {
        points[k] = measureSourceFrame(source, *sourceReaders[t], startFrame + k * step);
    });
    if (generation != sourceSnrGeneration.load()) return;

    const qint32 frame = startFrame + count * step;
    const bool finished = frame > total;
    emit sourceSnrReady(id, generation, points, finished);
    if (finished) {
        sourceReaders.clear();
        sourceReadersGeneration = 0;
    } else {
        QMetaObject::invokeMethod(this, "computeSourceSnr", Qt::QueuedConnection, Q_ARG(QString, id), Q_ARG(qint32, frame), Q_ARG(quint64, generation), Q_ARG(qint32, step));
    }
}
