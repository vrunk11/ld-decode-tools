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
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>

#include "ldx.h"
#include "ldxcodec.h"

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

// A file that is already written is added to: blocks go at the end, what was there is kept, and the cues are written again
static void testAppend(const QString &path)
{
    std::cerr << "Testing adding to a file\n";

    QVector<Ldx::Block> written;
    {
        Ldx::Writer writer;
        Ldx::Track video;
        video.kind = Ldx::Kind::Tbc;
        writer.addTrack(video);
        Ldx::Track metadata;
        metadata.kind = Ldx::Kind::Metadata;
        metadata.depth = 0;
        writer.addTrack(metadata);
        writer.addAttachment("session.stack", "application/json", "{}");
        QJsonObject entry;
        entry["tool"] = "first";
        writer.setHistory(QJsonArray { entry });
        QString error;
        CHECK(writer.open(path, error));
        auto put = [&](quint32 track, qint64 timeNs, int size, int seed) {
            Ldx::Block block;
            block.track = track;
            block.timeNs = timeNs;
            block.data = pattern(size, seed);
            CHECK(writer.writeBlock(block.track, block.timeNs, block.data));
            written.append(block);
        };
        put(2, 0, 1000, 1);
        for (int i = 0; i < 8; i++) put(1, i * 16683000LL, 100000, i);
        CHECK(writer.finish(error));
    }

    // Add a newer block of the metadata, and a field
    {
        Ldx::Writer writer;
        QString error;
        CHECK(writer.openAppend(path, error));
        QJsonObject entry;
        entry["tool"] = "second";
        writer.addHistory(entry);
        Ldx::Block block;
        block.track = 2;
        block.timeNs = 8 * 16683000LL;
        block.data = pattern(2000, 77);
        CHECK(writer.writeBlock(block.track, block.timeNs, block.data));
        written.append(block);
        Ldx::Block field;
        field.track = 1;
        field.timeNs = 8 * 16683000LL + 1000;
        field.data = pattern(100000, 78);
        CHECK(writer.writeBlock(field.track, field.timeNs, field.data));
        written.append(field);
        CHECK(writer.finish(error));
    }

    // In the order of the file, everything is there
    Ldx::Reader reader;
    QString error;
    CHECK(reader.open(path, error));
    Ldx::Block block;
    int index = 0;
    while (reader.nextBlock(block)) {
        CHECK(index < written.size());
        CHECK(block.track == written[index].track && block.data == written[index].data);
        index++;
    }
    CHECK(index == written.size());
    // The history is that of the last tags in the file, and the attachment is still there
    CHECK(reader.history().size() == 2 && reader.history().at(1).toObject()["tool"].toString() == "second");
    QByteArray session;
    CHECK(reader.attachment("session.stack", session) && session == "{}");

    // By place in the track: the new blocks follow the old ones
    Ldx::Reader random;
    CHECK(random.open(path, error));
    CHECK(random.buildIndex(error));
    CHECK(random.blockCount(1) == 9 && random.blockCount(2) == 2);
    Ldx::Block found;
    CHECK(random.readBlock(2, 0, found) && found.data == pattern(1000, 1));
    CHECK(random.readBlock(2, 1, found) && found.data == pattern(2000, 77));
    CHECK(random.readBlock(1, 3, found) && found.data == pattern(100000, 3));
    CHECK(random.readBlock(1, 8, found) && found.data == pattern(100000, 78));
    CHECK(random.lastTimeNs() == 8 * 16683000LL + 1000);

    // Added to again, the file is still whole
    Ldx::Writer again;
    CHECK(again.openAppend(path, error));
    QJsonObject third;
    third["tool"] = "third";
    again.addHistory(third);
    CHECK(again.writeBlock(2, 9 * 16683000LL, pattern(10, 5)));
    CHECK(again.finish(error));
    Ldx::Reader last;
    CHECK(last.open(path, error));
    CHECK(last.buildIndex(error));
    CHECK(last.blockCount(2) == 3);
    CHECK(last.history().size() == 3 && last.history().at(2).toObject()["tool"].toString() == "third");
    CHECK(last.readBlock(2, 2, found) && found.data == pattern(10, 5));
    CHECK(last.readBlock(1, 0, found) && found.data == pattern(100000, 0));
}

