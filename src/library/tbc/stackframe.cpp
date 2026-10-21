/******************************************************************************
 * stackframe.cpp
 * ld-decode-tools - stacking a frame of the timeline from the sources that have it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "stackframe.h"

namespace StackFrame {

qint32 ownFrame(qint32 timelineFrame, qint32 offset, qint32 numberOfFrames)
{
    // A source shifted by +2 starts two frames later on the timeline of the stack
    const qint32 own = timelineFrame - offset;
    if (own < 1 || own > numberOfFrames) return -1;
    return own;
}

bool isPadded(LdDecodeMetaData &metadata, qint32 ownFrame)
{
    const qint32 first = metadata.getFirstFieldNumber(ownFrame);
    const qint32 second = metadata.getSecondFieldNumber(ownFrame);
    if (first < 1 || second < 1) return true;
    return metadata.getField(first).pad || metadata.getField(second).pad;
}

bool read(const QVector<Input> &inputs, qint32 which, qint32 lines, Fields &fields)
{
    const qint32 count = inputs.size();
    fields = Fields();
    fields.first = QVector<SourceVideo::Data>(count);
    fields.second = QVector<SourceVideo::Data>(count);
    fields.firstMetadata = QVector<LdDecodeMetaData::Field>(count);
    fields.secondMetadata = QVector<LdDecodeMetaData::Field>(count);

    // The sources that have a frame here that is not padded
    QVector<qint32> candidates;
    for (qint32 i = 0; i < count; i++) {
        const Input &input = inputs[i];
        if (!input.metadata || !input.video || input.frame < 1) continue;
        if (isPadded(*input.metadata, input.frame)) continue;
        candidates.append(i);
    }
    if (candidates.isEmpty()) return false;

    // The sources must be alike: those that are not the size of the first are left out
    LdDecodeMetaData::VideoParameters videoParameters = inputs[candidates[0]].metadata->getVideoParameters();
    const qint32 fullHeight = videoParameters.fieldHeight;

    // Only the first lines, when that is all that is wanted (the stacking sees the fields as that short)
    const qint32 readLines = (lines > 0 && lines < fullHeight) ? lines : -1;
    if (readLines > 0) videoParameters.fieldHeight = readLines;
    const qint32 expectedSize = videoParameters.fieldWidth * videoParameters.fieldHeight;
    const auto readField = [readLines](SourceVideo &video, qint32 field) {
        return readLines > 0 ? video.getVideoField(field, 1, readLines) : video.getVideoField(field);
    };

    for (qint32 i: candidates) {
        const Input &input = inputs[i];
        const LdDecodeMetaData::VideoParameters &other = input.metadata->getVideoParameters();
        if (other.fieldWidth != videoParameters.fieldWidth || other.fieldHeight != fullHeight) continue;

        const qint32 first = input.metadata->getFirstFieldNumber(input.frame);
        const qint32 second = input.metadata->getSecondFieldNumber(input.frame);
        const qint32 available = input.video->getNumberOfAvailableFields();
        // The metadata can name fields that the file does not hold
        if (first < 1 || second < 1 || first > available || second > available) continue;

        // (in the order of the file, to save seeking)
        if ((which & FirstField) && (which & SecondField)) {
            if (first < second) {
                fields.first[i] = readField(*input.video, first);
                fields.second[i] = readField(*input.video, second);
            } else {
                fields.second[i] = readField(*input.video, second);
                fields.first[i] = readField(*input.video, first);
            }
            if (fields.first[i].size() != expectedSize || fields.second[i].size() != expectedSize) continue;
            fields.firstMetadata[i] = input.metadata->getField(first);
            fields.secondMetadata[i] = input.metadata->getField(second);
        } else if (which & FirstField) {
            fields.first[i] = readField(*input.video, first);
            if (fields.first[i].size() != expectedSize) continue;
            fields.firstMetadata[i] = input.metadata->getField(first);
        } else {
            fields.second[i] = readField(*input.video, second);
            if (fields.second[i].size() != expectedSize) continue;
            fields.secondMetadata[i] = input.metadata->getField(second);
        }
        fields.usable.append(i);
    }

    // The parameters are those of the first source, as ld-disc-stacker uses, or else of the first that is used
    fields.videoParameters = videoParameters;
    if (!inputs.isEmpty() && inputs[0].metadata && inputs[0].metadata != inputs[candidates[0]].metadata) {
        fields.videoParameters = inputs[0].metadata->getVideoParameters();
        if (readLines > 0) fields.videoParameters.fieldHeight = readLines;
    }
    return !fields.usable.isEmpty();
}

void repairFieldPhases(LdDecodeMetaData &metadata)
{
    const qint32 phaseCount = metadata.getVideoParameters().system == PAL ? 8 : 4;
    const qint32 fieldCount = metadata.getNumberOfFields();

    // Find the first non-padded field
    qint32 pivotField = 1;
    while (pivotField <= fieldCount && metadata.getField(pivotField).pad) {
        ++pivotField;
    }
    if (pivotField >= fieldCount) return;

    // Get the starting phase ID - 1
    qint32 currentPhaseID = metadata.getField(pivotField).fieldPhaseID - 1;
    currentPhaseID -= (pivotField - 1) % phaseCount;
    currentPhaseID += phaseCount;
    currentPhaseID %= phaseCount;

    // Overwrite phase IDs
    for (qint32 fieldNumber = 1; fieldNumber <= fieldCount; ++fieldNumber) {
        LdDecodeMetaData::Field field = metadata.getField(fieldNumber);
        field.fieldPhaseID = currentPhaseID + 1;
        metadata.updateField(field, fieldNumber);
        ++currentPhaseID;
        currentPhaseID %= phaseCount;
    }
}

void stack(StackerCore &core, const Fields &fields, qint32 which, qint32 frameNumber, qint32 mode, qint32 smartThreshold, bool diffDod, bool passthrough, Result &result)
{
    if (fields.usable.isEmpty()) return;
    const qint32 reference = fields.usable[0];
    if (which & FirstField) {
        result.first = SourceVideo::Data(fields.first[reference].size());
        core.stackField(frameNumber, fields.first, fields.videoParameters, fields.firstMetadata, fields.usable, !diffDod, passthrough,
                        result.first, result.firstDropOuts, mode, smartThreshold * 256, false);
    }
    if (which & SecondField) {
        result.second = SourceVideo::Data(fields.second[reference].size());
        core.stackField(frameNumber, fields.second, fields.videoParameters, fields.secondMetadata, fields.usable, !diffDod, passthrough,
                        result.second, result.secondDropOuts, mode, smartThreshold * 256, false);
    }
}

}
