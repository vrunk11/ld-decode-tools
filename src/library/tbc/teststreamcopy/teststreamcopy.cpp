/******************************************************************************
 * teststreamcopy.cpp
 * ld-decode-tools TBC library - unit test of the copy of the other streams of a source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "ldx.h"
#include "streamcopy.h"
#include "targetvideo.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static QByteArray pattern(int size, int seed)
{
    QByteArray data(size, 0);
    for (int i = 0; i < size; i++) data[i] = static_cast<char>((i * 31 + seed * 17) & 0xff);
    return data;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    // A source: a video track, its analogue audio, its EFM and AC3, and the 24-bit audio of another capture
    const QString source = directory.filePath("source.ldx");
    {
        Ldx::Writer writer;
        Ldx::Track video;
        video.kind = Ldx::Kind::Tbc;
        video.name = "capture";
        video.parameters["system"] = "PAL";
        writer.addTrack(video);
        Ldx::Track analogue;
        analogue.kind = Ldx::Kind::Pcm;
        analogue.name = "capture audio";
        analogue.parameters["sampleRate"] = 44100;
        analogue.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(analogue);
        Ldx::Track efm;
        efm.kind = Ldx::Kind::Efm;
        efm.depth = 8;
        efm.name = "efm";
        efm.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(efm);
        Ldx::Track ac3;
        ac3.kind = Ldx::Kind::Ac3;
        ac3.depth = 8;
        ac3.name = "ac3";
        ac3.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(ac3);
        Ldx::Track wide;
        wide.kind = Ldx::Kind::Pcm;
        wide.depth = 24;
        wide.name = "wide audio";
        wide.parameters["sampleRate"] = 48000;
        wide.parameters["bits"] = 24;
        wide.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(wide);
        QString error;
        CHECK(writer.open(source, error));
        for (int i = 0; i < 20; i++) {
            const qint64 time = i * 20000000LL;           // (20 ms)
            CHECK(writer.writeBlock(video.number, time, pattern(100, i)));
            CHECK(writer.writeBlock(analogue.number, time, pattern(40, 100 + i)));
            CHECK(writer.writeBlock(efm.number, time, pattern(30, 200 + i)));
            if (i % 2 == 0) CHECK(writer.writeBlock(ac3.number, time, pattern(20, 300 + i)));
            CHECK(writer.writeBlock(wide.number, time, pattern(24, 400 + i)));
        }
        CHECK(writer.finish(error));
    }

    // What there is to copy: the analogue audio is listed but written from the fields
    const QVector<StreamCopy::Candidate> all = StreamCopy::candidates({ source, directory.filePath("plain.tbc") });
    CHECK(all.size() == 4);
    qint32 derived = 0;
    for (const StreamCopy::Candidate &candidate : all) {
        CHECK(candidate.source == 0);
        if (candidate.derived) derived++;
    }
    CHECK(derived == 1);

    // The default is the efm, the ac3 and the wide audio of the first source
    QVector<StreamCopy::Candidate> chosen = StreamCopy::defaultChoice(all);
    CHECK(chosen.size() == 3);

    // A choice as text
    QString error;
    QVector<StreamCopy::Candidate> parsed;
    CHECK(StreamCopy::parseChoice("1:efm", all, parsed, error) && parsed.size() == 1 && parsed[0].kind == "efm");
    CHECK(StreamCopy::parseChoice("1:ac3,1:efm,1:efm", all, parsed, error) && parsed.size() == 2);
    CHECK(StreamCopy::parseChoice("1:audio", all, parsed, error) && parsed.size() == 1 && parsed[0].depth == 24);
    CHECK(StreamCopy::parseChoice("none", all, parsed, error) && parsed.isEmpty());
    CHECK(StreamCopy::parseChoice("default", all, parsed, error) && parsed.size() == 3);
    CHECK(!StreamCopy::parseChoice("2:efm", all, parsed, error));
    CHECK(!StreamCopy::parseChoice("efm", all, parsed, error));
    CHECK(!StreamCopy::parseChoice("1:rf", all, parsed, error));
    CHECK(StreamCopy::historyDetail(chosen, { source }).contains("not realigned"));

    // Copied into an LDX output, interleaved with the fields, and finished
    const QString output = directory.filePath("output.ldx");
    {
        TargetVideo target;
        target.setLdxVideo(4, 2, 1000000.0, QString(), QJsonArray());
        StreamCopy copy;
        CHECK(copy.prepare(chosen, target, error));
        CHECK(target.open(output, PAL));
        const QVector<quint16> field(8, 7);
        for (int i = 0; i < 6; i++) {
            CHECK(copy.writeUpTo(target, target.ldxTimeNs()));
            CHECK(target.writeField(field, QByteArray("{}")));
        }
        CHECK(copy.finish(target));
        CHECK(target.close());
    }

    Ldx::Reader reader;
    CHECK(reader.open(output, error) && reader.buildIndex(error));
    qint32 efmTrack = 0, ac3Track = 0, wideTrack = 0;
    for (const Ldx::Track &track : reader.tracks()) {
        if (track.kind == Ldx::Kind::Efm) efmTrack = track.number;
        if (track.kind == Ldx::Kind::Ac3) ac3Track = track.number;
        if (track.kind == Ldx::Kind::Pcm) wideTrack = track.number;
    }
    CHECK(efmTrack && ac3Track && wideTrack);
    CHECK(reader.blockCount(efmTrack) == 20 && reader.blockCount(ac3Track) == 10 && reader.blockCount(wideTrack) == 20);
    Ldx::Block block;
    CHECK(reader.readBlock(efmTrack, 7, block) && block.data == pattern(30, 207) && block.timeNs == 7 * 20000000LL);
    CHECK(reader.readBlock(ac3Track, 3, block) && block.data == pattern(20, 306) && block.timeNs == 6 * 20000000LL);
    CHECK(reader.readBlock(wideTrack, 19, block) && block.data == pattern(24, 419));

    // The alignment of a stream with the video: the frame of the source that each frame of the output is
    {
        const QJsonObject same = StreamCopy::alignmentOf({ 1, 2, 3, 4 });
        CHECK(same.value("segments").toArray().size() == 1 && StreamCopy::alignmentSummary(same) == "not shifted from the video");
        const QJsonObject shifted = StreamCopy::alignmentOf({ 0, 0, 1, 2, 3, 0 });          // (begins at the third frame of the output, ends before the last)
        CHECK(shifted.value("segments").toArray().size() == 3);
        CHECK(shifted.value("segments").toArray().at(1).toArray().at(0).toInt() == 3 && shifted.value("segments").toArray().at(1).toArray().at(1).toInt() == 1);
        CHECK(StreamCopy::alignmentSummary(shifted).contains("shifted from the video by -2 frames"));
        const QJsonObject skip = StreamCopy::alignmentOf({ 1, 2, 3, 7, 8, 0, 0, 4 });       // (a skip forward, frames padded, a frame seen again)
        CHECK(skip.value("segments").toArray().size() == 4);
        CHECK(StreamCopy::alignmentSummary(skip).contains("in 3 pieces"));
        CHECK(StreamCopy::alignmentSummary(StreamCopy::alignmentOf({ 0, 0 })) == "no frame of the output has this source");
        CHECK(StreamCopy::alignmentOf({}).value("segments").toArray().isEmpty());
    }

    std::printf("teststreamcopy: ok\n");
    return 0;
}