// A wave with noise on it, as a field of samples: unsigned around the middle of the range, as TBC samples are
static QByteArray wave(int samples, int seed)
{
    QByteArray data(samples * 2, 0);
    quint16 *values = reinterpret_cast<quint16 *>(data.data());
    quint32 state = 12345u + static_cast<quint32>(seed);
    for (int i = 0; i < samples; i++) {
        state = state * 1664525u + 1013904223u;
        const int noise = static_cast<int>((state >> 24) & 0x1f) - 16;
        const int signal = 32768 + static_cast<int>(20000.0 * ((i / 910) % 2 ? 1.0 : -1.0) * (((i % 910) < 700) ? 1.0 : 0.2));
        values[i] = static_cast<quint16>(signal + noise);
    }
    return data;
}

static void testFlac(const QString &directory)
{
    if (!Ldx::flacAvailable()) {
        std::cerr << "Skipping the FLAC tests: this build has no ffmpeg\n";
        return;
    }
    std::cerr << "Testing FLAC\n";

    QString error;
    const QByteArray codecPrivate = Ldx::flacCodecPrivate(1, error);
    CHECK(codecPrivate.size() == 4 + 4 + 34 && codecPrivate.left(4) == "fLaC");

    // Blocks of any length up to the most, unsigned and signed, come back as they were
    Ldx::FlacDecoder decoder;
    CHECK(decoder.open(codecPrivate, error));
    for (const bool unsignedSamples: { true, false }) {
        for (const int samples: { 1, 17, 1470, 32768, 32769, 239330, 262144 }) {
            const QByteArray original = wave(samples, samples);
            std::vector<QByteArray> frames;
            CHECK(Ldx::flacEncode(original, 1, unsignedSamples, frames, error));
            CHECK(!frames.empty() && static_cast<int>(frames.size()) <= Ldx::FlacMaxFrames);
            QByteArray decoded;
            if (!decoder.decode(frames, unsignedSamples, decoded, error)) std::cerr << "  decode failed for " << samples << " samples: " << error.toStdString() << " (" << frames.size() << " frames, first " << frames[0].size() << " bytes)\n";
            CHECK(decoder.decode(frames, unsignedSamples, decoded, error));
            CHECK(decoded == original);
        }
    }

    // A field of this kind is smaller as FLAC (unsigned samples are made signed, so the wave does not jump)
    std::vector<QByteArray> framesUnsigned, framesRaw;
    const QByteArray field = wave(239330, 1);
    CHECK(Ldx::flacEncode(field, 1, true, framesUnsigned, error));
    CHECK(Ldx::flacEncode(field, 1, false, framesRaw, error));
    qint64 compressed = 0, uncompressed = 0;
    for (const QByteArray &frame: framesUnsigned) compressed += frame.size();
    for (const QByteArray &frame: framesRaw) uncompressed += frame.size();
    CHECK(compressed < field.size() * 8 / 10);
    CHECK(compressed < uncompressed);

    // Stereo (the analogue audio): the two channels are coded together
    Ldx::FlacDecoder stereoDecoder;
    CHECK(stereoDecoder.open(Ldx::flacCodecPrivate(2, error), error));
    for (const int samples: { 1, 1470, 32769, 100000 }) {
        const QByteArray original = wave(samples * 2, samples + 7);
        std::vector<QByteArray> frames;
        CHECK(Ldx::flacEncode(original, 2, false, frames, error));
        QByteArray decoded;
        CHECK(stereoDecoder.decode(frames, false, decoded, error));
        CHECK(decoded == original);
    }

    // Too long a block is refused
    std::vector<QByteArray> none;
    CHECK(!Ldx::flacEncode(QByteArray((262144 + 1) * 2, 0), 1, false, none, error));
    Q_UNUSED(directory);
}

