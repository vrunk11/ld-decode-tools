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
#include <cmath>
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

    // The quality of a frame with 43 dB, no dropout, a number in order and full sync confidence: the parts add up to 100.3 of 1004 (and the dropouts to 100 000)
    CHECK(std::fabs(frameQualityPercent(43.0, 100.0, 100.0, 0, 1000000) - (100.0 + 100.0 + 100.0 + 100000.0) / 1004.0) < 1e-9);
    // Half of the amplitude (6.02 dB less) is half of the share of the black PSNR (it was 1.2 % with (dB/20)^10)
    {
        const double full = frameQualityPercent(43.0, 100.0, 100.0, 0, 1000000);
        const double less = frameQualityPercent(43.0 - 20.0 * std::log10(2.0), 100.0, 100.0, 0, 1000000);
        CHECK(std::fabs((full - less) - 50.0 / 1004.0) < 1e-9);
    }
    // 5 % of the samples in dropouts cost 5 % of the quality that dropouts decide (1000 of 1004 parts), whatever the size of the frame
    {
        const double full = frameQualityPercent(43.0, 100.0, 100.0, 0, 1000000);
        CHECK(std::fabs((full - frameQualityPercent(43.0, 100.0, 100.0, 50000, 1000000)) - 5.0 * 1000.0 / 1004.0) < 1e-9);
        CHECK(std::fabs((full - frameQualityPercent(43.0, 100.0, 100.0, 100, 2000)) - 5.0 * 1000.0 / 1004.0) < 1e-9);
    }
    // More dropouts than samples does not go under zero; a black PSNR over the reference does not go over
    CHECK(frameQualityPercent(43.0, 100.0, 100.0, 5000, 1000) == frameQualityPercent(43.0, 100.0, 100.0, 1000, 1000));
    CHECK(frameQualityPercent(60.0, 100.0, 100.0, 0, 1000) == frameQualityPercent(43.0, 100.0, 100.0, 0, 1000));

    std::printf("testframe: all checks passed\n");
    return 0;
}
