/******************************************************************************
 * testsnrplot.cpp
 * ld-stacker-gui - unit tests for SnrPlot
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "snrplot.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

// How many pixels of the image are of a colour (what the curve is drawn in), and in which columns
static qint32 pixelsOf(const QImage &image, const QColor &color, qint32 &firstColumn, qint32 &lastColumn)
{
    qint32 count = 0;
    firstColumn = image.width();
    lastColumn = -1;
    for (qint32 y = 0; y < image.height(); y++) {
        for (qint32 x = 0; x < image.width(); x++) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - color.red()) < 40 && std::abs(pixel.green() - color.green()) < 40 && std::abs(pixel.blue() - color.blue()) < 40) {
                count++;
                firstColumn = qMin(firstColumn, x);
                lastColumn = qMax(lastColumn, x);
            }
        }
    }
    return count;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cerr << "Testing SnrPlot\n";

    // A capture of an hour: 100 000 frames, a curve with a gap in the middle (frames that have no value)
    const qint32 frames = 100000;
    SnrCurve curve;
    curve.color = QColor(220, 30, 30);
    curve.values.resize(frames);
    double value = 30.0;
    for (qint32 i = 0; i < frames; i++) {
        value += (std::rand() % 200 - 100) / 400.0;
        curve.values[i] = (i >= 40000 && i < 50000) ? std::nanf("") : static_cast<float>(value);
    }

    SnrPlot plot;
    plot.resize(800, 120);
    plot.setCurves({ curve });
    plot.setMarker(1);

    QImage first = plot.grab().toImage();
    qint32 left, right;
    CHECK(pixelsOf(first, curve.color, left, right) > 500);
    // The curve goes over the width of the plot, not of the margin on the right
    CHECK(left < 5);
    CHECK(right > 800 - 60 - 10 && right < 800 - 50);

    // The gap is a gap: no pixel of the curve in the columns of the frames that have no value (about 40% to 50% of the width of 744 pixels)
    qint32 inGap = 0;
    for (qint32 y = 0; y < first.height(); y++) {
        for (qint32 x = 300; x < 365; x++) {
            const QColor pixel = first.pixelColor(x, y);
            if (std::abs(pixel.red() - curve.color.red()) < 40 && std::abs(pixel.green() - curve.color.green()) < 40 && std::abs(pixel.blue() - curve.color.blue()) < 40) inGap++;
        }
    }
    CHECK(inGap == 0);

    // The cursor and the marker move about without the curve being drawn again: a hundred pictures take a moment
    QElapsedTimer timer;
    timer.start();
    for (qint32 i = 0; i < 100; i++) {
        plot.setMarker(1 + i * 900);
        plot.grab();
    }
    const qint64 elapsed = timer.elapsed();
    std::cerr << "  100 pictures with the marker on the move: " << elapsed << " ms\n";
    CHECK(elapsed < 3000);

    // The curve is still where it was when the marker has moved
    qint32 left2, right2;
    CHECK(pixelsOf(plot.grab().toImage(), curve.color, left2, right2) > 500);
    CHECK(left2 < 5);

    std::cerr << "All tests passed\n";
    return 0;
}