// A track whose blocks are FLAC: the blocks written are the blocks read, in order and by place, and the file is smaller
static void testFlacTracks(const QString &path)
{
    if (!Ldx::flacAvailable()) return;
    std::cerr << "Testing FLAC tracks\n";

    Ldx::Writer writer;
    Ldx::Track video;
    video.kind = Ldx::Kind::Tbc;
    video.codec = Ldx::Codec::Flac;
    video.name = "flac capture";
    video.parameters["system"] = "NTSC";
    writer.addTrack(video);
    Ldx::Track plain;
    plain.kind = Ldx::Kind::Tbc;      // a raw one beside it
    writer.addTrack(plain);
    Ldx::Track audio;
    audio.kind = Ldx::Kind::Pcm;
    audio.codec = Ldx::Codec::Flac;
    writer.addTrack(audio);
    Ldx::Track metadata;
    metadata.kind = Ldx::Kind::Metadata;
    metadata.depth = 0;
    writer.addTrack(metadata);

    QString error;
    CHECK(writer.open(path, error));
    QVector<Ldx::Block> written;
    qint64 rawBytes = 0;
    auto put = [&](quint32 track, qint64 timeNs, const QByteArray &data) {
        Ldx::Block block;
        block.track = track;
        block.timeNs = timeNs;
        block.data = data;
        CHECK(writer.writeBlock(block.track, block.timeNs, block.data));
        written.append(block);
        rawBytes += data.size();
    };
    put(4, 0, QByteArray("metadata"));
    for (int i = 0; i < 6; i++) {
        const qint64 time = i * 16683000LL;
        put(1, time, wave(239330, i));
        put(2, time, wave(239330, i));
        put(3, time, wave(1470 * 2, i + 50));
    }
    CHECK(writer.finish(error));
    CHECK(QFileInfo(path).size() < rawBytes * 8 / 10);          // (a raw track is a third of what was written)

    Ldx::Reader reader;
    CHECK(reader.open(path, error));
    const QVector<Ldx::Track> tracks = reader.tracks();
    CHECK(tracks.size() == 4);
    CHECK(tracks[0].codec == Ldx::Codec::Flac && tracks[0].kind == Ldx::Kind::Tbc && tracks[0].name == "flac capture");
    CHECK(tracks[0].parameters["system"].toString() == "NTSC" && tracks[0].depth == 16);
    CHECK(tracks[1].codec == Ldx::Codec::Raw && tracks[2].codec == Ldx::Codec::Flac && tracks[2].kind == Ldx::Kind::Pcm);

    Ldx::Block block;
    int index = 0;
    while (reader.nextBlock(block)) {
        CHECK(index < written.size());
        CHECK(block.track == written[index].track && block.data == written[index].data);
        index++;
    }
    CHECK(index == written.size());

    Ldx::Reader random;
    CHECK(random.open(path, error));
    CHECK(random.buildIndex(error));
    CHECK(random.blockCount(1) == 6 && random.blockCount(3) == 6);
    for (const int wanted: { 4, 0, 5, 2 }) {
        Ldx::Block found;
        CHECK(random.readBlock(1, wanted, found) && found.data == wave(239330, wanted));
        CHECK(random.readBlock(2, wanted, found) && found.data == wave(239330, wanted));
        CHECK(random.readBlock(3, wanted, found) && found.data == wave(1470 * 2, wanted + 50));
    }

    // Added to: a block of a FLAC track at the end
    Ldx::Writer append;
    CHECK(append.openAppend(path, error));
    CHECK(append.writeBlock(1, 6 * 16683000LL, wave(239330, 77)));
    CHECK(append.finish(error));
    Ldx::Reader after;
    CHECK(after.open(path, error));
    CHECK(after.buildIndex(error));
    Ldx::Block found;
    CHECK(after.blockCount(1) == 7 && after.readBlock(1, 6, found) && found.data == wave(239330, 77));
    CHECK(after.readBlock(1, 1, found) && found.data == wave(239330, 1));
}

