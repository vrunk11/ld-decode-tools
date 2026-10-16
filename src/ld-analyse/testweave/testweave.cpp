/******************************************************************************
 * testweave.cpp
 * ld-analyse - unit test for the weave of two successive fields (frame view at field rate)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <cstdio>
#include <cstdlib>

#include "tbcsource.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (0)

static void step(const char *text)
{
    std::fprintf(stderr, "testweave: %s\n", text);
    std::fflush(stderr);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // (a real capture is needed: the test does nothing without one)
    const QString capture = "D:/decodde/test.ldx";
    if (!QFile::exists(capture)) {
        step("no capture to test with, skipped");
        return 0;
    }
    TbcSource source;
    source.loadSource(capture);
    QElapsedTimer wait;
    wait.start();
    while (!source.getIsSourceLoaded() && wait.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(source.getIsSourceLoaded());
    step("loaded");
    source.setViewMode(TbcSource::ViewMode::FRAME_VIEW);

    // Without the colour decoder: the fields as they are, one above the other
    const char *pictures = std::getenv("WEAVE_PICTURES");
    QImage previous;
    for (qint32 p = 600; p < 606; p++) {
        source.loadWeave(p);
        const QImage image = source.getImage();
        CHECK(!image.isNull() && image.height() > 400);
        if (pictures) image.save(QString("%1/weave%2.png").arg(pictures).arg(p));
        if (previous.isNull() == false) CHECK(image != previous);       // (each step is another picture)
        previous = image;
    }
    step("weaves of the fields done");

    // With the colour decoder: the weaves are decoded as frames are
    {
        // (a decoder that is set up: the window does it from its dialog)
        PalColour::Configuration pal = source.getPalConfiguration();
        Comb::Configuration ntsc = source.getNtscConfiguration();
        OutputWriter::Configuration output = source.getOutputConfiguration();
        ntsc.dimensions = 2;
        output.paddingAmount = 1;
        source.setChromaConfiguration(pal, ntsc, output);
    }
    source.setChromaDecoder(true);
    source.load(300, 599);
    const QImage colourFrame = source.getImage();
    CHECK(!colourFrame.isNull());
    step("a frame in colour done");
    QImage previousColour;
    for (qint32 p = 600; p < 604; p++) {
        source.loadWeave(p);
        const QImage image = source.getImage();
        CHECK(!image.isNull());
        if (pictures) image.save(QString("%1/colour%2.png").arg(pictures).arg(p));
        if (previousColour.isNull() == false) CHECK(image != previousColour);
        previousColour = image;
    }
    step("weaves in colour done");
    source.setChromaDecoder(false);

    // A real frame after the weaves is a real frame again
    source.load(300, 599);
    const QImage frame = source.getImage();
    CHECK(!frame.isNull());
    if (pictures) frame.save(QString("%1/frame300.png").arg(pictures));
    step("all tests passed");
    return 0;
}
