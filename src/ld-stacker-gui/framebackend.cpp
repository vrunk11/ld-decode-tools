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

#include <QFileInfo>
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

    auto loaded = std::make_shared<Loaded>();

    const QString metadataFile = LdDecodeMetaData::findMetadataFile(file);
    info.metadataFile = metadataFile;
    if (metadataFile.isEmpty() || !loaded->metadata.read(metadataFile)) {
        info.error = tr("Could not load the metadata of %1").arg(QFileInfo(file).fileName());
        emit sourceLoaded(info);
        return;
    }

    const LdDecodeMetaData::VideoParameters &videoParameters = loaded->metadata.getVideoParameters();
    if (!loaded->video.open(file, videoParameters.fieldWidth * videoParameters.fieldHeight)) {
        info.error = tr("Could not open %1").arg(QFileInfo(file).fileName());
        emit sourceLoaded(info);
        return;
    }
    loaded->metadata.setIsFirstFieldFirst(!reverseFields);
    loaded->ok = true;

    info.ok = true;
    info.system = videoParameters.system;
    info.fieldWidth = videoParameters.fieldWidth;
    info.fieldHeight = videoParameters.fieldHeight;
    info.frames = loaded->metadata.getNumberOfFrames();
    info.whiteSnr.resize(info.frames);
    info.blackSnr.resize(info.frames);
    for (qint32 frame = 1; frame <= info.frames; frame++) {
        const LdDecodeMetaData::Field &first = loaded->metadata.getField(loaded->metadata.getFirstFieldNumber(frame));
        const LdDecodeMetaData::Field &second = loaded->metadata.getField(loaded->metadata.getSecondFieldNumber(frame));
        info.whiteSnr[frame - 1] = frameSnr(first, second, true);
        info.blackSnr[frame - 1] = frameSnr(first, second, false);
    }

    sources.insert(id, loaded);
    emit sourceLoaded(info);
}

void FrameBackend::removeSource(const QString &id)
{
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
QImage FrameBackend::chromaPicture(Loaded &source, qint32 ownFrame, const SourceVideo::Data *stackedFirst, const SourceVideo::Data *stackedSecond)
{
    const LdDecodeMetaData::VideoParameters videoParameters = source.metadata.getVideoParameters();
    configureDecoders(videoParameters);
    const bool isPal = videoParameters.system == PAL || videoParameters.system == PAL_M;

    const qint32 lookBehind = isPal ? palConfiguration.getLookBehind() : ntscConfiguration.getLookBehind();
    const qint32 lookAhead = isPal ? palConfiguration.getLookAhead() : ntscConfiguration.getLookAhead();

    QVector<SourceField> fields;
    qint32 startIndex, endIndex;
    SourceField::loadFields(source.video, source.metadata, ownFrame, 1, lookBehind, lookAhead, fields, startIndex, endIndex);

    if (stackedFirst && stackedSecond && endIndex - startIndex == 2) {
        fields[startIndex].data = *stackedFirst;
        fields[startIndex + 1].data = *stackedSecond;
        if (isPal && videoParameters.isSubcarrierLocked) {
            // As the fields are read for the decoder: 4fSC PAL has the second field two samples out
            fields[startIndex + 1].data.remove(0, 2);
            for (qint32 i = 0; i < 2; i++) fields[startIndex + 1].data.append(videoParameters.black16bIre);
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
    QVector<qint32> available;
    qint32 reference = -1;
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
                result.image = request.chroma ? chromaPicture(source, frames[i], nullptr, nullptr)
                                              : weave(firstData, secondData, videoParameters.fieldWidth, videoParameters.fieldHeight);
            }
        }
        emit frameReady(result);
        return;
    }

    // The stack
    for (qint32 i = 0; i < total; i++) {
        if (request.entries[i].enabled && frames[i] >= 0) {
            if (reference < 0) reference = i;
            available.append(i);
        }
    }

    if (available.isEmpty()) {
        result.message = result.inStack ? tr("No source has a frame here") : tr("No source is on");
        emit frameReady(result);
        return;
    }

    // The sources must be alike: those that are not the size of the first are left out
    const LdDecodeMetaData::VideoParameters videoParameters = sources.value(request.entries[reference].id)->metadata.getVideoParameters();
    QVector<qint32> usable;
    for (qint32 i: available) {
        const LdDecodeMetaData::VideoParameters &other = sources.value(request.entries[i].id)->metadata.getVideoParameters();
        if (other.fieldWidth == videoParameters.fieldWidth && other.fieldHeight == videoParameters.fieldHeight) usable.append(i);
    }

    QVector<SourceVideo::Data> firstData(total), secondData(total);
    QVector<LdDecodeMetaData::Field> firstMetadata(total), secondMetadata(total);
    for (qint32 i: usable) {
        Loaded &source = *sources.value(request.entries[i].id);
        const qint32 first = source.metadata.getFirstFieldNumber(frames[i]);
        const qint32 second = source.metadata.getSecondFieldNumber(frames[i]);
        // In the order of the file, to save seeking
        if (first < second) {
            firstData[i] = source.video.getVideoField(first);
            secondData[i] = source.video.getVideoField(second);
        } else {
            secondData[i] = source.video.getVideoField(second);
            firstData[i] = source.video.getVideoField(first);
        }
        firstMetadata[i] = source.metadata.getField(first);
        secondMetadata[i] = source.metadata.getField(second);
    }

    // The mode: as ld-disc-stacker, auto goes by how many sources there are
    qint32 mode = request.settings.mode;
    if (mode == -1) mode = StackerCore::autoMode(result.inStack);
    result.modeUsed = mode;
    result.contributing = usable.size();

    StackerCore core;
    SourceVideo::Data outputFirst(firstData[usable[0]].size());
    SourceVideo::Data outputSecond(secondData[usable[0]].size());
    DropOuts outputFirstDropOuts, outputSecondDropOuts;
    const bool noDiffDod = !request.settings.diffDod;
    core.stackField(request.frame, firstData, videoParameters, firstMetadata, usable, noDiffDod,
                    request.settings.passthrough, outputFirst, outputFirstDropOuts, mode,
                    request.settings.smartThreshold, false);
    core.stackField(request.frame, secondData, videoParameters, secondMetadata, usable, noDiffDod,
                    request.settings.passthrough, outputSecond, outputSecondDropOuts, mode,
                    request.settings.smartThreshold, false);

    if (request.chroma) {
        Loaded &referenceSource = *sources.value(request.entries[usable[0]].id);
        result.image = chromaPicture(referenceSource, frames[usable[0]], &outputFirst, &outputSecond);
    } else {
        result.image = weave(outputFirst, outputSecond, videoParameters.fieldWidth, videoParameters.fieldHeight);
    }

    // The SNR of the stack is not in any metadata: it is measured from the stacked fields
    double whiteTotal = 0, blackTotal = 0;
    qint32 whitePoints = 0, blackPoints = 0;
    measureField(outputFirst, videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    measureField(outputSecond, videoParameters, whiteTotal, whitePoints, blackTotal, blackPoints);
    if (whitePoints) result.stackWhiteSnr = whiteTotal / whitePoints;
    if (blackPoints) result.stackBlackSnr = blackTotal / blackPoints;

    emit frameReady(result);
}