// The samples of a TBC are unsigned: they are made signed for FLAC, so the same wave is smaller as a TBC than as RF samples
static void testUnsignedSamples(const QString &directory)
{
    if (!Ldx::flacAvailable()) return;
    std::cerr << "Testing the unsigned samples of a TBC\n";
    qint64 sizes[2] = { 0, 0 };
    int which = 0;
    for (const Ldx::Kind kind: { Ldx::Kind::Tbc, Ldx::Kind::Rf }) {
        const QString path = directory + (which ? "/rf.ldx" : "/tbc.ldx");
        Ldx::Writer writer;
        Ldx::Track track;
        track.kind = kind;
        track.codec = Ldx::Codec::Flac;
        writer.addTrack(track);
        QString error;
        CHECK(writer.open(path, error));
        CHECK(writer.writeBlock(1, 0, wave(239330, 3)));
        CHECK(writer.finish(error));
        sizes[which++] = QFileInfo(path).size();

        Ldx::Reader reader;
        CHECK(reader.open(path, error));
        Ldx::Block block;
        CHECK(reader.nextBlock(block) && block.data == wave(239330, 3));
    }
    CHECK(sizes[0] < sizes[1]);
}

// A frame of CVBS samples: 10 bits in words, a line of a ramp with noise, which repeats
static QByteArray cvbsFrame(qint64 samples, int width, int seed)
{
    QByteArray data(static_cast<int>(samples * 2), 0);
    quint16 *values = reinterpret_cast<quint16 *>(data.data());
    quint32 state = 777u + static_cast<quint32>(seed);
    for (qint64 i = 0; i < samples; i++) {
        state = state * 1664525u + 1013904223u;
        const int noise = static_cast<int>((state >> 24) & 0x7) - 4;
        const int signal = 200 + static_cast<int>((i % width) * 600 / width);
        values[i] = static_cast<quint16>(qBound(0, signal + noise, 1023));
    }
    return data;
}

static void testFfv1()
{
    if (!Ldx::ffv1Available()) {
        std::cerr << "Skipping the FFV1 tests: this build has no ffmpeg\n";
        return;
    }
    std::cerr << "Testing FFV1\n";

    // A frame that fills its image (NTSC), and one that does not (PAL: 625 lines and 4 samples)
    struct Shape { int width; qint64 samples; };
    for (const Shape shape: { Shape { 910, 477750 }, Shape { 1135, 709379 }, Shape { 909, 477225 } }) {
        const int height = Ldx::ffv1Height(shape.samples, shape.width);
        CHECK(height == (shape.samples == 709379 ? 626 : 525));

        const QByteArray frame = cvbsFrame(shape.samples, shape.width, shape.width);
        const QByteArray image = Ldx::ffv1Pad(frame, shape.width, height, 252);
        CHECK(image.size() == shape.width * height * 2);
        CHECK(Ldx::ffv1Unpad(image, shape.samples) == frame);

        QString error;
        Ldx::Ffv1Encoder encoder;
        CHECK(encoder.open(shape.width, height, error));
        CHECK(!encoder.extradata().isEmpty());
        Ldx::Ffv1Decoder decoder;
        CHECK(decoder.open(shape.width, height, encoder.extradata(), error));

        // Several in a row, each alone
        for (int n = 0; n < 3; n++) {
            const QByteArray sent = Ldx::ffv1Pad(cvbsFrame(shape.samples, shape.width, n), shape.width, height, 252);
            QByteArray packet, received;
            CHECK(encoder.encode(sent, packet, error));
            CHECK(packet.size() < sent.size() / 2);
            if (!decoder.decode(packet, received, error)) std::cerr << "  " << error.toStdString() << "\n";
            CHECK(decoder.decode(packet, received, error));
            CHECK(received == sent);
        }
    }
}

