/******************************************************************************
 * testframe.cpp
 * ld-discmap - unit test of the order of the frames of the disc map
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "frame.h"

#define CHECK(condition) \
    do { if (!(condition)) { std::fprintf(stderr, "FAILED: %s (line %d)\n", #condition, __LINE__); std::exit(1); } } while (0)

static Frame frame(qint32 seq, qint32 vbi, bool pullDown)
{
    Frame value;
    value.seqFrameNumber(seq);
    value.vbiFrameNumber(vbi);
    value.isPullDown(pullDown);
    return value;
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    // The order of the numbers, and a pulldown frame after the numbered frame that has its number
    CHECK(frame(1, 5, false) < frame(2, 6, false));
    CHECK(!(frame(2, 6, false) < frame(1, 5, false)));
    CHECK(frame(1, 5, false) < frame(2, 5, true));
    CHECK(!(frame(2, 5, true) < frame(1, 5, false)));

    // It is a strict order: nothing is before itself, and two frames cannot each be before the other (two pulldown frames with the same number, or
    // frames that have no number, did that, and sorting them could crash)
    const Frame a = frame(1, -1, true), b = frame(2, -1, true);
    CHECK(!(a < a) && !(a < b) && !(b < a));
    const Frame c = frame(3, 0, false), d = frame(4, 0, false);
    CHECK(!(c < d) && !(d < c));

    // A map with many frames of the same number, pulldown or not, sorts (with the numbers in order, and the frames of one number in the order they were
    // in, as std::stable_sort keeps them)
    std::vector<Frame> frames;
    for (qint32 i = 0; i < 4000; i++) {
        const qint32 number = (i * 7919) % 53 - 3;          // (some are negative: no number)
        frames.push_back(frame(i, number, i % 3 == 0));
    }
    std::stable_sort(frames.begin(), frames.end());
    for (size_t i = 1; i < frames.size(); i++) {
        CHECK(frames[i - 1].vbiFrameNumber() <= frames[i].vbiFrameNumber());
        if (frames[i - 1].vbiFrameNumber() == frames[i].vbiFrameNumber()) {
            // (within a number a pulldown frame is never before a frame that is not one)
            CHECK(!(frames[i - 1].isPullDown() && !frames[i].isPullDown()));
        }
    }

    std::printf("testframe: all checks passed\n");
    return 0;
}
