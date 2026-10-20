/******************************************************************************
 * testaudio.cpp
 * ld-decode-tools TBC library - unit tests for the audio reader, peaks and mix
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "audiopeaks.h"
#include "sourceaudio.h"
#include "audioreader.h"
#include "audioring.h"
#include "ldxcapture.h"
#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (0)

static QVector<qint16> ramp(qint64 pairs, int seed)
{
    QVector<qint16> samples(static_cast<qint32>(pairs * 2));
    for (qint64 i = 0; i < pairs; i++) {
        samples[static_cast<qint32>(2 * i)] = static_cast<qint16>((i * 7 + seed) % 20000 - 10000);
        samples[static_cast<qint32>(2 * i + 1)] = static_cast<qint16>(10000 - (i * 5 + seed) % 20000);
    }
    return samples;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    // The mix: both channels, one on both sides, none, and the volume
    {
        qint16 in[4] = { 1000, -2000, 3000, -4000 };
        qint16 out[4];
        AudioMix mix;
        mixStereo(in, out, 2, mix);
        CHECK(out[0] == 1000 && out[1] == -2000 && out[2] == 3000 && out[3] == -4000);
        mix.right = false;
        mixStereo(in, out, 2, mix);
        CHECK(out[0] == 1000 && out[1] == 1000 && out[2] == 3000 && out[3] == 3000);
        mix.left = false;
        mix.right = true;
        mixStereo(in, out, 2, mix);
        CHECK(out[0] == -2000 && out[1] == -2000 && out[2] == -4000 && out[3] == -4000);
        mix.right = false;
        mixStereo(in, out, 2, mix);
        CHECK(out[0] == 0 && out[3] == 0);
        mix.left = mix.right = true;
        mix.volume = 0.5;
        mixStereo(in, out, 2, mix);
        CHECK(out[0] == 500 && out[1] == -1000);
        mixStereo(in, in, 2, mix);         // (in place)
        CHECK(in[2] == 1500);
    }

    // The audio of a capture that has less than was asked for: what is there is read, and the rest is silence (it used to stop the program)
    {
        const QVector<qint16> audio = ramp(1000, 11);
        const QString pcm = directory.path() + "/short.pcm";
        QFile file(pcm);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(reinterpret_cast<const char *>(audio.constData()), static_cast<qint64>(audio.size()) * 2);
        file.close();
        QFile tbc(directory.path() + "/short.tbc");
        CHECK(tbc.open(QIODevice::WriteOnly));
        tbc.close();
        SourceAudio source;
        CHECK(source.open(QFileInfo(directory.path() + "/short.tbc")));
        CHECK(source.getNumberOfSamples() == 1000);
        bool shortRead = true;
        SourceAudio::Data inside = source.getAudioDataPadded(100, 50, &shortRead);
        CHECK(!shortRead && inside.size() == 100 && inside[0] == audio[200] && inside[99] == audio[299]);
        SourceAudio::Data across = source.getAudioDataPadded(990, 30, &shortRead);
        CHECK(shortRead && across.size() == 60);
        CHECK(across[0] == audio[1980] && across[19] == audio[1999] && across[20] == 0 && across[59] == 0);
        SourceAudio::Data beyond = source.getAudioDataPadded(5000, 10, &shortRead);
        CHECK(shortRead && beyond.size() == 20);
        for (const qint16 value: beyond) CHECK(value == 0);
        source.close();
    }

    // The peaks: bins, ranges, adding in pieces gives the same as all at once
    {
        const QVector<qint16> samples = ramp(5000, 3);
        AudioPeaks whole(256), pieces(256);
        whole.add(samples.constData(), 5000);
        pieces.add(samples.constData(), 1234);
        pieces.add(samples.constData() + 2 * 1234, 5000 - 1234);
        CHECK(whole.samples() == 5000 && whole.bins() == 20);       // 19 whole bins and a part of one
        CHECK(pieces.bins() == whole.bins());
        for (qint64 i = 0; i < whole.bins(); i++) {
            const AudioPeaks::Peak a = whole.bin(i), b = pieces.bin(i);
            CHECK(a.minLeft == b.minLeft && a.maxLeft == b.maxLeft && a.minRight == b.minRight && a.maxRight == b.maxRight);
        }
        qint16 low = 32767, high = -32768;
        for (qint32 i = 0; i < 5000; i++) { low = std::min(low, samples[2 * i]); high = std::max(high, samples[2 * i]); }
        const AudioPeaks::Peak all = whole.range(0, 5000);
        CHECK(all.minLeft == low && all.maxLeft == high);
    }

    // Audio held in memory, read back, and the peaks of a reader
    {
        AudioReader reader;
        const QVector<qint16> samples = ramp(3000, 11);
        reader.openMemory(samples, 44100);
        CHECK(reader.samples() == 3000 && reader.sampleRate() == 44100);
        CHECK(reader.read(10, 5) == samples.mid(20, 10));
        CHECK(reader.read(2998, 10).size() == 4);
        CHECK(reader.read(3000, 10).isEmpty());
        AudioPeaks peaks(512);
        CHECK(peaks.addAll(reader));
        CHECK(peaks.samples() == 3000 && peaks.bins() == 6);
        std::atomic_bool cancelled(true);
        AudioPeaks none(512);
        CHECK(!none.addAll(reader, &cancelled));
    }

    // The .pcm beside a .tbc
    {
        const QVector<qint16> samples = ramp(4000, 5);
        const QString tbc = directory.filePath("capture.tbc");
        QFile(tbc).open(QIODevice::WriteOnly);
        QFile pcm(directory.filePath("capture.pcm"));
        CHECK(pcm.open(QIODevice::WriteOnly));
        pcm.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
        pcm.close();
        const QVector<AudioTrackInfo> tracks = AudioReader::tracksOf(tbc);
        CHECK(tracks.size() == 1 && tracks[0].source == "analogue");
        AudioReader reader;
        QString error;
        CHECK(reader.open(tracks[0], error));
        CHECK(reader.samples() == 4000);
        CHECK(reader.read(100, 50) == samples.mid(200, 100));
        CHECK(AudioReader::tracksOf(directory.filePath("other.tbc")).isEmpty());
    }

#ifdef LDDECODE_WITH_LDX
    // The pcm tracks of an LDX file: analogue 16 bits, decoded from EFM, and 24 bits
    {
        const QVector<qint16> analogue = ramp(3000, 1), efm = ramp(2000, 9);
        Ldx::Writer writer;
        Ldx::Track video;
        video.kind = Ldx::Kind::Tbc;
        writer.addTrack(video);
        Ldx::Track metadata;
        metadata.kind = Ldx::Kind::Metadata;
        metadata.depth = 0;
        metadata.parameters["track"] = 1;
        writer.addTrack(metadata);
        Ldx::Track first;
        first.kind = Ldx::Kind::Pcm;
        first.name = "analogue";
        first.parameters["sampleRate"] = 44100;
        first.parameters["track"] = 1;
        writer.addTrack(first);
        Ldx::Track second;
        second.kind = Ldx::Kind::Pcm;
        second.name = "from efm";
        second.parameters["sampleRate"] = 44100;
        second.parameters["source"] = "efm";
        second.parameters["track"] = 1;
        writer.addTrack(second);
        Ldx::Track wide;
        wide.kind = Ldx::Kind::Pcm;
        wide.depth = 24;
        wide.name = "wide";
        wide.parameters["sampleRate"] = 48000;
        wide.parameters["track"] = 1;
        writer.addTrack(wide);

        const QString file = directory.filePath("audio.ldx");
        QString error;
        CHECK(writer.open(file, error));
        CHECK(writer.writeBlock(1, 0, QByteArray(10, 'v')));
        CHECK(writer.writeBlock(2, 0, QByteArray("{}")));
        CHECK(writer.writeBlock(3, 0, QByteArray(reinterpret_cast<const char *>(analogue.constData()), analogue.size() * 2)));
        CHECK(writer.writeBlock(4, 0, QByteArray(reinterpret_cast<const char *>(efm.constData()), efm.size() * 2)));
        QByteArray wideBytes;
        for (int i = 0; i < 500; i++) {
            const qint32 left = i * 1000 - 200000, right = 300000 - i * 1200;
            for (const qint32 value: { left, right }) {
                wideBytes.append(static_cast<char>(value & 0xFF));
                wideBytes.append(static_cast<char>((value >> 8) & 0xFF));
                wideBytes.append(static_cast<char>((value >> 16) & 0xFF));
            }
        }
        CHECK(writer.writeBlock(5, 0, wideBytes));
        CHECK(writer.finish(error));

        const QVector<AudioTrackInfo> tracks = AudioReader::tracksOf(file);
        CHECK(tracks.size() == 3);
        CHECK(tracks[0].source == "analogue" && tracks[0].trackId == 3 && tracks[0].bits == 16);
        CHECK(tracks[1].source == "efm" && tracks[1].trackId == 4);
        CHECK(tracks[2].bits == 24 && tracks[2].trackId == 5 && tracks[2].sampleRate == 48000);
        CHECK(tracks[1].where == file && !tracks[1].label.isEmpty());

        AudioReader a, b, c;
        CHECK(a.open(tracks[0], error) && a.samples() == 3000);
        CHECK(a.read(100, 20) == analogue.mid(200, 40));
        CHECK(b.open(tracks[1], error) && b.samples() == 2000);
        CHECK(b.read(0, 10) == efm.mid(0, 20));
        CHECK(c.open(tracks[2], error) && c.samples() == 500 && c.sampleRate() == 48000);
        const QVector<qint16> top = c.read(2, 1);                       // the top 16 bits of the 24
        CHECK(top.size() == 2 && top[0] == static_cast<qint16>((2 * 1000 - 200000) >> 8) && top[1] == static_cast<qint16>((300000 - 2 * 1200) >> 8));
        // The analogue audio of the capture is what openAudio with no track gives, and not the one decoded from EFM
        LdxCapture capture;
        CHECK(capture.openAudio(file, error) && capture.audioSamples() == 3000);
    }
#endif


    // A ring of arriving audio: it keeps the last part, the samples keep their numbers, a piece of a pair waits for the next
    {
        AudioRing ring(0.01, 1000.0);                   // (1024 pairs at the least: that is what is kept)
        QByteArray all;
        for (qint32 i = 0; i < 3000; i++) {
            const qint16 left = static_cast<qint16>(i), right = static_cast<qint16>(-i);
            all.append(reinterpret_cast<const char *>(&left), 2);
            all.append(reinterpret_cast<const char *>(&right), 2);
        }
        // in pieces that cut a pair in two
        ring.append(all.left(1001));
        CHECK(ring.endSample() == 250 && ring.firstSample() == 0);
        ring.append(all.mid(1001, 5000));
        ring.append(all.mid(6001));
        CHECK(ring.endSample() == 3000);
        CHECK(ring.firstSample() == 3000 - 1024);
        qint64 firstGiven = -1;
        QVector<qint16> got = ring.read(2900, 50, &firstGiven);
        CHECK(firstGiven == 2900 && got.size() == 100 && got[0] == 2900 && got[1] == -2900 && got[98] == 2949 && got[99] == -2949);
        // what was let go is not there: the first held is given
        got = ring.read(0, 3000, &firstGiven);
        CHECK(firstGiven == 3000 - 1024 && got.size() == 2 * 1024 && got[0] == static_cast<qint16>(3000 - 1024) && got[2 * 1023] == 2999);
        CHECK(ring.read(3000, 10).isEmpty());
        // a piece longer than the ring
        AudioRing small(0.001, 1000.0);
        small.append(all);
        CHECK(small.endSample() == 3000 && small.firstSample() == 3000 - 1024);
        got = small.read(2990, 20);
        CHECK(got.size() == 20 && got[0] == 2990);
        // read as audio
        AudioReader reader;
        auto shared = std::make_shared<AudioRing>(0.01, 1000.0);
        reader.openRing(shared);
        shared->append(all);
        CHECK(reader.samples() == 3000 && reader.firstSample() == 3000 - 1024 && reader.sampleRate() == 1000.0);
        got = reader.read(2995, 10);
        CHECK(got.size() == 10 && got[0] == 2995);
    }
    std::fprintf(stderr, "testaudio: all tests passed\n");
    return 0;
}