// The frames of a CVBS capture as a track: raw and FFV1 give back the same samples, in order and by place
static void testCvbsTracks(const QString &directory)
{
    std::cerr << "Testing cvbs tracks\n";
    struct Shape { int width; qint64 samples; const char *system; };
    for (const Shape shape: { Shape { 910, 477750, "NTSC" }, Shape { 1135, 709379, "PAL" } }) {
        for (const Ldx::Codec codec: { Ldx::Codec::Raw, Ldx::Codec::Ffv1 }) {
            if (codec == Ldx::Codec::Ffv1 && !Ldx::ffv1Available()) continue;
            const QString path = directory + "/cvbs.ldx";
            Ldx::Writer writer;
            Ldx::Track track;
            track.kind = Ldx::Kind::Cvbs;
            track.codec = codec;
            track.depth = 10;
            track.name = "a cvbs capture";
            track.parameters["system"] = shape.system;
            track.parameters["lineWidth"] = shape.width;
            track.parameters["frameSamples"] = static_cast<double>(shape.samples);
            track.parameters["padValue"] = 252;
            writer.addTrack(track);
            QString error;
            CHECK(writer.open(path, error));
            for (int n = 0; n < 4; n++) CHECK(writer.writeBlock(1, n * 33366000LL, cvbsFrame(shape.samples, shape.width, n)));
            // A frame of the wrong size is refused
            CHECK(!writer.writeBlock(1, 4 * 33366000LL, QByteArray(100, 0)));
            CHECK(writer.finish(error));
            if (codec == Ldx::Codec::Ffv1) CHECK(QFileInfo(path).size() < 4 * shape.samples * 2 / 2);

            Ldx::Reader reader;
            CHECK(reader.open(path, error));
            CHECK(reader.tracks().size() == 1 && reader.tracks()[0].kind == Ldx::Kind::Cvbs && reader.tracks()[0].codec == codec);
            CHECK(reader.tracks()[0].parameters["lineWidth"].toInt() == shape.width);
            Ldx::Block block;
            int index = 0;
            while (reader.nextBlock(block)) {
                CHECK(block.data == cvbsFrame(shape.samples, shape.width, index));
                index++;
            }
            CHECK(index == 4);

            Ldx::Reader random;
            CHECK(random.open(path, error) && random.buildIndex(error));
            CHECK(random.blockCount(1) == 4);
            for (const int wanted: { 3, 0, 2, 2 }) {
                CHECK(random.readBlock(1, wanted, block) && block.data == cvbsFrame(shape.samples, shape.width, wanted));
            }
        }
    }
}

