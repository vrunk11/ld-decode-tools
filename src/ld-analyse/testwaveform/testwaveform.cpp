/******************************************************************************
 * testwaveform.cpp
 * ld-analyse - unit test for the audio engine's peaks and the waveform panel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audioengine.h"
#include "audioconfigdialog.h"
#include "waveformpanel.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (0)

static void waitMs(int milliseconds)
{
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < milliseconds) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    // Two seconds of audio beside a .tbc: a sine on the left, silence on the right
    const qint32 rate = 44100;
    QByteArray pcm;
    for (qint32 i = 0; i < 2 * rate; i++) {
        const qint16 left = static_cast<qint16>(20000 * std::sin(2.0 * M_PI * 440.0 * i / rate));
        const qint16 right = 0;
        pcm.append(reinterpret_cast<const char *>(&left), 2);
        pcm.append(reinterpret_cast<const char *>(&right), 2);
    }
    const QString tbc = directory.filePath("capture.tbc");
    QFile(tbc).open(QIODevice::WriteOnly);
    QFile file(directory.filePath("capture.pcm"));
    CHECK(file.open(QIODevice::WriteOnly));
    file.write(pcm);
    file.close();

    AudioEngine engine;
    CHECK(!engine.hasAudio());
    CHECK(engine.setVideo(tbc).size() == 1);
    CHECK(engine.hasAudio());
    CHECK(engine.selectTrack(0));
    CHECK(engine.samples() == 2 * rate && engine.sampleRate() == rate);
    CHECK(std::fabs(engine.durationSeconds() - 2.0) < 1e-9);
    CHECK(std::fabs(engine.videoSecondsOf(engine.sampleAt(1.0)) - 1.0) < 1e-4);
    engine.setOffsetMs(100);
    CHECK(engine.sampleAt(1.1) == rate);            // (the audio is 100 ms later than the video)
    engine.setOffsetMs(0);

    // The peaks are made on a thread of their own
    QElapsedTimer timer;
    timer.start();
    while (!engine.peaksComplete() && timer.elapsed() < 10000) waitMs(20);
    CHECK(engine.peaksComplete());
    CHECK(engine.peaks().samples() == 2 * rate);
    const AudioPeaks::Peak all = engine.peaks().range(0, 2 * rate);
    CHECK(all.maxLeft > 19000 && all.minLeft < -19000 && all.maxRight == 0 && all.minRight == 0);

    // The waveform is drawn: the left half has the colour of the waveform, the right half none, and the line is where the video is
    WaveformPanel panel(&engine);
    panel.resize(500, 120);
    panel.setColor(QColor(255, 0, 0));
    panel.setTracks(engine.tracks(), 0);
    panel.setPlayheadSeconds(1.0);
    panel.show();
    waitMs(50);
    const QImage image = panel.grab().toImage();
    CHECK(!image.isNull());
    const QWidget *view = panel.findChild<WaveformView *>();
    CHECK(view);
    const QRect area = view->geometry();
    qint32 redLeft = 0, redRight = 0;
    for (qint32 y = area.top(); y < area.bottom(); y++) {
        for (qint32 x = area.left(); x < area.right(); x++) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 150 && pixel.green() < 120 && pixel.blue() < 120) {
                if (y < area.top() + area.height() / 2) redLeft++; else redRight++;
            }
        }
    }
    CHECK(redLeft > 200);           // (the sine fills the left half)
    CHECK(redRight <= 2 * area.width() && redLeft > 4 * redRight);      // (the right channel is silent: a thin line)
    // The playhead: a line in the highlight colour in the middle of the view
    const QColor highlight = panel.palette().color(QPalette::Highlight);
    const qint32 middle = area.left() + area.width() / 2;
    bool line = false;
    for (qint32 x = middle - 2; x <= middle + 2; x++) {
        const QColor pixel = image.pixelColor(x, area.top() + 2);
        if (std::abs(pixel.red() - highlight.red()) < 10 && std::abs(pixel.green() - highlight.green()) < 10) line = true;
    }
    CHECK(line);

    // A click asks to go to a place
    double asked = -1.0;
    QObject::connect(&panel, &WaveformPanel::seekRequested, [&asked](double seconds) { asked = seconds; });
    const QPointF at(view->width() / 4, 10);
    QMouseEvent press(QEvent::MouseButtonPress, at, view->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(const_cast<QWidget *>(view), &press);
    CHECK(std::fabs(asked - 0.5) < 0.02);


    // Imported audio: ffmpeg reads a file, the dialog gives the samples cut to the length of the video, the engine plays them as a track
    QProcess probe;
    probe.start("ffmpeg", { "-version" });
    if (probe.waitForFinished(5000) && probe.exitCode() == 0) {
        const QString tone = directory.filePath("tone.wav");
        QProcess make;
        make.start("ffmpeg", { "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-ar", "22050", "-ac", "1", tone });
        CHECK(make.waitForFinished(20000) && make.exitCode() == 0);
        AudioConfigDialog dialog;
        dialog.setVideo(1.0, QString());
        QVector<qint16> got;
        double gotRate = 0;
        QObject::connect(&dialog, &AudioConfigDialog::imported, [&](const QVector<qint16> &samples, double rate, const QString &) { got = samples; gotRate = rate; });
        dialog.findChild<QLineEdit *>()->setText(tone);
        for (QPushButton *button: dialog.findChildren<QPushButton *>()) {
            if (button->text() == "Use") button->click();
        }
        QElapsedTimer wait;
        wait.start();
        while (got.isEmpty() && wait.elapsed() < 20000) waitMs(20);
        CHECK(gotRate == 44100.0);
        CHECK(got.size() == 2 * 44100);                 // (one second, cut to the video, in stereo)
        const qint32 index = engine.setImported(got, gotRate, "tone");
        CHECK(engine.selectTrack(index));
        CHECK(engine.samples() == 44100 && engine.tracks()[index].source == "import");
        engine.clearImported();
        CHECK(engine.currentTrack() < 0);
    }
    // The channels and the volume
    bool left = true, right = true;
    qint32 volume = 0;
    QObject::connect(&panel, &WaveformPanel::mixChanged, [&](bool l, bool r, qint32 v) { left = l; right = r; volume = v; });
    panel.setMix(true, true, 70);
    CHECK(panel.leftOn() && panel.rightOn() && panel.volume() == 70);

    std::fprintf(stderr, "testwaveform: all tests passed\n");
    return 0;
}
