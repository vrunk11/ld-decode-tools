/******************************************************************************
 * testdropoutdrawing.cpp
 * ld-decode-tools - unit tests for DropoutDrawing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QGuiApplication>
#include <QPainter>
#include <cstdlib>
#include <iostream>

#include "dropoutdrawing.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static constexpr qint32 WIDTH = 40;
static constexpr qint32 FIELD_HEIGHT = 12;
static constexpr qint32 FRAME_HEIGHT = FIELD_HEIGHT * 2 - 1;

// Dropouts that are always the same: on every line, inside the picture and across its ends, and some outside it
static DropOuts makeDropouts()
{
    DropOuts dropOuts;
    quint32 state = 99;
    for (qint32 i = 0; i < 60; i++) {
        state = state * 1664525u + 1013904223u;
        const qint32 line = 1 + static_cast<qint32>((state >> 8) % FIELD_HEIGHT);
        state = state * 1664525u + 1013904223u;
        const qint32 start = static_cast<qint32>((state >> 8) % (WIDTH + 6)) - 3;
        state = state * 1664525u + 1013904223u;
        dropOuts.append(start, start + static_cast<qint32>((state >> 8) % 9), line);
    }
    dropOuts.append(0, WIDTH - 1, FIELD_HEIGHT);
    dropOuts.append(5, 5, 1);
    dropOuts.append(-5, 3, 2);
    dropOuts.append(WIDTH - 2, WIDTH + 10, 3);
    dropOuts.append(2, 6, FIELD_HEIGHT + 3);
    return dropOuts;
}

static QImage blank()
{
    QImage image(WIDTH, FRAME_HEIGHT, QImage::Format_RGB32);
    image.fill(Qt::black);
    return image;
}

// The drawing as ld-analyse did it before: a painter and a line for each dropout
static QImage drawnWithPainter(const DropOuts &dropOuts, bool secondField, const QColor &color, DropoutDrawing::Layout layout)
{
    QImage image = blank();
    QPainter painter(&image);
    painter.setPen(color);
    for (qint32 i = 0; i < dropOuts.size(); i++) {
        const qint32 startx = dropOuts.startx(i), endx = dropOuts.endx(i), fieldLine = dropOuts.fieldLine(i);
        switch (layout) {
        case DropoutDrawing::Layout::Frame: {
            const qint32 lineY = secondField ? (fieldLine * 2) - 1 : (fieldLine - 1) * 2;
            painter.drawLine(startx, lineY, endx, lineY);
            break;
        }
        case DropoutDrawing::Layout::Split: {
            const qint32 lineY = secondField ? fieldLine + (FRAME_HEIGHT / 2) : fieldLine - 1;
            painter.drawLine(startx, lineY, endx, lineY);
            break;
        }
        case DropoutDrawing::Layout::FieldStretched: {
            const qint32 lineY = fieldLine - 1;
            const qint32 down = secondField ? 1 : 0;
            painter.drawLine(startx, lineY * 2 + down, endx, lineY * 2 + down);
            painter.drawLine(startx, lineY * 2 + 1 + down, endx, lineY * 2 + 1 + down);
            break;
        }
        case DropoutDrawing::Layout::Field: {
            const qint32 lineY = fieldLine - 1 + (FRAME_HEIGHT / 4);
            painter.drawLine(startx, lineY, endx, lineY);
            break;
        }
        }
    }
    painter.end();
    return image;
}

static void testAsBefore()
{
    std::cerr << "Testing DropoutDrawing::draw against the painter of ld-analyse\n";

    const DropOuts dropOuts = makeDropouts();
    using Layout = DropoutDrawing::Layout;
    for (Layout layout: { Layout::Frame, Layout::Split, Layout::Field, Layout::FieldStretched }) {
        for (bool secondField: { false, true }) {
            for (const QColor &color: { QColor(Qt::red), QColor(Qt::blue), QColor(30, 200, 90) }) {
                QImage image = blank();
                DropoutDrawing::draw(image, dropOuts, secondField, color, layout, FRAME_HEIGHT);
                CHECK(image == drawnWithPainter(dropOuts, secondField, color, layout));
            }
        }
    }
}

static void testPixels()
{
    std::cerr << "Testing DropoutDrawing::draw pixel by pixel\n";

    DropOuts dropOuts;
    dropOuts.append(3, 6, 2);
    const QColor color(10, 20, 30);

    // Frame: the line 2 of the first field is the row 2, of the second the row 3
    QImage image = blank();
    DropoutDrawing::draw(image, dropOuts, false, color);
    CHECK(image.pixelColor(2, 2) == QColor(Qt::black) && image.pixelColor(3, 2) == color && image.pixelColor(6, 2) == color && image.pixelColor(7, 2) == QColor(Qt::black));
    CHECK(image.pixelColor(3, 3) == QColor(Qt::black) && image.pixelColor(3, 1) == QColor(Qt::black));
    image = blank();
    DropoutDrawing::draw(image, dropOuts, true, color);
    CHECK(image.pixelColor(3, 3) == color && image.pixelColor(3, 2) == QColor(Qt::black));

    // Nothing for an image that is not RGB32, or no image
    QImage other(WIDTH, FRAME_HEIGHT, QImage::Format_ARGB32);
    other.fill(Qt::black);
    DropoutDrawing::draw(other, dropOuts, false, color);
    CHECK(other.pixelColor(3, 2) == QColor(Qt::black));
    QImage none;
    DropoutDrawing::draw(none, dropOuts, false, color);
    CHECK(none.isNull());
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    testAsBefore();
    testPixels();
    std::cerr << "All tests passed\n";
    return 0;
}
