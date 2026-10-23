/******************************************************************************
 * testclosedcaptions.cpp
 * ld-export-metadata - unit test for the timestamps of the SCC output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QString>
#include <cstdio>
#include <cstdlib>

#include "closedcaptions.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

// The first field of the frame n (from 0) is the field 2n + 1
static qint32 fieldOfFrame(qint32 frame) { return 2 * frame + 1; }

#define STAMP(frame, system) generateTimeStamp(fieldOfFrame(frame), system).toStdString()

int main()
{
    // PAL: 25 frames a second, a colon before the frames
    CHECK(STAMP(0, PAL) == "00:00:00:00");
    CHECK(STAMP(24, PAL) == "00:00:00:24");
    CHECK(STAMP(25, PAL) == "00:00:01:00");
    CHECK(STAMP(1500, PAL) == "00:01:00:00");
    CHECK(STAMP(90000, PAL) == "01:00:00:00");

    // NTSC: drop-frame timecode (a semicolon before the frames); the numbers 0 and 1 of the frames are
    // skipped at the start of each minute that is not a tenth minute
    CHECK(STAMP(0, NTSC) == "00:00:00;00");
    CHECK(STAMP(29, NTSC) == "00:00:00;29");
    CHECK(STAMP(30, NTSC) == "00:00:01;00");
    CHECK(STAMP(1799, NTSC) == "00:00:59;29");
    CHECK(STAMP(1800, NTSC) == "00:01:00;02");
    CHECK(STAMP(1801, NTSC) == "00:01:00;03");
    CHECK(STAMP(17981, NTSC) == "00:09:59;29");
    CHECK(STAMP(17982, NTSC) == "00:10:00;00");
    CHECK(STAMP(17983, NTSC) == "00:10:00;01");
    CHECK(STAMP(19781, NTSC) == "00:10:59;29");
    CHECK(STAMP(19782, NTSC) == "00:11:00;02");
    // One hour of video at 30000/1001 frames a second is 107 892 frames: the clock reads one hour
    CHECK(STAMP(107892, NTSC) == "01:00:00;00");
    CHECK(STAMP(107892, PAL_M) == "01:00:00;00");

    std::printf("testclosedcaptions: ok\n");
    return 0;
}
