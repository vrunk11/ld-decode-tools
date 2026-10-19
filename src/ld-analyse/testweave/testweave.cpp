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
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "playbackcontroller.h"
#include "playbackrenderer.h"
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

    // What a picture costs (WEAVE_TIMES=1): a frame and a weave of fields, in colour, with the comb filter in 2 and in 3 dimensions
    if (std::getenv("WEAVE_TIMES")) {
        for (const qint32 dimensions: { 2, 3 }) {
            PalColour::Configuration pal = source.getPalConfiguration();
            Comb::Configuration ntsc = source.getNtscConfiguration();
            OutputWriter::Configuration output = source.getOutputConfiguration();
            ntsc.dimensions = dimensions;
            output.paddingAmount = 1;
            source.setChromaConfiguration(pal, ntsc, output);
            source.setChromaDecoder(true);
            QElapsedTimer timer;
            timer.start();
            for (qint32 frame = 300; frame < 330; frame++) {
                source.load(frame, 2 * frame - 1);
                source.getImage();
            }
            const double frameMs = timer.elapsed() / 30.0;
            timer.restart();
            for (qint32 p = 600; p < 630; p++) {
                source.loadWeave(p);
                source.getImage();
            }
            const double weaveMs = timer.elapsed() / 30.0;
            std::fprintf(stderr, "testweave: %dD comb: a frame %.1f ms, a weave %.1f ms\n", dimensions, frameMs, weaveMs);
        }
        source.setChromaDecoder(false);
    }
    source.setChromaDecoder(false);

    // The field view is a bob: the lines of the top field are doubled on the rows 2k and 2k+1, those of the bottom field on the rows
    // 2k+1 and 2k+2
    source.setViewMode(TbcSource::ViewMode::FIELD_VIEW);
    source.setStretchField(true);
    source.load(300, 599);
    const QImage top = source.getImage();
    source.load(301, 602);
    const QImage bottom = source.getImage();
    CHECK(!top.isNull() && !bottom.isNull());
    if (pictures) {
        top.save(QString("%1/field-top.png").arg(pictures));
        bottom.save(QString("%1/field-bottom.png").arg(pictures));
    }
    const auto sameRows = [](const QImage &image, qint32 a, qint32 b) {
        return std::memcmp(image.constScanLine(a), image.constScanLine(b), static_cast<size_t>(image.width()) * 4) == 0;
    };
    CHECK(sameRows(top, 100, 101) && !sameRows(top, 101, 102));
    CHECK(sameRows(bottom, 101, 102) && !sameRows(bottom, 100, 101));
    step("the field view bobs");
    source.setViewMode(TbcSource::ViewMode::FRAME_VIEW);

    // A real frame after the weaves is a real frame again
    source.load(300, 599);
    const QImage frame = source.getImage();
    CHECK(!frame.isNull());
    if (pictures) frame.save(QString("%1/frame300.png").arg(pictures));
    // Looping: the decoders go to the end of the range; when the clock goes back to the first position they must be started again,
    // or no image comes for the positions of the second round (they only go forward)
    {
        PlaybackRenderer renderer;
        const quint32 generation = source.getSettingsGeneration();
        const auto renderRound = [&](const char *name) {
            renderer.start(source.getRenderSettings(), generation, 1, 6, nullptr);
            QElapsedTimer roundWait;
            roundWait.start();
            qint32 seen = 0, position = 0;
            for (qint32 due = 1; due <= 6 && roundWait.elapsed() < 60000; due++) {
                renderer.setDue(due);
                QImage image;
                while (!renderer.take(due, position, image) && roundWait.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!image.isNull()) seen++;
            }
            std::fprintf(stderr, "testweave: %s: %d images\n", name, seen);
            CHECK(seen >= 3);
        };
        renderRound("first round");
        renderRound("second round, the decoders started again");
        renderer.stop();
    }
    // The speeds: the clock gives as many positions a second as the rate times the speed, no more (nothing is made in between)
    for (const double speed: { 0.25, 0.5, 1.0 }) {
        PlaybackController clock;
        clock.setRate(30000.0 / 1001.0);
        clock.setSpeed(speed);
        clock.setRange(1, 100000);
        qint32 given = 0;
        QObject::connect(&clock, &PlaybackController::positionDue, [&given](qint32) { given++; });
        clock.play(1);
        QElapsedTimer run;
        run.start();
        while (run.elapsed() < 2000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        clock.pause();
        const double perSecond = given / (run.elapsed() / 1000.0);
        std::fprintf(stderr, "testweave: speed %.2f: %.1f positions a second\n", speed, perSecond);
        CHECK(std::fabs(perSecond - 29.97 * speed) < 29.97 * speed * 0.25 + 1.0);
    }
    step("all tests passed");
    return 0;
}

