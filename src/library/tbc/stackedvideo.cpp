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
#include "stackframe.h"
#include "discnumbering.h"

#include <QFileInfo>
#include <QHash>
#include <cmath>
#include <limits>

#include "ldxcapture.h"
#include "tbcfiles.h"
#include "vbidecoder.h"
#include "mappedmetadata.h"

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

// The metadata of a source of the session, with the field order the session says
static bool readSourceMetadata(const QString &name, bool reverseFields, LdDecodeMetaData &metadata, QString &error)
{
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(name);
    if (!metadataThere(metadataFile) || !metadata.read(metadataFile)) {
        error = QObject::tr("Could not read the metadata of %1").arg(name);
        return false;
    }
    metadata.setIsFirstFieldFirst(!reverseFields);
    return true;
}

// The metadata of a source of the session, with the field order the session says. A source that has a map is the capture seen through it: its
// metadata are those of the mapped copy (see MappedMetadata), and the metadata of the capture itself are given in captureMetadata, for opening the video.
static bool readSourceMetadata(const StackSource &entry, bool reverseFields, LdDecodeMetaData &metadata, QString &error, LdDecodeMetaData *captureMetadata = nullptr,
                               std::shared_ptr<const QVector<qint32>> *embeddedTable = nullptr)
{
    const QString name = entry.resolvedReference();
    const FrameMap *map = entry.activeMap();
    if (!map) {
        if (!readSourceMetadata(name, reverseFields, metadata, error)) return false;
        // A source with no map in the session whose own metadata keep one is the capture seen through it (the Follow map box off reads the capture as it is)
        if (entry.followMap && metadata.hasDiscMap()) {
            const auto table = MappedMetadata::applyEmbeddedMap(metadata, LdDecodeMetaData::findMetadataFile(name), StackProject::referenceFile(name), error);
            if (!table) {
                if (error.isEmpty()) error = QObject::tr("The map in the metadata of %1 cannot be used").arg(name);
                return false;
            }
            metadata.setIsFirstFieldFirst(!reverseFields);
            if (embeddedTable) *embeddedTable = table;
        }
        return true;
    }

    LdDecodeMetaData local;
    LdDecodeMetaData &capture = captureMetadata ? *captureMetadata : local;
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(name);
    if (!metadataThere(metadataFile) || !capture.read(metadataFile)) {
        error = QObject::tr("Could not read the metadata of %1").arg(name);
        return false;
    }

    // The map is good for the capture it was made for
    const QString videoFile = StackProject::referenceFile(name);
    if (!LdxCapture::isLdxName(videoFile) && map->captureSize > 0 && QFileInfo::exists(videoFile) && QFileInfo(videoFile).size() != map->captureSize) {
        error = QObject::tr("The map of %1 was made for another capture (it is not of the same size)").arg(name);
        return false;
    }

    QString buildError;
    if (!MappedMetadata::build(capture, *map, MappedMetadata::paddedAudioSamples(capture, metadataFile), metadata, buildError)) {
        error = QObject::tr("The map of %1 cannot be used: %2").arg(name, buildError);
        return false;
    }
    metadata.setIsFirstFieldFirst(!reverseFields);
    return true;
}

