/******************************************************************************
 * teststackaudio.cpp
 * ld-analyse - unit test for the audio of a stack
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

#include "stackaudio.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (false)

static void testChoose()
{
    std::fprintf(stderr, "Testing which source is heard where\n");
    // Two sources that do not overlap: the first, silence, the second
    auto segments = StackAudio::choose({ 0, 20 }, { 10, 30 }, { true, true }, 30);
    CHECK(segments.size() == 3);
    CHECK(segments[0].from == 0 && segments[0].to == 10 && segments[0].source == 0);
    CHECK(segments[1].from == 10 && segments[1].to == 20 && segments[1].source == -1);
    CHECK(segments[2].from == 20 && segments[2].to == 30 && segments[2].source == 1);

    // Sources that overlap: the one that goes on the longest, until it ends; then the next one that covers the place
    segments = StackAudio::choose({ 0, 5, 12 }, { 15, 25, 20 }, { true, true, true }, 25);
    CHECK(segments.size() == 2);
    CHECK(segments[0].from == 0 && segments[0].to == 15 && segments[0].source == 0);
    CHECK(segments[1].from == 15 && segments[1].to == 25 && segments[1].source == 1);

    // A source that has no audio is not chosen, however long it is
    segments = StackAudio::choose({ 0, 0 }, { 30, 10 }, { false, true }, 30);
    CHECK(segments.size() == 2);
    CHECK(segments[0].source == 1 && segments[0].to == 10);
    CHECK(segments[1].source == -1 && segments[1].from == 10 && segments[1].to == 30);

    // No audio at all: nothing to hear
    segments = StackAudio::choose({ 0 }, { 30 }, { false }, 30);
    CHECK(segments.size() == 1 && segments[0].source == -1);

    // The timeline ends before a source does
    segments = StackAudio::choose({ 0 }, { 50 }, { true }, 30);
    CHECK(segments.size() == 1 && segments[0].to == 30);
}

static void writePcm(const QString &name, qint16 value, qint64 pairs)
{
    QFile file(name);
    CHECK(file.open(QIODevice::WriteOnly));
    QByteArray data(static_cast<qsizetype>(pairs * 4), 0);
    qint16 *samples = reinterpret_cast<qint16 *>(data.data());
    for (qint64 i = 0; i < pairs * 2; i++) samples[i] = value;
    file.write(data);
}

static void testFile()
{
    std::fprintf(stderr, "Testing the audio that is made\n");
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const double fps = 25.0;
    const qint64 perFrame = 44100 / 25;      // (1764 pairs)
    // A source of 10 frames at the start, another of 10 frames from the frame 20: a pcm file beside each .tbc name
    for (const char *name : { "a", "b" }) {
        QFile tbc(directory.filePath(QString("%1.tbc").arg(name)));
        CHECK(tbc.open(QIODevice::WriteOnly));
    }
    writePcm(directory.filePath("a.pcm"), 1000, perFrame * 12);
    writePcm(directory.filePath("b.pcm"), 2000, perFrame * 12);

    QVector<StackAudioSpan> spans(2);
    spans[0].reference = directory.filePath("a.tbc");
    spans[0].start = 0;
    spans[0].frames = 10;
    spans[1].reference = directory.filePath("b.tbc");
    spans[1].start = 20;
    spans[1].frames = 10;

    StackAudio audio;
    QString error;
    CHECK(audio.start(spans, 30, fps, StackAudio::Kind::Analogue, {}, error));
    QEventLoop loop;
    bool done = false;
    bool success = false;
    QObject::connect(&audio, &StackAudio::finished, &loop, [&](bool ok, const QString &) { done = true; success = ok; loop.quit(); });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(done && success);

    QFile result(audio.cacheFile());
    CHECK(result.open(QIODevice::ReadOnly));
    const QByteArray data = result.readAll();
    CHECK(data.size() == 30 * perFrame * 4);
    const qint16 *samples = reinterpret_cast<const qint16 *>(data.constData());
    auto at = [&](qint64 frame, qint64 pair) { return samples[(frame * perFrame + pair) * 2]; };
    CHECK(at(5, 100) == 1000);          // the first source, in the middle of its stretch
    CHECK(at(0, 0) == 0);               // (it fades in)
    CHECK(at(15, 100) == 0);            // no source: silence
    CHECK(at(25, 100) == 2000);         // the second one
    CHECK(audio.description().contains("source 1") && audio.description().contains("source 2"));

    // The other kinds: these sources have only analogue audio, so there is no digital or imported audio, and the mix is the analogue audio
    CHECK(!StackAudio::available(spans, StackAudio::Kind::Digital, {}));
    CHECK(!StackAudio::available(spans, StackAudio::Kind::Imported, {}));
    CHECK(StackAudio::available(spans, StackAudio::Kind::Mixed, {}));
    CHECK(StackAudio::available(spans, StackAudio::Kind::Analogue, {}));

    // A source that has only EFM (no audio of any kind): the digital audio and the mix ask for it to be decoded, and wait for it
    {
        QFile tbc(directory.filePath("c.tbc"));
        CHECK(tbc.open(QIODevice::WriteOnly));
        writePcm(directory.filePath("c_decoded.raw"), 3000, perFrame * 12);
        QVector<StackAudioSpan> efmOnly(1);
        efmOnly[0].reference = directory.filePath("c.tbc");
        efmOnly[0].start = 0;
        efmOnly[0].frames = 10;
        const StackAudio::EfmOf efmOf = [&](const StackAudioSpan &span) { return span.reference.endsWith("c.tbc") ? directory.filePath("c.efm") : QString(); };
        CHECK(!StackAudio::available(efmOnly, StackAudio::Kind::Analogue, efmOf));
        CHECK(StackAudio::available(efmOnly, StackAudio::Kind::Digital, efmOf));
        CHECK(StackAudio::available(efmOnly, StackAudio::Kind::Mixed, efmOf));

        StackAudio digital;
        QEventLoop wait;
        qint32 asked = -1;
        QString askedFile;
        QObject::connect(&digital, &StackAudio::efmNeeded, &digital, [&](qint32 source, const QString &file) {
            asked = source;
            askedFile = file;
            // (decoded a moment later, as a program would)
            QTimer::singleShot(200, &digital, [&digital, source, &directory]() { digital.efmDecoded(source, directory.filePath("c_decoded.raw")); });
        });
        bool finished = false;
        bool good = false;
        QObject::connect(&digital, &StackAudio::finished, &wait, [&](bool ok, const QString &) { finished = true; good = ok; wait.quit(); });
        QTimer::singleShot(20000, &wait, &QEventLoop::quit);
        CHECK(digital.start(efmOnly, 10, fps, StackAudio::Kind::Digital, efmOf, error));
        wait.exec();
        CHECK(asked == 0 && askedFile.endsWith("c.efm"));
        CHECK(finished && good);
        QFile out(digital.cacheFile());
        CHECK(out.open(QIODevice::ReadOnly));
        const QByteArray bytes = out.readAll();
        CHECK(bytes.size() == 10 * perFrame * 4);
        CHECK(reinterpret_cast<const qint16 *>(bytes.constData())[(5 * perFrame + 100) * 2] == 3000);
    }

    // No audio in any source: no audio
    StackAudio none;
    QVector<StackAudioSpan> bare(1);
    bare[0].reference = directory.filePath("nothing.tbc");
    bare[0].frames = 10;
    CHECK(!none.start(bare, 10, fps, StackAudio::Kind::Mixed, {}, error));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testChoose();
    testFile();
    std::fprintf(stderr, "All tests passed\n");
    return 0;
}
