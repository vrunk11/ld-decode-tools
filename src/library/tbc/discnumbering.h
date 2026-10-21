/******************************************************************************
 * discnumbering.h
 * ld-decode-tools - the frame numbers of a disc, read from the VBI of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef DISCNUMBERING_H
#define DISCNUMBERING_H

#include <QString>
#include <QtGlobal>
#include <QVector>
#include <limits>

#include "lddecodemetadata.h"
#include "vbidecoder.h"


// What a capture says about the numbers of the frames of its disc, in one place for the tools that
// line captures up by them (ld-disc-stacker, ld-dropout-correct)
namespace DiscNumbering {

// "No number": a frame number of a disc can be 0 or negative (the frames of a lead-in come before the first picture), so -1 cannot stand for it
constexpr qint32 NoNumber = std::numeric_limits<qint32>::min();

// The number of the disc that the VBI of a frame gives: the picture number of a CAV disc (0 is a number; -1 is the VBI decoder saying there is none), or else a CLV time code
// that has its hour, minute, second and picture, in frames. False if the VBI gives none.
bool cavNumber(const VbiDecoder::Vbi &vbi, qint32 &number);
bool clvNumber(LdDecodeMetaData &metadata, const VbiDecoder::Vbi &vbi, qint32 &number);
bool frameNumber(LdDecodeMetaData &metadata, const VbiDecoder::Vbi &vbi, qint32 &number);

// The numbers a capture holds
struct Range {
    bool found = false;     // false if no frame has a complete CAV picture number or CLV time code
    bool isCav = false;     // CAV if more frames have a picture number than a time code, otherwise CLV
    qint32 minimum = 0;     // the lowest and the highest frame number (a CLV time code is taken as a frame number)
    qint32 maximum = 0;
    qint32 cavCount = 0;    // the frames that have a picture number (0 included)
    qint32 clvCount = 0;    // the frames that have a complete CLV time code (hour, minute, second and picture)
};

// The numbers of a disc as they are written, in every window and file that shows them. A CAV picture is its number, with no zeros before it (123). A CLV time is
// hours:minutes:seconds.picture, the hours as they are, the other parts on two digits (1:23:45.06); a part that is not known (-1) is xx. The second form takes the time as
// a number of frames, as a disc number is (see frameNumber), for the system that counts them
QString cavText(qint32 picture);
QString clvText(qint32 hours, qint32 minutes, qint32 seconds, qint32 picture);
QString clvText(qint32 frames, VideoSystem system);

// Decode the VBI of every frame of the capture and give the range of its numbers. Only the numbers of the
// disc type that most frames have are in the range
Range scanRange(LdDecodeMetaData &metadata);

// The number of a frame of the disc from its place in the capture (1 is the first frame), and back, for a
// capture whose lowest frame number is firstDiscFrame
qint32 sequentialToDisc(qint32 sequentialFrameNumber, qint32 firstDiscFrame);
qint32 discToSequential(qint32 discFrameNumber, qint32 firstDiscFrame);

// The numbers of the frames that each of several captures holds, for the tools that line them up by disc number
class SourceRanges {
public:
    // Scan every capture (see scanRange), and say for each its disc type and range on the standard error. False if one has no numbers.
    bool scan(const QVector<LdDecodeMetaData *> &metadata);

    // Whether capture source holds a frame with this number of the disc
    bool contains(qint32 discFrameNumber, qint32 source) const {
        return discFrameNumber >= minimum[source] && discFrameNumber <= maximum[source];
    }
    // The number of the disc of the n-th frame of a capture (1 is the first), and back
    qint32 toDisc(qint32 sequentialFrameNumber, qint32 source) const { return sequentialToDisc(sequentialFrameNumber, minimum[source]); }
    qint32 toSequential(qint32 discFrameNumber, qint32 source) const { return discToSequential(discFrameNumber, minimum[source]); }

private:
    QVector<qint32> minimum;
    QVector<qint32> maximum;
};

}

#endif // DISCNUMBERING_H