// A stream (a pipe, here a file that is only written and read in order): everything comes back, with no index
static void testStream(const QString &path)
{
    std::cerr << "Testing a stream\n";

    QVector<Ldx::Block> written;
    {
        Ldx::Writer writer;
        Ldx::Track plain;
        plain.kind = Ldx::Kind::Tbc;
        plain.name = "raw";
        writer.addTrack(plain);
        Ldx::Track metadata;
        metadata.kind = Ldx::Kind::Metadata;
        metadata.depth = 0;
        writer.addTrack(metadata);
        if (Ldx::flacAvailable()) {
            Ldx::Track flac;
            flac.kind = Ldx::Kind::Tbc;
            flac.codec = Ldx::Codec::Flac;
            writer.addTrack(flac);
            Ldx::Track audio;
            audio.kind = Ldx::Kind::Pcm;
            audio.codec = Ldx::Codec::Flac;
            writer.addTrack(audio);
        }
        writer.addAttachment("session.stack", "application/json", "{}");
        QJsonObject entry;
        entry["tool"] = "stream";
        writer.setHistory(QJsonArray { entry });

        FILE *file = std::fopen(path.toLocal8Bit().constData(), "wb");
        CHECK(file);
        QString error;
        CHECK(writer.openStream(file, error));
        auto put = [&](quint32 track, qint64 timeNs, const QByteArray &data) {
            Ldx::Block block;
            block.track = track;
            block.timeNs = timeNs;
            block.data = data;
            CHECK(writer.writeBlock(block.track, block.timeNs, block.data));
            written.append(block);
        };
        put(2, 0, QByteArray("metadata"));
        for (int i = 0; i < 6; i++) {
            const qint64 time = i * 16683000LL;
            put(1, time, wave(239330, i));
            if (Ldx::flacAvailable()) {
                put(3, time, wave(239330, i + 20));
                put(4, time, wave(1470 * 2, i + 40));
            }
        }
        CHECK(writer.finish(error));
        std::fclose(file);
    }

    FILE *file = std::fopen(path.toLocal8Bit().constData(), "rb");
    CHECK(file);
    Ldx::Reader reader;
    QString error;
    CHECK(reader.openStream(file, error));
    CHECK(reader.profile() == Ldx::ProfileVersion);
    CHECK(reader.tracks().size() == (Ldx::flacAvailable() ? 4 : 2));
    CHECK(reader.history().size() == 1);
    QByteArray session;
    CHECK(reader.attachment("session.stack", session) && session == "{}");
    Ldx::Block block;
    int index = 0;
    while (reader.nextBlock(block)) {
        CHECK(index < written.size());
        CHECK(block.track == written[index].track && block.timeNs == written[index].timeNs && block.data == written[index].data);
        index++;
    }
    CHECK(index == written.size());
    // There is no way to the blocks but in turn
    CHECK(!reader.buildIndex(error));
    std::fclose(file);

    // The same stream, saved as it came, is a file with no index: it is opened as any file, the index is made by reading it
    {
        Ldx::Reader saved;
        CHECK(saved.open(path, error));
        CHECK(saved.buildIndex(error));
        std::map<quint32, qint64> counts;
        for (const Ldx::Block &expected: written) counts[expected.track]++;
        for (const auto &count: counts) CHECK(saved.blockCount(count.first) == count.second);
        std::map<quint32, qint64> seen;
        for (const Ldx::Block &expected: written) {
            Ldx::Block got;
            CHECK(saved.readBlock(expected.track, seen[expected.track]++, got));
            CHECK(got.data == expected.data && got.timeNs == expected.timeNs);
        }
    }

    // And it can be added to where it ends
    {
        Ldx::Writer more;
        CHECK(more.openAppend(path, error));
        more.addHistory(QJsonObject { { "tool", "append" } });
        CHECK(more.writeBlock(2, 200000000LL, QByteArray("more metadata")));
        CHECK(more.finish(error));
        Ldx::Reader again;
        CHECK(again.open(path, error));
        CHECK(again.buildIndex(error));
        CHECK(again.blockCount(2) == 2);
        Ldx::Block last;
        CHECK(again.readBlock(2, 1, last) && last.data == "more metadata");
        CHECK(again.history().size() == 2);
    }
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

// Several blocks of a track at the same time (the metadata of single fields go with the video of their time): the index
// has all of them, in the order they were written
static void testSameTime(const QString &path)
{
    std::cerr << "Testing blocks of one track at the same time\n";

    Ldx::Writer writer;
    Ldx::Track metadata;
    metadata.kind = Ldx::Kind::Metadata;
    writer.addTrack(metadata);
    Ldx::Track video;
    video.kind = Ldx::Kind::Pcm;
    writer.addTrack(video);
    QString error;
    CHECK(writer.open(path, error));
    for (int step = 0; step < 6; step++) {
        const qint64 time = (step / 3) * 20000000;      // three blocks at each time
        CHECK(writer.writeBlock(1, time, pattern(50 + step, step)));
        if (step % 3 == 0) CHECK(writer.writeBlock(2, time, pattern(100, step)));
    }
    CHECK(writer.finish(error));

    Ldx::Reader reader;
    CHECK(reader.open(path, error));
    CHECK(reader.buildIndex(error));
    CHECK(reader.blockCount(1) == 6);
    for (int step = 0; step < 6; step++) {
        Ldx::Block block;
        CHECK(reader.readBlock(1, step, block));
        CHECK(block.data == pattern(50 + step, step));
    }
}

#ifdef LDX_EXAMPLES_DIR
// The examples of the specification (docs/Specifications/examples): what a reader returns for each track, by SHA-256
static QString sumOfTrack(Ldx::Reader &reader, quint32 track)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 blocks = reader.blockCount(track);
    for (qint64 i = 0; i < blocks; i++) {
        Ldx::Block block;
        CHECK(reader.readBlock(track, i, block));
        hash.addData(block.data);
    }
    return QString::fromLatin1(hash.result().toHex());
}

