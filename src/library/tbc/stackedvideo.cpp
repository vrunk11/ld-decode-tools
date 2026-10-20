/******************************************************************************
 * stackedvideo.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "stackedvideo.h"

#include <QFileInfo>
#include <QHash>
#include <limits>

#include "ldxcapture.h"
#include "tbcfiles.h"
#include "vbidecoder.h"

#include "tbc/logging.h"

StackedVideo::StackedVideo()
{
    // The fields of a few frames, which a viewer or a decoder asks for again and again
    cache.setMaxCost(64);
}

StackedVideo::~StackedVideo() = default;

// The reference is the first source that is on: its metadata is the stack's
// Whether the metadata of a capture are there: in a file, or in an LDX file
static bool metadataThere(const QString &metadataFile)
{
    if (LdxCapture::isLdxName(metadataFile)) {
        QString path;
        qint32 track;
        LdxCapture::split(metadataFile, path, track);
        return QFileInfo::exists(path);
    }
    return QFileInfo::exists(metadataFile);
}

static const StackSource *referenceSource(const StackProject &project)
{
    for (const StackSource &source: project.sources) {
        if (source.enabled) return &source;
    }
    return nullptr;
}

qint32 StackedVideo::discOrigin(LdDecodeMetaData &metadata)
{
    VbiDecoder vbiDecoder;
    const qint32 framesPerSecond = metadata.getVideoParameters().system == PAL ? 25 : 30;
    const qint32 frames = metadata.getNumberOfFrames();
    QHash<qint32, qint32> votes;
    qint32 cast = 0;
    for (qint32 frame = 1; frame <= frames && cast < 600; frame++) {
        const qint32 first = metadata.getFirstFieldNumber(frame);
        const qint32 second = metadata.getSecondFieldNumber(frame);
        if (first < 1 || second < 1) continue;
        const LdDecodeMetaData::Field &a = metadata.getField(first);
        const LdDecodeMetaData::Field &b = metadata.getField(second);
        const VbiDecoder::Vbi vbi = vbiDecoder.decodeFrame(a.vbi.vbiData[0], a.vbi.vbiData[1], a.vbi.vbiData[2],
                                                           b.vbi.vbiData[0], b.vbi.vbiData[1], b.vbi.vbiData[2]);
        qint32 number = -1;
        if (vbi.picNo != -1) number = vbi.picNo;
        else if (vbi.clvHr != -1 && vbi.clvPicNo != -1) number = (vbi.clvHr * 3600 + vbi.clvMin * 60 + vbi.clvSec) * framesPerSecond + vbi.clvPicNo;
        if (number < 0) continue;
        votes[number - (frame - 1)]++;
        cast++;
    }
    qint32 best = 0, origin = -1;
    for (auto it = votes.cbegin(); it != votes.cend(); ++it) {
        if (it.value() > best || (it.value() == best && it.key() < origin)) {
            best = it.value();
            origin = it.key();
        }
    }
    return origin;
}

QVector<qint32> StackedVideo::alignedPositions(const QVector<qint32> &origins, const QVector<qint32> &offsets, const QVector<qint32> &frames, qint32 &total, qint32 *discAtStart)
{
    const qint32 count = origins.size();
    bool numbered = false;
    for (qint32 origin: origins) numbered = numbered || origin >= 0;
    QVector<qint32> positions(count, 0);
    qint32 lowest = std::numeric_limits<qint32>::max();
    for (qint32 i = 0; i < count; i++) {
        positions[i] = qMax(0, origins[i]) + offsets[i];
        lowest = qMin(lowest, positions[i]);
    }
    if (count == 0) lowest = 0;
    const qint32 startOfTimeline = lowest;
    total = 0;
    for (qint32 i = 0; i < count; i++) {
        positions[i] -= lowest;
        total = qMax(total, positions[i] + frames[i]);
    }
    if (discAtStart) *discAtStart = numbered ? startOfTimeline : -1;
    return positions;
}

// What a source is read from: its mapping (ld-discmap) if it has one that exists, the file beside it with _map in its name or the track of an LDX file that is the
// mapping of it, unless the metadata of the source say it is mapped already; the source as it is when it has none. Nothing is made here: the mapping is made by
// ld-discmap (or ld-stacker-gui), and a source that has none is used as it is.
static QString resolvedReference(const StackSource &source)
{
    const QString reference = source.reference();
    QString mapped;
    if (LdxCapture::isLdxName(reference)) {
        mapped = LdxCapture::mappedOfOriginal(reference);
    } else {
        const QString name = TbcFiles::mappedFileName(reference);
        if (!name.isEmpty() && QFileInfo::exists(name)) mapped = name;
    }
    if (mapped.isEmpty() || mapped == reference) return reference;
    if (!metadataThere(LdDecodeMetaData::findMetadataFile(mapped))) return reference;
    LdDecodeMetaData own;
    const QString ownMetadata = LdDecodeMetaData::findMetadataFile(reference);
    if (metadataThere(ownMetadata) && own.read(ownMetadata) && own.getVideoParameters().isMapped) return reference;
    return mapped;
}

bool StackedVideo::readMetadata(const QString &stackFile, LdDecodeMetaData &metadata, QString &error)
{
    StackProject session;
    if (!session.load(stackFile, error)) return false;

    const StackSource *reference = referenceSource(session);
    if (!reference) {
        error = QObject::tr("No source is on in %1").arg(stackFile);
        return false;
    }

    const QString referenceName = resolvedReference(*reference);
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(referenceName);
    if (!metadataThere(metadataFile) || !metadata.read(metadataFile)) {
        error = QObject::tr("Could not read the metadata of %1").arg(referenceName);
        return false;
    }
    metadata.setIsFirstFieldFirst(!session.stacking.reverseFields);

    // The other sources that are on, to line them up with it
    std::vector<std::unique_ptr<LdDecodeMetaData>> others;
    QVector<qint32> origins, offsets, frames;
    origins.append(discOrigin(metadata));
    offsets.append(reference->offset);
    frames.append(metadata.getNumberOfFrames());
    bool afterReference = false;
    for (const StackSource &entry: session.sources) {
        if (!entry.enabled) continue;
        if (!afterReference) {
            afterReference = true;
            continue;
        }
        auto other = std::make_unique<LdDecodeMetaData>();
        const QString otherName = resolvedReference(entry);
        const QString otherFile = LdDecodeMetaData::findMetadataFile(otherName);
        if (!metadataThere(otherFile) || !other->read(otherFile)) {
            error = QObject::tr("Could not read the metadata of %1").arg(otherName);
            return false;
        }
        other->setIsFirstFieldFirst(!session.stacking.reverseFields);
        origins.append(discOrigin(*other));
        offsets.append(entry.offset);
        frames.append(other->getNumberOfFrames());
        others.push_back(std::move(other));
    }
    qint32 total = 0;
    const QVector<qint32> positions = alignedPositions(origins, offsets, frames, total);

    // The fields of the timeline: those of the first source that has the frame (with no dropouts, which the stack fills, and no VITS measurements, which are
    // those of the source and not of the stack), a padded one where none has
    const bool firstFirst = !session.stacking.reverseFields;
    QVector<LdDecodeMetaData::Field> timeline;
    timeline.reserve(total * 2);
    for (qint32 frame = 1; frame <= total; frame++) {
        LdDecodeMetaData::Field pair[2];
        bool found = false;
        for (qint32 i = 0; i < frames.size() && !found; i++) {
            const qint32 own = frame - positions[i];
            if (own < 1 || own > frames[i]) continue;
            LdDecodeMetaData &from = i == 0 ? metadata : *others[i - 1];
            const qint32 a = from.getFirstFieldNumber(own);
            const qint32 b = from.getSecondFieldNumber(own);
            if (a < 1 || b < 1) continue;
            pair[0] = from.getField(a);
            pair[1] = from.getField(b);
            if (pair[0].pad || pair[1].pad) continue;
            found = true;
        }
        if (!found) {
            pair[0] = LdDecodeMetaData::Field();
            pair[1] = LdDecodeMetaData::Field();
            pair[0].pad = pair[1].pad = true;
            pair[0].audioSamples = pair[1].audioSamples = 0;
        }
        pair[0].isFirstField = true;
        pair[1].isFirstField = false;
        for (LdDecodeMetaData::Field &field: pair) {
            field.dropOuts = DropOuts();
            field.vitsMetrics = LdDecodeMetaData::VitsMetrics();
        }
        timeline.append(firstFirst ? pair[0] : pair[1]);
        timeline.append(firstFirst ? pair[1] : pair[0]);
    }
    metadata.clearFields();
    for (const LdDecodeMetaData::Field &field: timeline) metadata.appendField(field);
    const qint32 fields = metadata.getNumberOfFields();

    // NTSC fields come in a sequence of 4 phases: the padded fields of the reference may have broken it, and
    // ld-disc-stacker puts it right in what it writes
    if (metadata.getVideoParameters().system == NTSC) {
        constexpr qint32 PHASE_COUNT = 4;
        qint32 pivot = 1;
        while (pivot <= fields && metadata.getField(pivot).pad) pivot++;
        if (pivot < fields) {
            qint32 phase = metadata.getField(pivot).fieldPhaseID - 1;
            phase -= (pivot - 1) % PHASE_COUNT;
            phase += PHASE_COUNT;
            phase %= PHASE_COUNT;
            for (qint32 fieldNumber = 1; fieldNumber <= fields; fieldNumber++) {
                LdDecodeMetaData::Field field = metadata.getField(fieldNumber);
                field.fieldPhaseID = phase + 1;
                metadata.updateField(field, fieldNumber);
                phase = (phase + 1) % PHASE_COUNT;
            }
        }
    }
    return true;
}

bool StackedVideo::open(const QString &stackFile, QString &error)
{
    if (!project.load(stackFile, error)) return false;

    sources.clear();
    bool firstIsCvbs = false;
    for (const StackSource &entry: project.sources) {
        if (!entry.enabled) continue;
        const QString name = resolvedReference(entry);

        // TBC and CVBS are not mixed
        const bool isCvbs = Cvbs::isDataFileName(StackProject::referenceFile(name));
        if (sources.empty()) {
            firstIsCvbs = isCvbs;
        } else if (isCvbs != firstIsCvbs) {
            error = QObject::tr("%1 is not of the same format (TBC or CVBS) as the first source: they cannot be stacked together").arg(name);
            return false;
        }

        auto source = std::make_unique<Source>();
        source->offset = entry.offset;    // (the position on the timeline is worked out below)

        const QString metadataFile = LdDecodeMetaData::findMetadataFile(name);
        if (!metadataThere(metadataFile) || !source->metadata.read(metadataFile)) {
            error = QObject::tr("Could not read the metadata of %1").arg(name);
            return false;
        }
        source->metadata.setIsFirstFieldFirst(!project.stacking.reverseFields);

        const LdDecodeMetaData::VideoParameters &parameters = source->metadata.getVideoParameters();
        if (sources.empty()) {
            fieldWidth = parameters.fieldWidth;
            fieldHeight = parameters.fieldHeight;
            fieldLength = fieldWidth * fieldHeight;
        } else {
            const LdDecodeMetaData::VideoParameters &first = sources.front()->metadata.getVideoParameters();
            if (parameters.fieldWidth != fieldWidth || parameters.fieldHeight != fieldHeight || parameters.system != first.system) {
                error = QObject::tr("%1 is not of the same video system and size as the first source").arg(name);
                return false;
            }
        }

        // (with the line length, so that only the first lines of a field can be asked for)
        if (!source->video.open(name, fieldLength, fieldWidth)) {
            error = QObject::tr("Could not open %1").arg(name);
            return false;
        }
        sources.push_back(std::move(source));
    }
    if (sources.empty()) {
        error = QObject::tr("No source is on in %1").arg(stackFile);
        return false;
    }

    // As ld-disc-stacker: auto goes by how many sources there are
    mode = project.stacking.mode;
    if (mode == -1) mode = StackerCore::autoMode(static_cast<qint32>(sources.size()));

    // Where each source is on the timeline, by the numbers of the disc and then by its offset
    QVector<qint32> origins, offsets, frames;
    for (const std::unique_ptr<Source> &source: sources) {
        origins.append(discOrigin(source->metadata));
        offsets.append(source->offset);
        frames.append(source->metadata.getNumberOfFrames());
    }
    const QVector<qint32> placed = alignedPositions(origins, offsets, frames, timelineFrames);
    for (qint32 i = 0; i < static_cast<qint32>(sources.size()); i++) sources[i]->offset = placed[i];

    // Which frame of the timeline each of its fields is of (the fields of a frame are in the order of the metadata that readMetadata makes)
    positions = QVector<FieldPosition>(timelineFrames * 2 + 1);
    for (qint32 frame = 1; frame <= timelineFrames; frame++) {
        const qint32 a = frame * 2 - 1;
        positions[a] = { frame, !project.stacking.reverseFields };
        positions[a + 1] = { frame, project.stacking.reverseFields };
    }
    return true;
}

qint32 StackedVideo::getNumberOfFields()
{
    return timelineFrames * 2;
}

qint32 StackedVideo::sourceFrame(Source &source, qint32 frame, bool isReference) const
{
    (void)isReference;
    const qint32 own = frame - source.offset;
    if (own < 1 || own > source.metadata.getNumberOfFrames()) return -1;
    return own;
}

SourceVideo::Data StackedVideo::getField(qint32 fieldNumber, qint32 startLine, qint32 endLine)
{
    const bool whole = startLine == -1 && endLine == -1;
    if (whole) {
        if (const SourceVideo::Data *cached = cache.object(fieldNumber)) return *cached;
    }

    // The test signal lines are enough for what only reads those: a couple more than asked for, for the
    // stacking that looks at the lines around
    const qint32 lines = whole ? fieldHeight : qMin(fieldHeight, endLine + 2);
    SourceVideo::Data stacked = stackField(fieldNumber, lines);

    if (whole) {
        cache.insert(fieldNumber, new SourceVideo::Data(stacked), 1);
        return stacked;
    }
    return stacked.mid((startLine - 1) * fieldWidth, (endLine - startLine + 1) * fieldWidth);
}

// A field of the stack, of the first lines of the fields of the sources
SourceVideo::Data StackedVideo::stackField(qint32 fieldNumber, qint32 lines)
{
    const auto read = [lines, this](Source &source, qint32 number) {
        return lines < fieldHeight ? source.video.getVideoField(number, 1, lines) : source.video.getVideoField(number);
    };

    // A field of no frame of the timeline, or that no source can give (a frame where none has one), is blank
    const FieldPosition position = (fieldNumber >= 1 && fieldNumber < positions.size()) ? positions[fieldNumber] : FieldPosition();
    Source &reference = *sources.front();

    QVector<SourceVideo::Data> inputs(static_cast<qint32>(sources.size()));
    QVector<LdDecodeMetaData::Field> fieldMetadata(static_cast<qint32>(sources.size()));
    QVector<qint32> usable;
    if (position.frame >= 1) {
        for (qint32 i = 0; i < static_cast<qint32>(sources.size()); i++) {
            Source &source = *sources[i];
            const qint32 frame = sourceFrame(source, position.frame, i == 0);
            if (frame < 0) continue;

            const qint32 first = source.metadata.getFirstFieldNumber(frame);
            const qint32 second = source.metadata.getSecondFieldNumber(frame);
            const qint32 available = source.video.getNumberOfAvailableFields();
            if (first < 1 || second < 1 || first > available || second > available) continue;
            if (source.metadata.getField(first).pad || source.metadata.getField(second).pad) continue;

            const qint32 number = position.isFirst ? first : second;
            SourceVideo::Data data = read(source, number);
            if (data.size() != fieldWidth * lines) continue;
            inputs[i] = data;
            fieldMetadata[i] = source.metadata.getField(number);
            usable.append(i);
        }
    }

    if (usable.isEmpty()) return SourceVideo::Data(fieldWidth * lines, 0);

    LdDecodeMetaData::VideoParameters parameters = reference.metadata.getVideoParameters();
    parameters.fieldHeight = lines;
    SourceVideo::Data output(fieldWidth * lines);
    DropOuts dropOuts;
    core.stackField(position.frame, inputs, parameters, fieldMetadata, usable, !project.stacking.diffDod,
                    project.stacking.passthrough, output, dropOuts, mode, project.stacking.smartThreshold * 256, false);
    return output;
}
