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
// The sources are lined up by the numbers of the disc that their VBI gives (the picture of a CAV disc, the time of a CLV one), and then by their own offsets
// in addition. The stack goes from the earliest frame of any source that is on to the latest of any of them: where no source has a frame, the frame is a padded one
// (blank). A source whose VBI gives no number begins at the number 0 of the disc.
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

    // Where the first frame of a capture is on the disc, by the numbers of its VBI (the picture of a CAV disc, the time of a CLV one in frames), or DiscNumbering::NoNumber if the VBI
    // gives none (the number can be 0 or negative: a capture that begins with frames of a lead-in that have no number is before the first number): the one most of the first frames agree on, so that a frame with a corrupt number does not decide it
    static qint32 discOrigin(LdDecodeMetaData &metadata);

    // Where each source begins on the timeline (in frames from the start of the timeline, the earliest being 0): by the origin on the disc (DiscNumbering::NoNumber: the number 0), plus its offset.
    // The frames of the timeline are returned in total, with the number of frames of each source.
    static QVector<qint32> alignedPositions(const QVector<qint32> &origins, const QVector<qint32> &offsets, const QVector<qint32> &frames, qint32 &total, qint32 *discAtStart = nullptr);

    // The metadata of the stack, as the tools read it: the parameters of the first source that is on, the fields of the timeline (those of the first source that has
    // each frame, a padded field where none has), with no dropouts (the stack has filled them) and no VITS measurements (those of a source are not the stack's),
    // and the phases of the fields in sequence as ld-disc-stacker leaves them. Returns false, with the reason in error.
    static bool readMetadata(const QString &stackFile, LdDecodeMetaData &metadata, QString &error);

private:
    struct Source {
        qint32 offset = 0;     // where its first frame is on the timeline
        LdDecodeMetaData metadata;
        SourceVideo video;
    };

    struct FieldPosition {
        qint32 frame = -1;     // the frame of the reference this field is of, -1 if none
        bool isFirst = true;
    };

    SourceVideo::Data stackField(qint32 fieldNumber, qint32 lines);

    StackProject project;
    std::vector<std::unique_ptr<Source>> sources;    // those that are on, the first of them gives the parameters
    QVector<FieldPosition> positions;            // for each field of the timeline, from 1
    qint32 timelineFrames = 0;
    qint32 fieldLength = 0;
    qint32 fieldWidth = 0;
    qint32 fieldHeight = 0;
    qint32 mode = 0;
    StackerCore core;
    QCache<qint32, SourceVideo::Data> cache;
};

#endif // STACKEDVIDEO_H
