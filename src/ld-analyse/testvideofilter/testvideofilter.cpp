/******************************************************************************
 * testvideofilter.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdio>
#include <cstdlib>

#include "videofilter.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

// A frame of two fields: the first white, the second black
static QImage interlacedFrame(qint32 width, qint32 height)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (qint32 y = 0; y < height; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (qint32 x = 0; x < width; x++) line[x] = (y % 2 == 0) ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
    }
    return image;
}

static VideoFilter::Format makeFormat(const QImage &image, qint32 displayWidth, qint32 displayHeight)
{
    VideoFilter::Format format;
    format.width = image.width();
    format.height = image.height();
    format.displayWidth = displayWidth;
    format.displayHeight = displayHeight;
    return format;
}

int main()
{
    if (!VideoFilter::isAvailable()) {
        std::printf("SKIPPED: this build has no libavfilter\n");
        return 0;
    }
    const QImage image = interlacedFrame(64, 48);

    // No graph: the image is left alone
    {
        VideoFilter filter;
        CHECK(filter.setGraph(QString()).isEmpty());
        CHECK(!filter.isActive());
        CHECK(filter.filter(image, makeFormat(image, 64, 48), false) == image);
    }

    // A mistake in the graph is reported at once
    {
        VideoFilter filter;
        CHECK(!filter.setGraph("nosuchfilter=1").isEmpty());
        CHECK(!filter.isActive());
        CHECK(filter.setGraph("yadif=mode=send_frame").isEmpty());
        CHECK(filter.isActive());
    }

    // Every deinterlacer and scaler the dialogue offers exists in this ffmpeg
    for (const char *graph: { "yadif=mode=send_frame", "bwdif=mode=send_frame", "w3fdif=filter=complex:mode=frame",
                              "estdif=mode=frame", "scale=$W:$H:flags=bilinear", "scale=$W:$H:flags=bicubic",
                              "scale=$W:$H:flags=lanczos", "scale=$W:$H:flags=spline", "scale=$W:$H:flags=gauss",
                              "scale=$W:$H:flags=area", "scale=$W:$H:flags=neighbor" }) {
        VideoFilter filter;
        const QString problem = filter.setGraph(graph);
        if (!problem.isEmpty()) std::fprintf(stderr, "%s: %s\n", graph, qPrintable(problem));
        CHECK(problem.isEmpty());
    }

    // $W and $H give the size the picture is shown at
    {
        VideoFilter filter;
        CHECK(filter.setGraph("scale=$W:$H:flags=lanczos").isEmpty());
        const QImage result = filter.filter(image, makeFormat(image, 100, 90), false);
        CHECK(filter.lastError().isEmpty());
        CHECK(result.size() == QSize(100, 90));
    }

    // Deinterlacing the alternating white and black lines blends them
    {
        VideoFilter filter;
        CHECK(filter.setGraph("yadif=mode=send_frame").isEmpty());
        const QImage result = filter.filter(image, makeFormat(image, 64, 48), false);
        CHECK(filter.lastError().isEmpty());
        CHECK(result.size() == image.size());
        qint32 differing = 0;
        for (qint32 y = 8; y < 40; y++) {
            if (qGray(result.pixel(32, y)) != qGray(image.pixel(32, y))) differing++;
        }
        CHECK(differing > 8);
    }

    // Images in a sequence come out, though a filter that looks ahead holds the first back
    {
        VideoFilter filter;
        CHECK(filter.setGraph("yadif=mode=send_frame").isEmpty());
        qint32 received = 0;
        for (qint32 i = 0; i < 6; i++) {
            const QImage result = filter.filter(image, makeFormat(image, 64, 48), i > 0);
            CHECK(filter.lastError().isEmpty());
            if (!result.isNull()) {
                CHECK(result.size() == image.size());
                received++;
            }
        }
        CHECK(received >= 4);
    }

    // An image given the frames either side of it comes out whole, and is made of them (the same
    // image three times is just the image on its own)
    {
        VideoFilter filter;
        CHECK(filter.setGraph("yadif=mode=send_frame").isEmpty());
        const QImage same = filter.filterWithContext(image, image, image, makeFormat(image, 64, 48));
        CHECK(filter.lastError().isEmpty());
        CHECK(same.size() == image.size());
        CHECK(same == filter.filter(image, makeFormat(image, 64, 48), false));

        // Neighbours that differ from it change the result
        QImage moved(image.size(), QImage::Format_RGB32);
        for (qint32 y = 0; y < moved.height(); y++) {
            for (qint32 x = 0; x < moved.width(); x++) moved.setPixel(x, y, (y % 2 == 1) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        }
        const QImage withContext = filter.filterWithContext(moved, image, moved, makeFormat(image, 64, 48));
        CHECK(withContext.size() == image.size());
        CHECK(withContext != same);
    }

    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("All video filter tests passed\n");
    return 0;
}
