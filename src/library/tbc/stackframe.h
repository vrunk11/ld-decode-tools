/******************************************************************************
 * stackframe.h
 * ld-decode-tools - stacking a frame of the timeline from the sources that have it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STACKFRAME_H
#define STACKFRAME_H

#include <QVector>

#include "lddecodemetadata.h"
#include "sourcevideo.h"
#include "stackercore.h"

// What a stack of several captures needs besides StackerCore, in one place for the programs that stack a frame of a timeline (the stack of a session
// read by ld-analyse and ld-chroma-decoder, the preview of ld-stacker-gui): which of the sources can give a frame, and the fields they give
namespace StackFrame {

// The frame of a source (numbered from 1 in its own file, which has numberOfFrames) that falls on a frame of the timeline when the source begins offset frames
// later on it; -1 if it has none
qint32 ownFrame(qint32 timelineFrame, qint32 offset, qint32 numberOfFrames);

// A frame that has a field that is missing (padded, or not in the metadata) cannot be stacked
bool isPadded(LdDecodeMetaData &metadata, qint32 ownFrame);

// A source, for one frame of the timeline: its metadata, the video to read the fields with (a thread can have a handle of its own on the file), and the frame
// of it that falls here (-1 if none)
struct Input {
    LdDecodeMetaData *metadata = nullptr;
    SourceVideo *video = nullptr;
    qint32 frame = -1;
};

// Which of the fields of a frame
constexpr qint32 FirstField = 1;
constexpr qint32 SecondField = 2;
constexpr qint32 BothFields = FirstField | SecondField;

// The fields of the sources that can be stacked. first / second and their metadata are indexed as inputs are: empty for a source that is not usable
struct Fields {
    QVector<SourceVideo::Data> first;
    QVector<SourceVideo::Data> second;
    QVector<LdDecodeMetaData::Field> firstMetadata;
    QVector<LdDecodeMetaData::Field> secondMetadata;
    QVector<qint32> usable;                                 // the inputs that can be stacked, in order
    LdDecodeMetaData::VideoParameters videoParameters;      // those of the first input (of the first usable one if it is not), with the height of what was read
};

// Read the fields of the inputs that can be stacked: those that have a frame here, whose fields are in the metadata and in the file, are not padded, are the size
// of the first one, and whose lines read are all there. Only the first lines of the fields if lines is above 0 and under their height. Only the field asked for
// of a frame (which). False if no input can be used.
bool read(const QVector<Input> &inputs, qint32 which, qint32 lines, Fields &fields);

// The phases of the fields of the metadata of a stack, put in sequence again. The fields come in a sequence of phases (4 for NTSC and PAL-M, 8 for PAL: 1, 2, 3... and round again,
// a field at a time), and the padded fields of the reference may have broken it. The first field that is not padded keeps its phase and the others follow from it.
void repairFieldPhases(LdDecodeMetaData &metadata);

// The stack of the fields that were read, for the field or the fields asked for (an output that is not asked for is left as it is). mode is as for StackerCore (what -1 is
// is for the caller to say: StackerCore::autoMode); smartThreshold is in levels of 8 bits, 0 to 128.
struct Result {
    SourceVideo::Data first;
    SourceVideo::Data second;
    DropOuts firstDropOuts;
    DropOuts secondDropOuts;
};
void stack(StackerCore &core, const Fields &fields, qint32 which, qint32 frameNumber, qint32 mode, qint32 smartThreshold, bool diffDod, bool passthrough, Result &result);

}

#endif // STACKFRAME_H
