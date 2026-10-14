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

#include "ldxcapture.h"

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

bool StackedVideo::readMetadata(const QString &stackFile, LdDecodeMetaData &metadata, QString &error)
{
    StackProject session;
    if (!session.load(stackFile, error)) return false;

    const StackSource *reference = referenceSource(session);
    if (!reference) {
        error = QObject::tr("No source is on in %1").arg(stackFile);
        return false;
    }

    const QString metadataFile = LdDecodeMetaData::findMetadataFile(reference->reference());
    if (!metadataThere(metadataFile) || !metadata.read(metadataFile)) {
        error = QObject::tr("Could not read the metadata of %1").arg(reference->reference());
        return false;
    }
    metadata.setIsFirstFieldFirst(!session.stacking.reverseFields);

    // What the stack has no use for: the sources' dropouts, which it fills, and the VITS measurements, which
    // are those of the reference and not of the stack
    const qint32 fields = metadata.getNumberOfFields();
    for (qint32 fieldNumber = 1; fieldNumber <= fields; fieldNumber++) {
        LdDecodeMetaData::Field field = metadata.getField(fieldNumber);
        field.dropOuts = DropOuts();
        field.vitsMetrics = LdDecodeMetaData::VitsMetrics();
        metadata.updateField(field, fieldNumber);
    }

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

        // TBC and CVBS are not mixed
        const bool isCvbs = Cvbs::isDataFileName(StackProject::referenceFile(entry.reference()));
        if (sources.empty()) {
            firstIsCvbs = isCvbs;
        } else if (isCvbs != firstIsCvbs) {
            error = QObject::tr("%1 is not of the same format (TBC or CVBS) as the first source: they cannot be stacked together").arg(entry.reference());
            return false;
        }

        auto source = std::make_unique<Source>();
        source->offset = sources.empty() ? 0 : entry.offset;    // the reference has none

        const QString metadataFile = LdDecodeMetaData::findMetadataFile(entry.reference());
        if (!metadataThere(metadataFile) || !source->metadata.read(metadataFile)) {
            error = QObject::tr("Could not read the metadata of %1").arg(entry.reference());
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
                error = QObject::tr("%1 is not of the same video system and size as the first source").arg(entry.reference());
                return false;
            }
        }

        // (with the line length, so that only the first lines of a field can be asked for)
        if (!source->video.open(entry.reference(), fieldLength, fieldWidth)) {
            error = QObject::tr("Could not open %1").arg(entry.reference());
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

    // Which frame of the reference each of its fields is of
    LdDecodeMetaData &metadata = sources.front()->metadata;
    positions = QVector<FieldPosition>(metadata.getNumberOfFields() + 1);
    const qint32 frames = metadata.getNumberOfFrames();
    for (qint32 frame = 1; frame <= frames; frame++) {
        const qint32 first = metadata.getFirstFieldNumber(frame);
        const qint32 second = metadata.getSecondFieldNumber(frame);
        if (first >= 1 && first < positions.size()) positions[first] = { frame, true };
        if (second >= 1 && second < positions.size()) positions[second] = { frame, false };
    }
    return true;
}

qint32 StackedVideo::getNumberOfFields()
{
    return sources.empty() ? 0 : sources.front()->metadata.getNumberOfFields();
}

qint32 StackedVideo::sourceFrame(Source &source, qint32 frame, bool isReference) const
{
    const qint32 own = isReference ? frame : frame - source.offset;
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

    // A field that is of no frame, or that no source can give, is the reference's own
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

    if (usable.isEmpty()) {
        const qint32 available = reference.video.getNumberOfAvailableFields();
        if (fieldNumber < 1 || fieldNumber > available) return SourceVideo::Data(fieldWidth * lines, 0);
        return read(reference, fieldNumber);
    }

    LdDecodeMetaData::VideoParameters parameters = reference.metadata.getVideoParameters();
    parameters.fieldHeight = lines;
    SourceVideo::Data output(fieldWidth * lines);
    DropOuts dropOuts;
    core.stackField(position.frame, inputs, parameters, fieldMetadata, usable, !project.stacking.diffDod,
                    project.stacking.passthrough, output, dropOuts, mode, project.stacking.smartThreshold * 256, false);
    return output;
}