static void testExamples()
{
    std::cerr << "Testing the examples of the specification\n";
    const QString directory = QStringLiteral(LDX_EXAMPLES_DIR);
    const QString tbc = "191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f";
    const QString pcm = "2e1345f1fc63dacca0c177ceaba46d651c41ce3368809f62324010c69d178464";
    struct Expected { const char *file; quint32 track; const char *sum; };
    const Expected expected[] = {
        { "tbc-raw.ldx", 1, "191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f" },
        { "tbc-raw.ldx", 3, "2e1345f1fc63dacca0c177ceaba46d651c41ce3368809f62324010c69d178464" },
        { "tbc-raw.ldx", 4, "95f267b5cfda93763d87c0121cefc7dcdee75ce2d51aaf65e4386fe61eededa4" },
        { "tbc-flac.ldx", 1, "191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f" },
        { "tbc-flac.ldx", 3, "2e1345f1fc63dacca0c177ceaba46d651c41ce3368809f62324010c69d178464" },
        { "tbc-stream.ldx", 1, "191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f" },
        { "tbc-nocues.ldx", 1, "191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f" },
        { "tbc-nocues.ldx", 3, "2e1345f1fc63dacca0c177ceaba46d651c41ce3368809f62324010c69d178464" },
        { "tbc-yc.ldx", 3, "251d7b04048c25f51eb7780e2222c8b4ac17ffdde6cd35ebe3cf2e52b978f917" },
    };
    Q_UNUSED(tbc);
    Q_UNUSED(pcm);
    for (const Expected &entry: expected) {
        const QString file = directory + "/" + entry.file;
        if (QString(entry.file).contains("flac") && !Ldx::flacAvailable()) continue;
        Ldx::Reader reader;
        QString error;
        CHECK(reader.open(file, error));
        CHECK(reader.profile() == 1);
        CHECK(reader.buildIndex(error));
        if (sumOfTrack(reader, entry.track) != entry.sum) {
            std::cerr << "  " << entry.file << " track " << entry.track << ": not the bytes the specification says\n";
            std::exit(1);
        }
    }
    // The metadata of the stream has the parameters and then a block for each field, before the video of its field
    Ldx::Reader reader;
    QString error;
    CHECK(reader.open(directory + "/tbc-stream.ldx", error));
    CHECK(reader.buildIndex(error));
    CHECK(reader.blockCount(2) == 5);
    // The FFV1 example: two frames of 477,750 words
    if (Ldx::ffv1Available()) {
        Ldx::Reader cvbs;
        CHECK(cvbs.open(directory + "/cvbs-ffv1.ldx", error));
        CHECK(cvbs.buildIndex(error));
        CHECK(cvbs.blockCount(1) == 2);
        Ldx::Block frame;
        CHECK(cvbs.readBlock(1, 0, frame));
        CHECK(frame.data.size() == 477750 * 2);
        CHECK(sumOfTrack(cvbs, 3) == "c422f4b9314a316b74193e02dee667e24c548ebe906d48bb3cdd25849f0d6f31" || cvbs.blockCount(3) == 1);
    }
}
#endif

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    testBlockSizes(directory.path());
    testRoundTrip(directory.filePath("test.ldx"));
    testIdsAndAttachments(directory.filePath("ids.ldx"));
    testAppend(directory.filePath("append.ldx"));
    testFlac(directory.path());
    testFlacTracks(directory.filePath("flac.ldx"));
    testUnsignedSamples(directory.path());
    testFfv1();
    testCvbsTracks(directory.path());
    testStream(directory.filePath("stream.ldx"));
    testSameTime(directory.filePath("sametime.ldx"));
    testNotAnLdxFile(directory.filePath("garbage.ldx"));
#ifdef LDX_EXAMPLES_DIR
    testExamples();
#endif

    std::cerr << "All tests passed\n";
    return 0;
}