qint32 StackedVideo::discOrigin(LdDecodeMetaData &metadata)
{
    VbiDecoder vbiDecoder;
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
        qint32 number;
        if (!DiscNumbering::frameNumber(metadata, vbi, number)) continue;
        votes[number - (frame - 1)]++;
        cast++;
    }
    qint32 best = 0, origin = DiscNumbering::NoNumber;
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
    for (qint32 origin: origins) numbered = numbered || origin != DiscNumbering::NoNumber;
    QVector<qint32> positions(count, 0);
    qint32 lowest = std::numeric_limits<qint32>::max();
    for (qint32 i = 0; i < count; i++) {
        positions[i] = (origins[i] == DiscNumbering::NoNumber ? 0 : origins[i]) + offsets[i];
        lowest = qMin(lowest, positions[i]);
    }
    if (count == 0) lowest = 0;
    const qint32 startOfTimeline = lowest;
    total = 0;
    for (qint32 i = 0; i < count; i++) {
        positions[i] -= lowest;
        total = qMax(total, positions[i] + frames[i]);
    }
    if (discAtStart) *discAtStart = numbered ? startOfTimeline : DiscNumbering::NoNumber;
    return positions;
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

    if (!readSourceMetadata(*reference, session.stacking.reverseFields, metadata, error)) return false;

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
        if (!readSourceMetadata(entry, session.stacking.reverseFields, *other, error)) return false;
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
    // (what the session says was measured of the stack, if it is good for these captures, offsets and options: the SNR is in the metadata of the stack)
    const StackMeasure *measured = session.measureFor(session.stackMeasureKey(true));
    if (!measured) measured = session.measureFor(session.stackMeasureKey(false));
    const bool haveSnr = measured && measured->white.size() == total && measured->black.size() == total;
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
        // (a session with one source on is that source: its dropouts and its VITS measurements are the ones the video has)
        if (frames.size() > 1) {
            for (LdDecodeMetaData::Field &field: pair) {
                field.dropOuts = DropOuts();
                field.vitsMetrics = LdDecodeMetaData::VitsMetrics();
            }
        }
        if (frames.size() > 1 && haveSnr && (std::isfinite(measured->white[frame - 1]) || std::isfinite(measured->black[frame - 1]))) {
            for (LdDecodeMetaData::Field &field: pair) {
                field.vitsMetrics.inUse = true;
                field.vitsMetrics.wSNR = std::isfinite(measured->white[frame - 1]) ? measured->white[frame - 1] : 0.0;
                field.vitsMetrics.bPSNR = std::isfinite(measured->black[frame - 1]) ? measured->black[frame - 1] : 0.0;
            }
        }
        timeline.append(firstFirst ? pair[0] : pair[1]);
        timeline.append(firstFirst ? pair[1] : pair[0]);
    }
    metadata.clearFields();
    for (const LdDecodeMetaData::Field &field: timeline) metadata.appendField(field);
    const qint32 fields = metadata.getNumberOfFields();

    // The fields come in a sequence of phases: the padded fields of the reference may have broken it, and
    // ld-disc-stacker puts it right in what it writes
    StackFrame::repairFieldPhases(metadata);
    return true;
}

bool StackedVideo::open(const QString &stackFile, QString &error)
{
    if (!project.load(stackFile, error)) return false;

    sources.clear();
    bool firstIsCvbs = false;
    for (const StackSource &entry: project.sources) {
        if (!entry.enabled) continue;
        const QString name = entry.resolvedReference();

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

        LdDecodeMetaData captureMetadata;
        std::shared_ptr<const QVector<qint32>> embeddedTable;
        if (!readSourceMetadata(entry, project.stacking.reverseFields, source->metadata, error, &captureMetadata, &embeddedTable)) return false;

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
        const FrameMap *map = entry.activeMap();
        if (!(map ? source->video.openMapped(name, fieldLength, fieldWidth, *map, captureMetadata)
                  : (embeddedTable ? source->video.openMappedFields(name, fieldLength, fieldWidth, embeddedTable) : source->video.open(name, fieldLength, fieldWidth)))) {
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
    // A field of no frame of the timeline, or that no source can give (a frame where none has one), is blank
    const FieldPosition position = (fieldNumber >= 1 && fieldNumber < positions.size()) ? positions[fieldNumber] : FieldPosition();

    QVector<StackFrame::Input> inputs(static_cast<qint32>(sources.size()));
    for (qint32 i = 0; i < static_cast<qint32>(sources.size()); i++) {
        Source &source = *sources[i];
        inputs[i].metadata = &source.metadata;
        inputs[i].video = &source.video;
        if (position.frame >= 1) inputs[i].frame = StackFrame::ownFrame(position.frame, source.offset, source.metadata.getNumberOfFrames());
    }

    const qint32 which = position.isFirst ? StackFrame::FirstField : StackFrame::SecondField;
    StackFrame::Fields fields;
    if (!StackFrame::read(inputs, which, lines < fieldHeight ? lines : -1, fields)) return SourceVideo::Data(fieldWidth * lines, 0);

    StackFrame::Result result;
    StackFrame::stack(core, fields, which, position.frame, mode, project.stacking.smartThreshold, project.stacking.diffDod,
                      project.stacking.passthrough, result);
    return position.isFirst ? result.first : result.second;
}
