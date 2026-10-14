/******************************************************************************
 * stackedvideo.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STACKEDVIDEO_H
#define STACKEDVIDEO_H

#include <QCache>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>

#include "lddecodemetadata.h"
#include "sourcevideo.h"
#include "stackercore.h"
#include "stackproject.h"

// The video of a stacking session (a .stack file) as if it were a TBC file: the fields are stacked from
// the sources when they are asked for. SourceVideo opens a .stack file with this, and LdDecodeMetaData reads
// the metadata that goes with it, so that the tools that read a TBC file (ld-analyse, ld-chroma-decoder...)
// can read a session, and show or decode the stack without it being written first.
//
// The stack is made as ld-disc-stacker does it for a session: the first source that is on is the reference,
// and the others are lined up with it by their offsets.
class StackedVideo
{
public:
    StackedVideo();
    ~StackedVideo();

    StackedVideo(const StackedVideo &) = delete;
    StackedVideo &operator=(const StackedVideo &) = delete;

    // Open the sources of the session
    bool open(const QString &stackFile, QString &error);

    qint32 getNumberOfFields();
    qint32 getFieldLength() const { return fieldLength; }

    // A field of the stack. startLine and endLine (numbered from 1) give the lines wanted, -1 for all of them.
    SourceVideo::Data getField(qint32 fieldNumber, qint32 startLine = -1, qint32 endLine = -1);

    // The metadata of the stack, as the tools read it: that of the reference source, but with no dropouts (the
    // stack has filled them) and no VITS measurements (those of the reference are not the stack's), and the
    // phases of the fields in sequence as ld-disc-stacker leaves them. Returns false, with the reason in error.
    static bool readMetadata(const QString &stackFile, LdDecodeMetaData &metadata, QString &error);

private:
    struct Source {
        qint32 offset = 0;
        LdDecodeMetaData metadata;
        SourceVideo video;
    };

    struct FieldPosition {
        qint32 frame = -1;     // the frame of the reference this field is of, -1 if none
        bool isFirst = true;
    };

    // The frame of a source that falls on a frame of the reference, or -1
    qint32 sourceFrame(Source &source, qint32 frame, bool isReference) const;
    SourceVideo::Data stackField(qint32 fieldNumber, qint32 lines);

    StackProject project;
    std::vector<std::unique_ptr<Source>> sources;    // those that are on, the reference first
    QVector<FieldPosition> positions;            // for each field of the reference, from 1
    qint32 fieldLength = 0;
    qint32 fieldWidth = 0;
    qint32 fieldHeight = 0;
    qint32 mode = 0;
    StackerCore core;
    QCache<qint32, SourceVideo::Data> cache;
};

#endif // STACKEDVIDEO_H
