/******************************************************************************
 * testdiscnumbering.cpp
 * ld-decode-tools - unit tests for DiscNumbering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>

#include "discnumbering.h"
#include "lddecodemetadata.h"
#include "stackedvideo.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

// A capture of the given video system with a frame for each VBI given: a CAV picture number in line 17 of the
// first field (0xF followed by the five digits), or a CLV time code (hour and minute in line 17, second and
// picture in line 16), or nothing for a frame with no number. The words are those of IEC 60857-1986 10.1.3,
// 10.1.6 and 10.1.10, as testvbidecoder has them.
struct FrameVbi {
    quint32 line16 = 0;
    quint32 line17 = 0;
};

static FrameVbi cav(qint32 picture)
{
    FrameVbi vbi;
    vbi.line17 = 0xF00000u | (static_cast<quint32>(picture / 10000 % 10) << 16) | (static_cast<quint32>(picture / 1000 % 10) << 12)
                 | (static_cast<quint32>(picture / 100 % 10) << 8) | (static_cast<quint32>(picture / 10 % 10) << 4)
                 | static_cast<quint32>(picture % 10);
    return vbi;
}

static FrameVbi clv(qint32 hour, qint32 minute, qint32 second, qint32 picture)
{
    FrameVbi vbi;
    // 0xF1DD23: hour 1, minute 23 (line 17); 0x8EE223: second 42 (the first digit is 0xA more: E is 4), picture 23 (line 16)
    vbi.line17 = 0xF00000u | (static_cast<quint32>(hour) << 16) | 0xDD00u | (static_cast<quint32>(minute / 10) << 4) | static_cast<quint32>(minute % 10);
    vbi.line16 = 0x80E000u | (static_cast<quint32>(0xA + second / 10) << 16) | (static_cast<quint32>(second % 10) << 8)
                 | (static_cast<quint32>(picture / 10) << 4) | static_cast<quint32>(picture % 10);
    return vbi;
}

static void makeCapture(LdDecodeMetaData &metadata, VideoSystem system, const QVector<FrameVbi> &frames)
{
    metadata.initialiseDefault(system);
    metadata.ensureFields(static_cast<qint32>(frames.size()) * 2);
    for (qint32 i = 0; i < frames.size(); i++) {
        LdDecodeMetaData::Vbi vbi;
        vbi.inUse = true;
        vbi.vbiData = { static_cast<qint32>(frames[i].line16), static_cast<qint32>(frames[i].line17), 0 };
        metadata.updateFieldVbi(vbi, 2 * i + 1);
    }
}

static void testCav()
{
    std::cerr << "Testing DiscNumbering::scanRange with CAV picture numbers\n";

    // Numbers out of order, a frame with no number, and a picture number of 0, which is a number like another
    LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(120), cav(121), FrameVbi(), cav(0), cav(119), cav(5) });
    CHECK(metadata.getNumberOfFrames() == 6);
    const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
    CHECK(range.found);
    CHECK(range.isCav);
    CHECK(range.cavCount == 5);
    CHECK(range.clvCount == 0);
    CHECK(range.minimum == 0);
    CHECK(range.maximum == 121);
}

static void testClv()
{
    std::cerr << "Testing DiscNumbering::scanRange with CLV time codes\n";

    // NTSC counts 30 frames to the second: 00:01:02 picture 03 is (62 * 30) + 3
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, NTSC, { clv(0, 1, 2, 3), clv(0, 1, 2, 4), clv(0, 1, 2, 5) });
        const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
        CHECK(range.found);
        CHECK(!range.isCav);
        CHECK(range.clvCount == 3);
        CHECK(range.cavCount == 0);
        CHECK(range.minimum == 62 * 30 + 3);
        CHECK(range.maximum == 62 * 30 + 5);
    }

    // PAL counts 25, and the hour counts
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { clv(1, 23, 42, 20), clv(1, 23, 42, 21) });
        const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
        CHECK(range.found);
        CHECK(!range.isCav);
        CHECK(range.minimum == ((1 * 3600) + (23 * 60) + 42) * 25 + 20);
        CHECK(range.maximum == ((1 * 3600) + (23 * 60) + 42) * 25 + 21);
    }
}

static void testChoice()
{
    std::cerr << "Testing DiscNumbering::scanRange when a capture has both\n";

    // More picture numbers than time codes: CAV, and only its numbers are in the range
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(10), cav(11), clv(0, 0, 1, 0) });
        const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
        CHECK(range.isCav);
        CHECK(range.cavCount == 2 && range.clvCount == 1);
        CHECK(range.minimum == 10 && range.maximum == 11);
    }

    // The same number of each: CLV
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(10), clv(0, 0, 1, 0) });
        const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
        CHECK(range.found);
        CHECK(!range.isCav);
        CHECK(range.minimum == 25 && range.maximum == 25);
    }
}

static void testNothing()
{
    std::cerr << "Testing DiscNumbering::scanRange with no numbers\n";

    LdDecodeMetaData metadata;
    makeCapture(metadata, NTSC, { FrameVbi(), FrameVbi(), FrameVbi() });
    const DiscNumbering::Range range = DiscNumbering::scanRange(metadata);
    CHECK(!range.found);
    CHECK(range.cavCount == 0 && range.clvCount == 0);
}

static void testFrameNumber()
{
    std::cerr << "Testing DiscNumbering::frameNumber\n";

    LdDecodeMetaData ntsc;
    ntsc.initialiseDefault(NTSC);
    LdDecodeMetaData pal;
    pal.initialiseDefault(PAL);
    VbiDecoder::Vbi vbi;
    qint32 number = 0;

    // Nothing read: no number
    CHECK(!DiscNumbering::frameNumber(ntsc, vbi, number));

    // A picture number, 0 included
    vbi.picNo = 0;
    CHECK(DiscNumbering::frameNumber(ntsc, vbi, number) && number == 0);
    vbi.picNo = 54000;
    CHECK(DiscNumbering::frameNumber(ntsc, vbi, number) && number == 54000);

    // A time code needs its four parts; its number counts 25 or 30 frames to the second
    VbiDecoder::Vbi time;
    time.clvHr = 1;
    time.clvMin = 2;
    time.clvSec = 3;
    CHECK(!DiscNumbering::frameNumber(pal, time, number));
    time.clvPicNo = 4;
    CHECK(DiscNumbering::frameNumber(pal, time, number) && number == ((1 * 60 + 2) * 60 + 3) * 25 + 4);
    CHECK(DiscNumbering::frameNumber(ntsc, time, number) && number == ((1 * 60 + 2) * 60 + 3) * 30 + 4);
    time.clvMin = -1;
    CHECK(!DiscNumbering::frameNumber(ntsc, time, number));
    time.clvMin = 0;
    time.clvHr = 0;
    time.clvSec = 0;
    time.clvPicNo = 0;
    CHECK(DiscNumbering::frameNumber(ntsc, time, number) && number == 0);
}

static void testOrigin()
{
    std::cerr << "Testing StackedVideo::discOrigin\n";

    // The number of the disc of the first frame: the one that most of the first frames agree on
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(100), cav(101), cav(102), cav(103) });
        CHECK(StackedVideo::discOrigin(metadata) == 100);
    }
    // a frame with a corrupt number does not decide it
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(100), cav(101), cav(999), cav(103), cav(104) });
        CHECK(StackedVideo::discOrigin(metadata) == 100);
    }
    // a capture that begins with picture 0, and one that begins before it (3 frames of a lead-in with no number, then pictures from 1)
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { cav(0), cav(1), cav(2) });
        CHECK(StackedVideo::discOrigin(metadata) == 0);
    }
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, PAL, { FrameVbi(), FrameVbi(), FrameVbi(), cav(1), cav(2), cav(3) });
        CHECK(StackedVideo::discOrigin(metadata) == -2);
    }
    // a CLV capture: the time in frames
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, NTSC, { clv(0, 1, 2, 3), clv(0, 1, 2, 4) });
        CHECK(StackedVideo::discOrigin(metadata) == 62 * 30 + 3);
    }
    // no number at all
    {
        LdDecodeMetaData metadata;
        makeCapture(metadata, NTSC, { FrameVbi(), FrameVbi() });
        CHECK(StackedVideo::discOrigin(metadata) == DiscNumbering::NoNumber);
    }
}

static void testText()
{
    std::cerr << "Testing DiscNumbering::cavText and clvText\n";

    // A CAV picture is its number, no zeros before it
    CHECK(DiscNumbering::cavText(123) == "123");
    CHECK(DiscNumbering::cavText(0) == "0");
    CHECK(DiscNumbering::cavText(54000) == "54000");

    // A CLV time: the hours as they are, the other parts on two digits
    CHECK(DiscNumbering::clvText(1, 23, 45, 6) == "1:23:45.06");
    CHECK(DiscNumbering::clvText(0, 0, 0, 0) == "0:00:00.00");
    CHECK(DiscNumbering::clvText(12, 5, 9, 29) == "12:05:09.29");
    // a part that is not known is xx
    CHECK(DiscNumbering::clvText(-1, -1, -1, -1) == "xx:xx:xx.xx");
    CHECK(DiscNumbering::clvText(1, -1, 45, -1) == "1:xx:45.xx");
    CHECK(DiscNumbering::clvText(-1, 23, -1, 6) == "xx:23:xx.06");

    // From a number of frames: 25 or 30 to the second
    CHECK(DiscNumbering::clvText(((1 * 60 + 23) * 60 + 45) * 25 + 6, PAL) == "1:23:45.06");
    CHECK(DiscNumbering::clvText(((1 * 60 + 23) * 60 + 45) * 30 + 6, NTSC) == "1:23:45.06");
    CHECK(DiscNumbering::clvText(((1 * 60 + 23) * 60 + 45) * 30 + 6, PAL_M) == "1:23:45.06");
    CHECK(DiscNumbering::clvText(0, NTSC) == "0:00:00.00");
}

static void testConversions()
{
    std::cerr << "Testing DiscNumbering::sequentialToDisc and discToSequential\n";

    CHECK(DiscNumbering::sequentialToDisc(1, 755) == 755);
    CHECK(DiscNumbering::sequentialToDisc(24, 755) == 778);
    CHECK(DiscNumbering::discToSequential(755, 755) == 1);
    CHECK(DiscNumbering::discToSequential(778, 755) == 24);
    for (qint32 sequential = 1; sequential < 100; sequential++) {
        CHECK(DiscNumbering::discToSequential(DiscNumbering::sequentialToDisc(sequential, 34649), 34649) == sequential);
    }
}

static void testSourceRanges()
{
    std::cerr << "Testing DiscNumbering::SourceRanges\n";

    // Two captures of one disc: frames 10 to 14 and frames 12 to 20 (the second has its first frame with no number)
    LdDecodeMetaData first, second, none;
    makeCapture(first, PAL, { cav(10), cav(11), cav(12), cav(13), cav(14) });
    makeCapture(second, PAL, { cav(12), cav(13), cav(14), cav(15), cav(16), cav(17), cav(18), cav(19), cav(20) });
    makeCapture(none, PAL, { FrameVbi(), FrameVbi() });

    DiscNumbering::SourceRanges ranges;
    CHECK(ranges.scan({ &first, &second }));
    CHECK(ranges.contains(10, 0) && ranges.contains(14, 0));
    CHECK(!ranges.contains(9, 0) && !ranges.contains(15, 0));
    CHECK(ranges.contains(12, 1) && ranges.contains(20, 1));
    CHECK(!ranges.contains(11, 1) && !ranges.contains(21, 1));

    // The n-th frame of a capture, and back
    CHECK(ranges.toDisc(1, 0) == 10 && ranges.toDisc(5, 0) == 14);
    CHECK(ranges.toDisc(1, 1) == 12);
    CHECK(ranges.toSequential(14, 0) == 5);
    CHECK(ranges.toSequential(16, 1) == 5);

    // A capture with no number cannot be used
    DiscNumbering::SourceRanges unusable;
    CHECK(!unusable.scan({ &first, &none }));
}

int main()
{
    testCav();
    testClv();
    testChoice();
    testNothing();
    testFrameNumber();
    testOrigin();
    testText();
    testConversions();
    testSourceRanges();
    std::cerr << "All tests passed\n";
    return 0;
}
