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

#include "stackedvideo.h"

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

namespace {

// The SNR of a frame from its two fields: the average of those that have one
float frameSnr(const LdDecodeMetaData::Field &first, const LdDecodeMetaData::Field &second, bool white)
{
    double total = 0;
    qint32 points = 0;
    for (const LdDecodeMetaData::Field *field: { &first, &second }) {
        const double value = white ? field->vitsMetrics.wSNR : field->vitsMetrics.bPSNR;
        if (std::isfinite(value) && value > 0) {
            total += value;
            points++;
        }
    }
    return points ? static_cast<float>(total / points) : std::nanf("");
}

// The SNR of a stacked field, measured from its test signal lines
void measureField(const SourceVideo::Data &field, const LdDecodeMetaData::VideoParameters &videoParameters,
                  double &whiteTotal, qint32 &whitePoints, double &blackTotal, qint32 &blackPoints)
{
    LdDecodeMetaData::Field metrics;
    const VitsRawMeasurements raw = measureVitsRaw(field, 1, videoParameters);
    applyVitsLevels(raw, videoParameters, metrics, vitsIreSpan(videoParameters.system));
    if (std::isfinite(metrics.vitsMetrics.wSNR) && metrics.vitsMetrics.wSNR > 0) { whiteTotal += metrics.vitsMetrics.wSNR; whitePoints++; }
    if (std::isfinite(metrics.vitsMetrics.bPSNR) && metrics.vitsMetrics.bPSNR > 0) { blackTotal += metrics.vitsMetrics.bPSNR; blackPoints++; }
}

} // namespace

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
        // (a frame whose fields cannot be found has no SNR)
        if (firstNumber < 1 || secondNumber < 1) {
            info.paddedFrames++;
            continue;
        }
        const LdDecodeMetaData::Field &first = loaded->metadata.getField(firstNumber);
        const LdDecodeMetaData::Field &second = loaded->metadata.getField(secondNumber);
        if (first.pad || second.pad) info.paddedFrames++;

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
        info.whiteSnr[frame - 1] = frameSnr(first, second, true);
        info.blackSnr[frame - 1] = frameSnr(first, second, false);
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
    // A source shifted by +2 starts two frames later on the timeline of the stack
    const qint32 own = frame - offset;
    if (own < 1 || own > source.metadata.getNumberOfFrames()) return -1;
    return own;
}

