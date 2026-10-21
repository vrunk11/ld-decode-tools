/******************************************************************************
 * discnumbering.cpp
 * ld-decode-tools - the frame numbers of a disc, read from the VBI of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "discnumbering.h"
#include "tbc/logging.h"

#include "lddecodemetadata.h"
#include "vbidecoder.h"

namespace DiscNumbering {

bool cavNumber(const VbiDecoder::Vbi &vbi, qint32 &number)
{
    if (vbi.picNo < 0) return false;
    number = vbi.picNo;
    return true;
}

bool clvNumber(LdDecodeMetaData &metadata, const VbiDecoder::Vbi &vbi, qint32 &number)
{
    if (vbi.clvHr == -1 || vbi.clvMin == -1 || vbi.clvSec == -1 || vbi.clvPicNo == -1) return false;
    LdDecodeMetaData::ClvTimecode timecode;
    timecode.hours = vbi.clvHr;
    timecode.minutes = vbi.clvMin;
    timecode.seconds = vbi.clvSec;
    timecode.pictureNumber = vbi.clvPicNo;
    number = metadata.convertClvTimecodeToFrameNumber(timecode);
    return true;
}

bool frameNumber(LdDecodeMetaData &metadata, const VbiDecoder::Vbi &vbi, qint32 &number)
{
    return cavNumber(vbi, number) || clvNumber(metadata, vbi, number);
}

QString cavText(qint32 picture)
{
    return QString::number(picture);
}

QString clvText(qint32 hours, qint32 minutes, qint32 seconds, qint32 picture)
{
    const auto two = [](qint32 value) { return value == -1 ? QStringLiteral("xx") : QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0')); };
    return (hours == -1 ? QStringLiteral("xx") : QString::number(hours)) + QLatin1Char(':') + two(minutes) + QLatin1Char(':') + two(seconds) + QLatin1Char('.') + two(picture);
}

QString clvText(qint32 frames, VideoSystem system)
{
    const qint32 framesPerSecond = timecodeFramesPerSecond(system);
    const qint32 seconds = frames / framesPerSecond;
    return clvText(seconds / 3600, (seconds / 60) % 60, seconds % 60, frames % framesPerSecond);
}

Range scanRange(LdDecodeMetaData &metadata)
{
    Range range;
    VbiDecoder vbiDecoder;
    qint32 cavMin = std::numeric_limits<qint32>::max();
    qint32 cavMax = std::numeric_limits<qint32>::min();
    qint32 clvMin = std::numeric_limits<qint32>::max();
    qint32 clvMax = std::numeric_limits<qint32>::min();

    // Using sequential frame numbering starting from 1
    for (qint32 seqFrame = 1; seqFrame <= metadata.getNumberOfFrames(); seqFrame++) {
        // Get the VBI data and then decode
        auto vbi1 = metadata.getFieldVbi(metadata.getFirstFieldNumber(seqFrame)).vbiData;
        auto vbi2 = metadata.getFieldVbi(metadata.getSecondFieldNumber(seqFrame)).vbiData;
        VbiDecoder::Vbi vbi = vbiDecoder.decodeFrame(vbi1[0], vbi1[1], vbi1[2], vbi2[0], vbi2[1], vbi2[2]);

        // Look for a complete, valid CAV picture number or CLV time-code
        qint32 number;
        if (cavNumber(vbi, number)) {
            range.cavCount++;

            if (number < cavMin) cavMin = number;
            if (number > cavMax) cavMax = number;
        }

        if (clvNumber(metadata, vbi, number)) {
            range.clvCount++;

            if (number < clvMin) clvMin = number;
            if (number > clvMax) clvMax = number;
        }
    }

    // If the metadata has no picture numbers or time-codes, there is nothing to say
    if (range.cavCount == 0 && range.clvCount == 0) return range;

    range.found = true;
    range.isCav = range.cavCount > range.clvCount;
    range.minimum = range.isCav ? cavMin : clvMin;
    range.maximum = range.isCav ? cavMax : clvMax;
    return range;
}

qint32 sequentialToDisc(qint32 sequentialFrameNumber, qint32 firstDiscFrame)
{
    return (firstDiscFrame - 1) + sequentialFrameNumber;
}

qint32 discToSequential(qint32 discFrameNumber, qint32 firstDiscFrame)
{
    // Offset the VBI frame number to get the sequential source frame number
    return discFrameNumber - firstDiscFrame + 1;
}


bool SourceRanges::scan(const QVector<LdDecodeMetaData *> &metadata)
{
    const qint32 numberOfSources = metadata.size();
    minimum.fill(0, numberOfSources);
    maximum.fill(0, numberOfSources);

    for (qint32 sourceNumber = 0; sourceNumber < numberOfSources; sourceNumber++) {
        const Range range = scanRange(*metadata[sourceNumber]);
        tbcDebugStream() << "DiscNumbering::SourceRanges::scan(): Got" << range.cavCount << "CAV picture codes and" << range.clvCount << "CLV timecodes";

        // If the metadata has no picture numbers or time-codes, we cannot use the source
        if (!range.found) {
            tbcDebugStream() << "DiscNumbering::SourceRanges::scan(): Source does not seem to contain valid CAV picture numbers or CLV time-codes - cannot process";
            return false;
        }

        if (range.isCav) {
            qInfo().nospace() << "Source #" << sourceNumber << " has a disc type of CAV (uses VBI frame numbers)";
        } else {
            qInfo().nospace() << "Source #" << sourceNumber << " has a disc type of CLV (uses VBI timecodes)";
        }
        minimum[sourceNumber] = range.minimum;
        maximum[sourceNumber] = range.maximum;

        qInfo().nospace() << "Source #" << sourceNumber << " has a VBI frame number range of " << minimum[sourceNumber] << " to " << maximum[sourceNumber];
    }

    return true;
}

}
