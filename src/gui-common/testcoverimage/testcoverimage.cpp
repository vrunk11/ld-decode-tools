/******************************************************************************
 * testcoverimage.cpp
 * ld-decode-tools - unit tests for the cover of an LDX file as the programs make it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "coverimage.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>

#include <cstdlib>
#include <iostream>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ << ": check failed: " #condition "\n"; \
            std::exit(1); \
        } \
    } while (0)

static QByteArray pngOf(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) image.setPixel(x, y, qRgb(x * 255 / std::max(1, width - 1), y * 255 / std::max(1, height - 1), 90));
    }
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    CHECK(image.save(&buffer, "PNG"));
    return data;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // a picture higher than 800 pixels is scaled down keeping its shape; one that is not is as it was
    QByteArray cover;
    QString suffix, error;
    CHECK(CoverImage::prepare(pngOf(1200, 1000), cover, suffix, &error) && suffix == "png");
    const QImage scaled = CoverImage::fromData(cover);
    CHECK(!scaled.isNull() && scaled.height() == 800 && scaled.width() == 960);
    CHECK(CoverImage::prepare(pngOf(160, 90), cover, suffix, &error));
    const QImage small = CoverImage::fromData(cover);
    CHECK(small.width() == 160 && small.height() == 90);
    // what is not a picture is refused with a reason
    CHECK(!CoverImage::prepare(QByteArray("not a picture"), cover, suffix, &error) && !error.isEmpty());
    CHECK(CoverImage::fromData(QByteArray()).isNull());
    std::cerr << "All the tests passed\n";
    return 0;
}