// A frame with a padded (missing) field is not one the stack can use
bool FrameBackend::isPadded(Loaded &source, qint32 ownFrame)
{
    const qint32 first = source.metadata.getFirstFieldNumber(ownFrame);
    const qint32 second = source.metadata.getSecondFieldNumber(ownFrame);
    if (first < 1 || second < 1) return true;
    return source.metadata.getField(first).pad || source.metadata.getField(second).pad;
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

    if (videoParameters.system == PAL || videoParameters.system == PAL_M) palColour.updateConfiguration(videoParameters, palConfiguration);
    else ntscColour.updateConfiguration(videoParameters, ntscConfiguration);
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
                result.image = applyView(result.image, request.viewMode, request.fieldIndex);
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
    result.image = applyView(result.image, request.viewMode, request.fieldIndex);

    // The SNR of the stack is not in any metadata: it is measured from the stacked fields
    double whiteTotal = 0, blackTotal = 0;
    qint32 whitePoints = 0, blackPoints = 0;
    measureField(main.first, main.videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    measureField(main.second, main.videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    if (whitePoints) result.stackWhiteSnr = whiteTotal / whitePoints;
    if (blackPoints) result.stackBlackSnr = blackTotal / blackPoints;

    emit frameReady(result);
}

// Stack a frame of the timeline from the sources that are on and have it
bool FrameBackend::stackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, StackedFrame &out, qint32 lines,
                             const QMap<QString, SourceVideo *> *videos)
{
    const qint32 total = request.entries.size();
    QVector<qint32> frames(total, -1);
    QVector<qint32> available;
    for (qint32 i = 0; i < total; i++) {
        const FrameRequest::Entry &entry = request.entries[i];
        const auto found = sources.constFind(entry.id);
        if (found == sources.constEnd() || !(*found)->ok || !entry.enabled) continue;
        frames[i] = sourceFrame(**found, frame, entry.offset);
        if (frames[i] >= 0 && !isPadded(**found, frames[i])) available.append(i);
    }
    if (available.isEmpty()) return false;

    // The sources must be alike: those that are not the size of the first are left out
    LdDecodeMetaData::VideoParameters videoParameters = sources.value(request.entries[available[0]].id)->metadata.getVideoParameters();
    QVector<qint32> usable;
    for (qint32 i: available) {
        const LdDecodeMetaData::VideoParameters &other = sources.value(request.entries[i].id)->metadata.getVideoParameters();
        if (other.fieldWidth == videoParameters.fieldWidth && other.fieldHeight == videoParameters.fieldHeight) usable.append(i);
    }

    // Only the first lines, when that is all that is wanted (the stacking sees the fields as that short)
    const qint32 readLines = lines > 0 ? qMin(lines, videoParameters.fieldHeight) : -1;
    if (readLines > 0) videoParameters.fieldHeight = readLines;
    const auto readField = [readLines](SourceVideo &video, qint32 field) {
        return readLines > 0 ? video.getVideoField(field, 1, readLines) : video.getVideoField(field);
    };

    QVector<SourceVideo::Data> firstData(total), secondData(total);
    QVector<LdDecodeMetaData::Field> firstMetadata(total), secondMetadata(total);
    const qint32 expectedSize = videoParameters.fieldWidth * (readLines > 0 ? readLines : videoParameters.fieldHeight);
    QVector<qint32> good;
    for (qint32 i: usable) {
        Loaded &source = *sources.value(request.entries[i].id);
        // (a thread that has handles of its own reads with them, and a source it has none for is left out: the one of the source is not its to use)
        SourceVideo *video = &source.video;
        if (videos) {
            video = videos->value(request.entries[i].id, nullptr);
            if (!video) continue;
        }
        const qint32 first = source.metadata.getFirstFieldNumber(frames[i]);
        const qint32 second = source.metadata.getSecondFieldNumber(frames[i]);
        const qint32 available = video->getNumberOfAvailableFields();
        // The metadata can name fields that the file does not hold
        if (first < 1 || second < 1 || first > available || second > available) continue;
        // In the order of the file, to save seeking
        if (first < second) {
            firstData[i] = readField(*video, first);
            secondData[i] = readField(*video, second);
        } else {
            secondData[i] = readField(*video, second);
            firstData[i] = readField(*video, first);
        }
        if (firstData[i].size() != expectedSize || secondData[i].size() != expectedSize) continue;
        firstMetadata[i] = source.metadata.getField(first);
        secondMetadata[i] = source.metadata.getField(second);
        good.append(i);
    }
    usable = good;
    if (usable.isEmpty()) return false;

    // The mode: as ld-disc-stacker, auto goes by how many sources there are
    qint32 mode = request.settings.mode;
    if (mode == -1) mode = StackerCore::autoMode(inStack);

    StackerCore core;
    out.first = SourceVideo::Data(firstData[usable[0]].size());
    out.second = SourceVideo::Data(secondData[usable[0]].size());
    DropOuts firstDropOuts, secondDropOuts;
    const bool noDiffDod = !request.settings.diffDod;
    core.stackField(frame, firstData, videoParameters, firstMetadata, usable, noDiffDod,
                    request.settings.passthrough, out.first, firstDropOuts, mode,
                    request.settings.smartThreshold * 256, false);
    core.stackField(frame, secondData, videoParameters, secondMetadata, usable, noDiffDod,
                    request.settings.passthrough, out.second, secondDropOuts, mode,
                    request.settings.smartThreshold * 256, false);

    out.ok = true;
    out.usable = usable;
    out.ownFrame = frames[usable[0]];
    out.sourceId = request.entries[usable[0]].id;
    out.videoParameters = videoParameters;
    out.mode = mode;
    return true;
}

// The SNR of one frame of the stack, measured from the first lines of the stacked fields (the test signals are on the first 22 lines; a couple
// more for the neighbours). videos: the handles on the files that this thread has (null: those of the sources, for the thread of the backend)
StackSnrPoint FrameBackend::measureStackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, const QMap<QString, SourceVideo *> *videos)
{
    constexpr qint32 vitsLines = 24;
    StackSnrPoint point;
    point.frame = frame;
    point.white = std::nanf("");
    point.black = std::nanf("");
    StackedFrame stacked;
    if (stackFrame(request, frame, inStack, stacked, vitsLines, videos)) {
        double whiteTotal = 0, blackTotal = 0;
        qint32 whitePoints = 0, blackPoints = 0;
        measureField(stacked.first, stacked.videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
        measureField(stacked.second, stacked.videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
        if (whitePoints) point.white = static_cast<float>(whiteTotal / whitePoints);
        if (blackPoints) point.black = static_cast<float>(blackTotal / blackPoints);
    }
    return point;
}

// How many threads measure the SNR of the stack: all but one of the machine's (the one that is left is for the pictures and the window), up to eight
qint32 FrameBackend::snrThreadCount()
{
    return qBound(1, QThread::idealThreadCount() - 1, 8);
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
            if (video->open((*found)->file, videoParameters.fieldWidth * videoParameters.fieldHeight, videoParameters.fieldWidth)) {
                snrReaders[t].insert(entry.id, video);
            }
        }
    }
    snrReadersGeneration = generation;
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
        const qint32 chunk = qMax(40, threads * 12);
        const qint32 count = qMin(chunk, total - startFrame + 1);
        points.resize(count);
        std::atomic<qint32> next{0};
        QVector<QThread *> workers;
        for (qint32 t = 0; t < threads; t++) {
            QMap<QString, SourceVideo *> videos;
            for (auto it = snrReaders[t].cbegin(); it != snrReaders[t].cend(); ++it) videos.insert(it.key(), it.value().get());
            QThread *worker = QThread::create([this, &request, &points, &next, videos, count, startFrame, inStack, generation]() {
                for (;;) {
                    const qint32 k = next.fetch_add(1);
                    if (k >= count || generation != snrGeneration.load()) return;
                    points[k] = measureStackFrame(request, startFrame + k, inStack, &videos);
                }
            });
            workers.append(worker);
            worker->start();
        }
        for (QThread *worker: workers) {
            worker->wait();
            delete worker;
        }
        if (generation != snrGeneration.load()) return;
        frame = startFrame + count;
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
    constexpr qint32 vitsLines = 24;
    StackSnrPoint point;
    point.frame = frame;
    point.white = std::nanf("");
    point.black = std::nanf("");
    const qint32 first = source.metadata.getFirstFieldNumber(frame);
    const qint32 second = source.metadata.getSecondFieldNumber(frame);
    const qint32 available = video.getNumberOfAvailableFields();
    if (first < 1 || second < 1 || first > available || second > available) return point;
    LdDecodeMetaData::VideoParameters videoParameters = source.metadata.getVideoParameters();
    const qint32 readLines = qMin(vitsLines, videoParameters.fieldHeight);
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
    double whiteTotal = 0, blackTotal = 0;
    qint32 whitePoints = 0, blackPoints = 0;
    measureField(firstData, videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    measureField(secondData, videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    if (whitePoints) point.white = static_cast<float>(whiteTotal / whitePoints);
    if (blackPoints) point.black = static_cast<float>(blackTotal / blackPoints);
    return point;
}

// The SNR of one source over its own frames, measured instead of read from the metadata: shared between several threads, each with a handle of its
// own on the file, a chunk of frames at a time (so that pictures asked for in between are not kept waiting)
void FrameBackend::computeSourceSnr(const QString &id, qint32 startFrame, quint64 generation)
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
    const qint32 count = qMin(qMax(40, workers * 12), total - startFrame + 1);
    QVector<StackSnrPoint> points(count);
    std::atomic<qint32> next{0};
    QVector<QThread *> threadList;
    for (qint32 t = 0; t < workers; t++) {
        SourceVideo *video = sourceReaders[t].get();
        QThread *worker = QThread::create([this, &source, &points, &next, video, count, startFrame, generation]() {
            for (;;) {
                const qint32 k = next.fetch_add(1);
                if (k >= count || generation != sourceSnrGeneration.load()) return;
                points[k] = measureSourceFrame(source, *video, startFrame + k);
            }
        });
        threadList.append(worker);
        worker->start();
    }
    for (QThread *worker: threadList) {
        worker->wait();
        delete worker;
    }
    if (generation != sourceSnrGeneration.load()) return;

    const qint32 frame = startFrame + count;
    const bool finished = frame > total;
    emit sourceSnrReady(id, generation, points, finished);
    if (finished) {
        sourceReaders.clear();
        sourceReadersGeneration = 0;
    } else {
        QMetaObject::invokeMethod(this, "computeSourceSnr", Qt::QueuedConnection, Q_ARG(QString, id), Q_ARG(qint32, frame), Q_ARG(quint64, generation));
    }
}
