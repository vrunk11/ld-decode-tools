/******************************************************************************
 * testframemap.cpp
 * ld-decode-tools TBC library - unit test of the map of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdio>
#include <cstdlib>

#include "framemap.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static MapFrame frame(qint32 disc, qint32 capture, MapBasis basis, bool unusable = false)
{
    MapFrame value;
    value.discFrame = disc;
    value.captureFrame = capture;
    value.basis = basis;
    value.unusable = unusable;
    return value;
}

int main()
{
    // A capture without a skip is one run
    {
        QVector<MapFrame> frames;
        for (qint32 i = 0; i < 1000; i++) frames.append(frame(1000 + i, 1 + i, MapBasis::Measured));
        const FrameMap map = FrameMap::fromFrames(frames);
        CHECK(map.runs().size() == 1);
        CHECK(map.frameCount() == 1000);
        CHECK(map.frameAt(1).discFrame == 1000 && map.frameAt(1).captureFrame == 1);
        CHECK(map.frameAt(1000).discFrame == 1999 && map.frameAt(1000).captureFrame == 1000);
        CHECK(map.frames() == frames);
        CHECK(map.measuredShare() == 1.0);
    }

    // A gap padded, a frame deduced, an unusable frame: runs, the places, and the basis of each
    {
        QVector<MapFrame> frames;
        for (qint32 i = 0; i < 5; i++) frames.append(frame(10 + i, 1 + i, MapBasis::Measured));      // 10..14 from the capture 1..5
        frames.append(frame(15, 0, MapBasis::Padded));                                               // two frames of the gap are padded
        frames.append(frame(16, 0, MapBasis::Padded));
        frames.append(frame(17, 6, MapBasis::Sequence));                                             // the number was deduced from the neighbours
        frames.append(frame(18, 7, MapBasis::Measured));
        frames.append(frame(19, 8, MapBasis::Guessed, true));                                        // could not be numbered
        frames.append(frame(20, 9, MapBasis::Pulldown));
        const FrameMap map = FrameMap::fromFrames(frames);
        CHECK(map.runs().size() == 6);
        CHECK(map.frameCount() == 11);
        CHECK(map.unusable().size() == 1 && map.unusable()[0] == 10);
        CHECK(map.frameAt(6).captureFrame == 0 && map.frameAt(6).basis == MapBasis::Padded && map.frameAt(6).discFrame == 15);
        CHECK(map.frameAt(7).discFrame == 16 && map.frameAt(7).captureFrame == 0);
        CHECK(map.frameAt(10).unusable && !map.frameAt(9).unusable);
        CHECK(map.frames() == frames);
        const QVector<qint32> counts = map.basisCounts();
        CHECK(counts[0] == 6 && counts[2] == 1 && counts[3] == 1 && counts[4] == 1 && counts[5] == 2 && counts[1] == 0);
        CHECK(map.frameAt(0).basis == MapBasis::Padded && map.frameAt(12).captureFrame == 0);          // (outside the map)

        // The object in a file, and back
        FrameMap read;
        QString error;
        FrameMap made = map;
        made.captureFields = 18;
        made.captureSize = 123456789012LL;
        made.madeBy = "test";
        CHECK(FrameMap::fromJsonBytes(made.toJsonBytes(), read, error));
        CHECK(read == made);
        CHECK(read.frames() == frames);
        CHECK(read.isFor(18, 123456789012LL) && !read.isFor(18, 5) && !read.isFor(20, 123456789012LL));
        CHECK(read.madeBy == "test");
    }

    // Frames that look alike but do not follow each other are separate runs: a number that goes back, a capture frame that skips, another basis
    {
        QVector<MapFrame> frames = { frame(5, 1, MapBasis::Measured), frame(6, 2, MapBasis::Measured), frame(7, 4, MapBasis::Measured),
                                     frame(8, 5, MapBasis::Unconfirmed), frame(9, 6, MapBasis::Unconfirmed), frame(3, 7, MapBasis::Measured) };
        const FrameMap map = FrameMap::fromFrames(frames);
        CHECK(map.runs().size() == 4);
        CHECK(map.frames() == frames);
    }

    // An object that is not good is refused with a reason
    {
        FrameMap map;
        QString error;
        CHECK(!FrameMap::fromJsonBytes("not json", map, error));
        CHECK(!FrameMap::fromJsonBytes("{\"runs\":[]}", map, error));
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":2,\"runs\":[]}", map, error));
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,1,0,\"m\"]]}", map, error));                 // a count of 0
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,1,5,\"x\"]]}", map, error));                 // a basis that does not exist
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,0,5,\"m\"]]}", map, error));                 // padded without the basis
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,3,5,\"p\"]]}", map, error));                 // the basis without padding
        CHECK(!FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,1,5,\"m\"]],\"unusable\":[9]}", map, error)); // a place that is not there
        CHECK(FrameMap::fromJsonBytes("{\"ldxMap\":1,\"runs\":[[1,1,5,\"m\"]],\"unusable\":[2,4]}", map, error));
        CHECK(map.frameAt(2).unusable && map.frameAt(4).unusable && !map.frameAt(3).unusable);
    }

    std::printf("testframemap: ok\n");
    return 0;
}
