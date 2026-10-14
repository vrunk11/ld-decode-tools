/******************************************************************************
 * testldx.cpp
 * ld-decode-tools LDX library - unit tests for the LDX container
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#include "ldx.h"

// assert() is compiled out of release builds, so check explicitly
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ << ": check failed: " #condition "\n"; \
            std::exit(1); \
        } \
    } while (0)

static QByteArray pattern(int size, int seed)
{
    QByteArray data(size, 0);
    for (int i = 0; i < size; i++) data[i] = static_cast<char>((i * 31 + seed * 17 + (i >> 8)) & 0xff);
    return data;
}

// Two video tracks of the same kind, audio, and metadata of different sizes: what was written is read back
static void testRoundTrip(const QString &path)
{
    std::cerr << "Testing a round trip\n";

    Ldx::Writer writer;
    Ldx::Track first;
    first.kind = Ldx::Kind::Tbc;
    first.name = "capture 1";
    first.parameters["system"] = "NTSC";
    writer.addTrack(first);
    Ldx::Track second = first;
    second.name = "capture 2";
    second.uid = 0;
    writer.addTrack(second);
    Ldx::Track audio;
    audio.kind = Ldx::Kind::Pcm;
    audio.parameters["sampleRate"] = 44100;
    writer.addTrack(audio);
    Ldx::Track metadata;
    metadata.kind = Ldx::Kind::Metadata;
    metadata.depth = 0;
    writer.addTrack(metadata);

    CHECK(first.number == 1 && second.number == 2 && audio.number == 3 && metadata.number == 4);
    CHECK(first.uid != second.uid);

    QJsonObject entry;
    entry["tool"] = "testldx";
    writer.setHistory(QJsonArray { entry });

    QString error;
    CHECK(writer.open(path, error));

    // Sizes from a few bytes to larger than a field and a megabyte
    const int sizes[] = { 1, 100, 5880, 478660, 1483317 };
    QVector<Ldx::Block> written;
    auto put = [&](quint32 track, qint64 timeNs, int size, int seed) {
        Ldx::Block block;
        block.track = track;
        block.timeNs = timeNs;
        block.data = pattern(size, seed);
        CHECK(writer.writeBlock(block.track, block.timeNs, block.data));
        written.append(block);
    };
    put(4, 0, sizes[4], 1);
    for (int i = 0; i < 12; i++) {
        const qint64 time = i * 16683000LL;
        put(1, time, 478660, i);
        put(2, time, 478660, i + 100);
        if (i % 2 == 0) put(3, time, 5880, i + 200);
    }
    put(4, 12 * 16683000LL, sizes[0], 2);
    put(4, 12 * 16683000LL + 1000, sizes[1], 3);
    put(4, 12 * 16683000LL + 2000, sizes[2], 4);
    CHECK(writer.finish(error));

    Ldx::Reader reader;
    CHECK(reader.open(path, error));
    CHECK(reader.profile() == Ldx::ProfileVersion);
    CHECK(reader.history().size() == 1);
    CHECK(reader.history().at(0).toObject()["tool"].toString() == "testldx");

    const QVector<Ldx::Track> tracks = reader.tracks();
    CHECK(tracks.size() == 4);
    CHECK(tracks[0].kind == Ldx::Kind::Tbc && tracks[0].name == "capture 1" && tracks[0].number == 1);
    CHECK(tracks[1].kind == Ldx::Kind::Tbc && tracks[1].name == "capture 2" && tracks[1].number == 2);
    CHECK(tracks[0].parameters["system"].toString() == "NTSC");
    CHECK(tracks[2].kind == Ldx::Kind::Pcm && tracks[2].parameters["sampleRate"].toInt() == 44100);
    CHECK(tracks[3].kind == Ldx::Kind::Metadata && tracks[3].depth == 0);

    // Every block comes back with its data and its time, in the order it was written
    Ldx::Block block;
    int index = 0;
    while (reader.nextBlock(block)) {
        CHECK(index < written.size());
        CHECK(block.track == written[index].track);
        if (block.timeNs != written[index].timeNs) std::cerr << "  block " << index << " track " << block.track << ": time " << block.timeNs << " read, " << written[index].timeNs << " written\n";
        CHECK(block.timeNs == written[index].timeNs);
        CHECK(block.data == written[index].data);
        index++;
    }
    CHECK(index == written.size());

    // Random access: the Nth block of a track, from the cues, in any order
    Ldx::Reader random;
    CHECK(random.open(path, error));
    CHECK(random.buildIndex(error));
    CHECK(random.blockCount(1) == 12 && random.blockCount(2) == 12 && random.blockCount(3) == 6 && random.blockCount(4) == 4);
    CHECK(random.blockCount(99) == 0);
    for (const int wanted: { 7, 0, 11, 3, 3 }) {
        Ldx::Block found;
        CHECK(random.readBlock(2, wanted, found));
        CHECK(found.track == 2 && found.timeNs == wanted * 16683000LL && found.data == pattern(478660, wanted + 100));
        CHECK(random.blockTimeNs(2, wanted) == found.timeNs);
    }
    Ldx::Block audioBlock;
    CHECK(random.readBlock(3, 5, audioBlock) && audioBlock.data == pattern(5880, 10 + 200));
    CHECK(!random.readBlock(3, 6, audioBlock));
    Ldx::Block metadataBlock;
    CHECK(random.readBlock(4, 0, metadataBlock) && metadataBlock.data.size() == 1483317);
}

// One block of each size in a file of its own, on a track of samples and on a track of metadata
static void testBlockSizes(const QString &directory)
{
    std::cerr << "Testing block sizes\n";
    for (const Ldx::Kind kind: { Ldx::Kind::Pcm, Ldx::Kind::Metadata }) {
        for (const int size: { 1, 100, 5880, 32767, 32768, 65535, 65536, 478660, 1483317 }) {
            Ldx::Writer writer;
            Ldx::Track track;
            track.kind = kind;
            writer.addTrack(track);
            QString error;
            const QString path = directory + "/size.ldx";
            CHECK(writer.open(path, error));
            const bool ok = writer.writeBlock(1, 0, pattern(size, 1));
            if (!ok) std::cerr << "  " << size << " bytes on " << Ldx::kindName(kind).toStdString() << ": " << writer.lastError().toStdString() << "\n";
            CHECK(ok);
            CHECK(writer.finish(error));
        }
    }
}

// The ids of the tracks are kept when a file is written again without one of them, and are not given again
static void testIdsAndAttachments(const QString &path)
{
    std::cerr << "Testing the ids of the tracks and the attachments\n";

    Ldx::Writer writer;
    Ldx::Track first;
    first.kind = Ldx::Kind::Tbc;
    first.number = 1;
    writer.addTrack(first);
    Ldx::Track third;
    third.kind = Ldx::Kind::Tbc;
    third.number = 3;           // 2 was removed
    writer.addTrack(third);
    Ldx::Track next;
    next.kind = Ldx::Kind::Pcm;
    writer.addTrack(next);
    CHECK(next.number == 4);
    writer.setNextTrackId(7);   // 5 and 6 were removed too
    writer.addAttachment("session.stack", "application/json", "{\"version\":1}");
    writer.addAttachment("notes.txt", "text/plain", QByteArray(100000, 'x'));

    QString error;
    CHECK(writer.open(path, error));
    CHECK(writer.writeBlock(3, 0, pattern(100, 1)));
    CHECK(!writer.writeBlock(2, 0, pattern(100, 1)));      // there is no track 2
    CHECK(writer.finish(error));

    Ldx::Reader reader;
    CHECK(reader.open(path, error));
    const QVector<Ldx::Track> tracks = reader.tracks();
    CHECK(tracks.size() == 3);
    CHECK(tracks[0].number == 1 && tracks[1].number == 3 && tracks[2].number == 4);
    CHECK(reader.nextTrackId() == 7);
    const auto attachments = reader.attachments();
    CHECK(attachments.size() == 2);
    CHECK(attachments[0].name == "session.stack" && attachments[0].mediaType == "application/json");
    CHECK(attachments[0].data == "{\"version\":1}");
    CHECK(attachments[1].name == "notes.txt" && attachments[1].data == QByteArray(100000, 'x'));
    Ldx::Block block;
    CHECK(reader.nextBlock(block) && block.track == 3 && block.data == pattern(100, 1));
    CHECK(!reader.nextBlock(block));
}

static void testNotAnLdxFile(const QString &path)
{
    std::cerr << "Testing a file that is not an LDX file\n";
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write("this is not a matroska file at all");
    file.close();

    Ldx::Reader reader;
    QString error;
    CHECK(!reader.open(path, error));
    CHECK(!error.isEmpty());
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    testBlockSizes(directory.path());
    testRoundTrip(directory.filePath("test.ldx"));
    testIdsAndAttachments(directory.filePath("ids.ldx"));
    testNotAnLdxFile(directory.filePath("garbage.ldx"));

    std::cerr << "All tests passed\n";
    return 0;
}
