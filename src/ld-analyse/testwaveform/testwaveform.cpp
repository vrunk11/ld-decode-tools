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
#include <QThread>
#include <QPushButton>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QMouseEvent>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audioengine.h"
#include "audioconfigdialog.h"
#include "efmdecoding.h"
#include "sidecaraudio.h"
#include "theme.h"
#include "ldxbytetrack.h"
#include "waveformpanel.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
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

    // With the samples of each field from the metadata the place in the audio is exact (here 700 samples a field, the first 100 longer)
    {
        QVector<qint64> cumulative { 0 };
        for (qint32 field = 0; field < 100; field++) cumulative.append(cumulative.last() + (field == 0 ? 800 : 700));
        engine.setFieldSamples(cumulative, 60.0);
        CHECK(engine.sampleAt(0.0) == 0);
        CHECK(engine.sampleAt(1.0 / 60.0) == 800);
        CHECK(engine.sampleAt(11.0 / 60.0) == 800 + 10 * 700);
        CHECK(engine.sampleAt(10.5 / 60.0) == 800 + 9 * 700 + 350);
        CHECK(std::fabs(engine.videoSecondsOf(800 + 10 * 700) - 11.0 / 60.0) < 1e-9);
        engine.setFieldSamples(QVector<qint64>(), 0.0);
        CHECK(engine.sampleAt(1.0) == rate);
    }

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
    const QRect area(view->mapTo(&panel, QPoint(0, 0)), view->size());
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


    // The heights of the panel (and, if asked, pictures of them to look at)
    const char *pictures = std::getenv("WAVEFORM_PICTURES");
    panel.setColor(QColor());
    panel.resize(700, 120);
    qint32 heights[3] = { 0, 0, 0 };
    for (qint32 d = 0; d < 3; d++) {
        panel.setDensity(static_cast<WaveformPanel::Density>(d));
        panel.resize(700, panel.maximumHeight());
        waitMs(50);
        heights[d] = panel.height();
        if (pictures) panel.grab().save(QString("%1/density%2.png").arg(pictures).arg(d));
    }
    CHECK(heights[0] > heights[1] && heights[1] > heights[2]);
    // The colours of the waveform are among those the theme editor offers
    bool editable = false;
    for (const ThemeColourRole &role: themeColourRoles()) editable |= qstrcmp(role.key, "WaveformEfm") == 0;
    CHECK(editable);
    panel.setDensity(WaveformPanel::Density::Compact);
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
        CHECK(engine.selectTrack(0));         // (the audio of the capture again)

        // The audio files beside a capture with its name: audio is found whatever the extension, RF samples (a FLAC at 40000 Hz) are not
        {
            QTemporaryDir folder;
            CHECK(folder.isValid());
            QFile(folder.filePath("cap.tbc")).open(QIODevice::WriteOnly);
            QFile(folder.filePath("cap.json")).open(QIODevice::WriteOnly);
            const auto make = [&folder](const QString &name, const QString &rate, const QString &channels) {
                QProcess maker;
                maker.start("ffmpeg", { "-v", "error", "-f", "lavfi", "-i", "sine=frequency=330:duration=1", "-ar", rate, "-ac", channels, folder.filePath(name) });
                return maker.waitForFinished(20000) && maker.exitCode() == 0;
            };
            CHECK(make("cap.wav", "44100", "2"));
            CHECK(make("cap.aiff", "48000", "1"));
            CHECK(make("cap.flac", "40000", "1"));           // (RF samples)
            CHECK(make("other.wav", "44100", "2"));          // (another name)
            const QStringList beside = SidecarAudio::find(folder.filePath("cap.tbc"));
            CHECK(beside.size() == 2);
            CHECK(beside[0].endsWith("cap.aiff") && beside[1].endsWith("cap.wav"));
            AudioFileDecoding decoding;
            bool finished = false;
            QObject::connect(&decoding, &AudioFileDecoding::finished, [&finished](bool, const QString &) { finished = true; });
            QString decodeError;
            CHECK(decoding.start(beside[0], decodeError));
            QElapsedTimer sideWait;
            sideWait.start();
            while (!finished && sideWait.elapsed() < 20000) waitMs(20);
            CHECK(finished && std::fabs(decoding.seconds() - 1.0) < 0.1);
        }
    }

    // EFM decoded by the programs, as it comes (when the EFM of a real capture is at hand)
    const QString realCapture = qEnvironmentVariableIsSet("WAVEFORM_NO_CAPTURE") ? QString("nothing.ldx") : QString("D:/decodde/test.ldx");        // (WAVEFORM_NO_CAPTURE: as on a machine that has none)
    if (QFile::exists(realCapture)) {
        for (EfmDecoding::Backend backend: { EfmDecoding::Backend::Suite, EfmDecoding::Backend::ProcessEfm }) {
            EfmDecoding decoding(backend);
            bool done = false, success = false;
            qint32 growths = 0;
            QObject::connect(&decoding, &EfmDecoding::finished, [&](bool ok, const QString &) { done = true; success = ok; });
            QObject::connect(&decoding, &EfmDecoding::grew, [&]() { growths++; engine.extend(); });
            QString error;
            CHECK(decoding.start(realCapture, EfmDecoding::Options(), error));
            const qint32 index = engine.addDecoded(backend == EfmDecoding::Backend::Suite ? 0 : 1, decoding.cacheFile(), "efm");
            CHECK(engine.selectTrack(index));
            QElapsedTimer decodeWait;
            decodeWait.start();
            while (!done && decodeWait.elapsed() < 120000) waitMs(50);
            CHECK(done && success);
            CHECK(!decoding.messages().isEmpty() && decoding.runSeconds() > 0.0);        // (the programs count what they decode: the statistics)
            engine.extend();
            CHECK(growths > 0 && decoding.seconds() > 10.0);
            CHECK(engine.samples() > 10 * 44100 && engine.peaks().samples() == engine.samples());
            engine.removeDecoded(backend == EfmDecoding::Backend::Suite ? 0 : 1);
            CHECK(engine.currentTrack() < 0);
            CHECK(engine.selectTrack(0));
        }
    }


    // How long the sound waits for audio while the waveform is being made (the player reads small pieces, the waveform big ones)
    if (std::getenv("WAVEFORM_BENCH") && QFile::exists(realCapture)) {
        const QVector<AudioTrackInfo> tracks = AudioReader::tracksOf(realCapture);
        CHECK(!tracks.isEmpty());
        for (qint32 separate = 0; separate < 2; separate++) {
            AudioReader player, shape;
            QString error;
            CHECK(player.open(tracks[0], error));
            CHECK(separate ? shape.open(tracks[0], error) : true);
            AudioReader &peaksSource = separate ? shape : player;
            AudioPeaks peaks(1024);
            std::atomic_bool cancel{false};
            QThread *thread = QThread::create([&]() { peaks.addAll(peaksSource, &cancel); });
            thread->start();
            QElapsedTimer total;
            total.start();
            qint64 worst = 0, position = 0;
            for (qint32 i = 0; i < 1500; i++) {
                QElapsedTimer one;
                one.start();
                player.read(position, 441);
                position += 441;
                worst = qMax(worst, one.nsecsElapsed());
                QThread::usleep(10000);
            }
            cancel = true;
            thread->wait();
            delete thread;
            std::fprintf(stderr, "bench: %s reader, worst read %.1f ms\n", separate ? "separate" : "shared", worst / 1.0e6);
        }
    }

    // The sound card, if there is one: how the clock of the sound moves, and that it can be started and stopped again and again
    if (std::getenv("WAVEFORM_BENCH") && QFile::exists(realCapture)) {
        AudioEngine player;
        const QVector<AudioTrackInfo> tracks = player.setVideo(realCapture);
        CHECK(!tracks.isEmpty());
        CHECK(player.selectTrack(0));
        waitMs(500);
        for (qint32 round = 0; round < 6; round++) {
            const bool started = player.start(1.0 + round);
            if (!started) {
                std::fprintf(stderr, "bench: start %d failed (no sound card?)\n", round);
                break;
            }
            QElapsedTimer run;
            run.start();
            double last = -1.0, biggestStep = 0.0, lastMoved = 0.0;
            qint32 steps = 0;
            qint64 stillMax = 0, lastChange = 0;
            while (run.elapsed() < (round == 0 ? 3000 : 700)) {
                const double now = player.playedSeconds();
                if (now != last) {
                    if (last >= 0) { biggestStep = qMax(biggestStep, now - last); steps++; }
                    lastChange = run.elapsed();
                    last = now;
                    lastMoved = now;
                }
                stillMax = qMax(stillMax, run.elapsed() - lastChange);
                waitMs(1);
            }
            std::fprintf(stderr, "bench: given to the card %.3f s ahead of the clock\n",
                         static_cast<double>(player.samplePlaying()) / 44100.0 - (1.0 + round) - lastMoved);
            std::fprintf(stderr, "bench: round %d, clock %.3f s after %lld ms, %d steps, biggest step %.1f ms, longest still %lld ms\n",
                         round, lastMoved, static_cast<long long>(run.elapsed()), steps, biggestStep * 1000.0, static_cast<long long>(stillMax));
            player.stop();
        }
    }
    // A live input: the EFM is given as it comes, a piece at a time, and the audio is the same as from the file
    if (QFile::exists(realCapture)) {
        LdxByteTrack source;
        QString sourceError;
        CHECK(source.openFile(realCapture, LdxByteTrack::Kind::Efm, sourceError));
        const QByteArray efmBytes = source.readAll();
        CHECK(efmBytes.size() > 1000000);
        CHECK(source.size() == efmBytes.size());       // (efmSourceName tells EFM from none by the size)
        qint64 sizes[2] = { 0, 0 };
        for (qint32 i = 0; i < 2; i++) {
            for (qint32 mode = 0; mode < 2; mode++) {
                const EfmDecoding::Backend backend = i == 0 ? EfmDecoding::Backend::Suite : EfmDecoding::Backend::ProcessEfm;
                EfmDecoding decoding(backend);
                bool done = false;
                QObject::connect(&decoding, &EfmDecoding::finished, [&](bool, const QString &) { done = true; });
                QString error;
                CHECK(decoding.start(mode == 0 ? realCapture : QStringLiteral("-"), EfmDecoding::Options(), error));
                if (mode == 1) {
                    // (nothing for a moment, as when the EFM comes late)
                    waitMs(200);
                    for (qint64 at = 0; at < efmBytes.size(); at += 300000) {
                        decoding.feed(efmBytes.mid(at, 300000));
                        waitMs(5);
                    }
                    decoding.finishInput();
                }
                QElapsedTimer decodeWait;
                decodeWait.start();
                while (!done && decodeWait.elapsed() < 120000) waitMs(50);
                CHECK(done);
                QFile result(decoding.cacheFile());
                CHECK(result.open(QIODevice::ReadOnly));
                if (mode == 0) sizes[i] = result.size();
                else CHECK(result.size() == sizes[i] && sizes[i] > 0);
            }
        }
    }

    // The EFM source can be asked for when the EFM may be decoded: it is there to choose, and choosing it asks for a decoder
    {
        WaveformPanel asking(&engine);
        bool asked = false;
        QObject::connect(&asking, &WaveformPanel::efmDecodeRequested, [&asked]() { asked = true; });
        asking.setEfmPossible([]() { return true; });
        asking.setTracks(engine.tracks(), 0);
        QComboBox *sources = asking.findChildren<QComboBox *>().first();
        const qint32 efmIndex = sources->findData("efm");
        CHECK(efmIndex >= 0 && sources->itemText(efmIndex).contains("decode"));
        const qint32 before = sources->currentIndex();
        sources->setCurrentIndex(efmIndex);
        CHECK(asked && sources->currentIndex() == before);
        WaveformPanel refusing(&engine);
        refusing.setEfmPossible([]() { return false; });
        refusing.setTracks(engine.tracks(), 0);
        QComboBox *refusedSources = refusing.findChildren<QComboBox *>().first();
        CHECK(refusedSources->itemText(refusedSources->findData("efm")).contains("none"));
    }

    // Switching between the analogue audio and audio decoded from EFM, playing, there is sound each time (when there is a sound card)
    if (QFile::exists(realCapture)) {
        AudioEngine player;
        player.setVideo(realCapture);
        CHECK(player.hasAudio());
        EfmDecoding decoding(EfmDecoding::Backend::ProcessEfm);
        QString error;
        CHECK(decoding.start(realCapture, EfmDecoding::Options(), error));
        const qint32 efmIndex = player.addDecoded(0, decoding.cacheFile(), "efm");
        QElapsedTimer decodeWait;
        decodeWait.start();
        while (decoding.isRunning() && decodeWait.elapsed() < 120000) { waitMs(50); player.extend(); }
        const qint32 analogueIndex = 0;
        CHECK(player.tracks()[analogueIndex].source == "analogue");
        bool played = true;
        for (qint32 round = 0; round < 4 && played; round++) {
            const qint32 index = round % 2 == 0 ? analogueIndex : efmIndex;
            CHECK(player.selectTrack(index));
            if (!player.start(2.0)) { played = false; break; }       // (no sound card: nothing to test)
            waitMs(500);
            const qint32 loudest = player.takeOutputLevel();
            std::fprintf(stderr, "switch: round %d, track %d, loudest sample %d, %lld pairs of audio\n", round, index, loudest,
                         static_cast<long long>(player.pairsGiven()));
            CHECK(loudest > 100 && player.pairsGiven() > 1000);
            player.stop();
        }
    }

    // How the decoders give audio when the EFM comes at the pace of a live capture (about 0.9 MB a second of video)
    if (std::getenv("WAVEFORM_BENCH") && QFile::exists(realCapture)) {
        LdxByteTrack source;
        QString sourceError;
        CHECK(source.openFile(realCapture, LdxByteTrack::Kind::Efm, sourceError));
        const QByteArray efmBytes = source.readAll();
        for (qint32 i = 0; i < 2; i++) {
            EfmDecoding decoding(i == 0 ? EfmDecoding::Backend::Suite : EfmDecoding::Backend::ProcessEfm);
            QString error;
            CHECK(decoding.start("-", EfmDecoding::Options(), error));
            QElapsedTimer clock;
            clock.start();
            double fedSeconds = 0.0;
            for (qint64 at = 0; at < efmBytes.size(); at += 90000) {
                decoding.feed(efmBytes.mid(at, 90000));
                fedSeconds += 90000.0 / efmBytes.size() * 24.2;
                while (clock.elapsed() < (at / 90000 + 1) * 100) waitMs(5);       // (a tenth of a second for each piece: ten times real time)
                if ((at / 90000) % 25 == 24) {
                    std::fprintf(stderr, "bench: %s, %.1f s of EFM given, %.1f s of audio out\n", i == 0 ? "suite" : "ld-process-efm", fedSeconds, decoding.seconds());
                }
            }
            decoding.finishInput();
            waitMs(1500);
            std::fprintf(stderr, "bench: %s, at the end %.1f s of audio out\n", i == 0 ? "suite" : "ld-process-efm", decoding.seconds());
        }
    }

    // One channel shown on the whole height when the other is muted, if that is asked for
    {
        // (the engine may have no track chosen: the test of the imported audio, which needs ffmpeg, leaves it with none)
        CHECK(engine.selectTrack(0));
        QElapsedTimer peaksWait;
        peaksWait.start();
        while (!engine.peaksComplete() && peaksWait.elapsed() < 10000) waitMs(20);
        CHECK(engine.peaksComplete());
        WaveformPanel solo(&engine);        // (a panel of its own, of a known size: what the others did to theirs does not count)
        solo.setTracks(engine.tracks(), 0);
        solo.setDensity(WaveformPanel::Density::Large);
        solo.resize(500, 110);
        solo.show();
        waitMs(100);
        solo.setColor(QColor(255, 0, 0));
        const auto redCount = [&solo]() {
            waitMs(50);
            const QImage picture = solo.grab().toImage();
            const QWidget *shown = solo.findChild<WaveformView *>();
            const QRect where(shown->mapTo(&solo, QPoint(0, 0)), shown->size());
            qint32 count = 0;
            for (qint32 y = where.top(); y < where.bottom(); y++) {
                for (qint32 x = where.left(); x < where.right(); x++) {
                    const QColor pixel = picture.pixelColor(x, y);
                    if (pixel.red() > 150 && pixel.green() < 120 && pixel.blue() < 120) count++;
                }
            }
            return count;
        };
        solo.setSoloChannel(false);
        solo.setMix(true, false, 70);
        const qint32 shared = redCount();             // (the left channel has the upper half)
        solo.setSoloChannel(true);
        const qint32 whole = redCount();              // (the left channel alone has all of it)
        std::fprintf(stderr, "solo: shared %d, whole %d, view %dx%d\n", shared, whole, solo.findChild<WaveformView *>()->width(), solo.findChild<WaveformView *>()->height());
        CHECK(solo.findChild<WaveformView *>()->height() >= 60);
        CHECK(whole > shared * 6 / 5);
        solo.setMix(true, true, 70);
        CHECK(redCount() < whole);                    // (both on: two channels again)
        // Nothing is heard when both channels, or M, are muted: the waveform is grey, not in its colour
        solo.setMix(false, false, 70);
        CHECK(redCount() == 0);
        solo.setMix(true, true, 70);
        CHECK(redCount() > 0);
        solo.setMuted(true);
        solo.refreshSilence();
        CHECK(redCount() == 0);
        solo.setMuted(false);
        CHECK(redCount() > 0);
        solo.setSoloChannel(false);
        solo.setDensity(WaveformPanel::Density::Compact);
    }

    // A growing track that begins later in the capture (audio decoded from a part of the EFM, or that came late): the place in it of a
    // moment of the video is counted from there
    {
        QTemporaryFile cacheFile;
        CHECK(cacheFile.open());
        cacheFile.write(QByteArray(4 * 44100 * 3, 'x'));
        cacheFile.flush();
        AudioEngine late;
        late.setVideo(QString());
        const qint32 index = late.addGrowing(5, "efm", cacheFile.fileName(), "late", 44100.0, 5.0);
        CHECK(late.selectTrack(index));
        CHECK(late.sampleAt(7.0) == 2 * 44100);
        CHECK(std::fabs(late.videoSecondsOf(2 * 44100) - 7.0) < 1e-9);
        late.setGrowingStart(5, 6.0);
        CHECK(late.sampleAt(7.0) == 44100);
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
