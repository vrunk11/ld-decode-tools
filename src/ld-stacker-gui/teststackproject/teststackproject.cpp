/******************************************************************************
 * teststackproject.cpp
 * ld-stacker-gui - Unit tests of the .stack session file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "ldxcapture.h"
#include "tbcfiles.h"
#include "lddecodemetadata.h"
#include "stackedvideo.h"
#include "stackproject.h"
#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

#define CHECK(condition) \
    do { if (!(condition)) { std::fprintf(stderr, "FAILED: %s (line %d)\n", #condition, __LINE__); std::exit(1); } } while (0)

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QDir base(directory.path());

    // Saved with the paths relative to the session, and read back whole
    StackProject project;
    StackSource a;
    a.id = "a";
    a.file = base.absoluteFilePath("captures/a.tbc");
    a.label = "Disc copy A";
    StackSource b;
    b.id = "b";
    b.file = base.absoluteFilePath("captures/b.tbc");
    b.enabled = false;
    b.offset = -2;
    project.sources = { a, b };
    project.stacking.mode = 2;
    project.stacking.smartThreshold = 20;
    project.stacking.diffDod = false;
    project.stacking.reverseFields = true;
    project.outputFile = base.absoluteFilePath("stack.tbc");
    project.metadataFormat = "json";
    project.zoom = 3.0;
    project.selected = "b";
    project.showStack = false;

    const QString fileName = base.absoluteFilePath("session.stack");
    QString error;
    CHECK(project.save(fileName, error));

    QFile file(fileName);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    file.close();   // (a file open cannot be replaced on Windows)
    CHECK(text.contains("\"captures/a.tbc\""));
    CHECK(!text.contains(directory.path().toUtf8()));

    StackProject loaded;
    CHECK(loaded.load(fileName, error));
    CHECK(loaded.sources.size() == 2);
    CHECK(loaded.sources[0].id == "a");
    CHECK(loaded.sources[0].file == a.file);
    CHECK(loaded.sources[0].label == "Disc copy A");
    CHECK(loaded.sources[0].enabled);
    CHECK(loaded.sources[1].id == "b");
    CHECK(!loaded.sources[1].enabled);
    CHECK(loaded.sources[1].offset == -2);
    CHECK(loaded.stacking.mode == 2);
    CHECK(loaded.stacking.smartThreshold == 20);
    CHECK(!loaded.stacking.diffDod);
    CHECK(loaded.stacking.reverseFields);
    CHECK(loaded.outputFile == project.outputFile);
    CHECK(loaded.metadataFormat == "json");
    CHECK(loaded.zoom == 3.0);
    CHECK(loaded.selected == "b");
    CHECK(!loaded.showStack);

    // In a session kept in an LDX file a source is a track of that file, and has no path
    StackProject embedded;
    StackSource t1;
    t1.id = "a";
    t1.track = 1;
    StackSource t3;
    t3.id = "b";
    t3.track = 3;
    t3.offset = 4;
    embedded.sources = { t1, t3 };
    CHECK(embedded.save(fileName, error));
    StackProject embeddedLoaded;
    CHECK(embeddedLoaded.load(fileName, error));
    CHECK(embeddedLoaded.sources.size() == 2);
    CHECK(embeddedLoaded.sources[0].track == 1 && embeddedLoaded.sources[0].file.isEmpty());
    CHECK(embeddedLoaded.sources[1].track == 3 && embeddedLoaded.sources[1].offset == 4);
    CHECK(!project.sources.isEmpty() && project.sources[0].track == 0);

    // Auto is the mode when there is none
    project.stacking.mode = -1;
    CHECK(project.save(fileName, error));
    CHECK(loaded.load(fileName, error));
    CHECK(loaded.stacking.mode == -1);

    // Only the file of a source is needed, the rest has defaults; a source without an id gets one
    QFile minimal(base.absoluteFilePath("minimal.stack"));
    CHECK(minimal.open(QIODevice::WriteOnly));
    minimal.write("{ \"version\": 1, \"sources\": [ { \"file\": \"x.tbc\" }, { \"file\": \"y.tbc\" } ] }");
    minimal.close();
    StackProject small;
    CHECK(small.load(minimal.fileName(), error));
    CHECK(small.sources.size() == 2);
    CHECK(small.sources[0].enabled && small.sources[0].offset == 0);
    CHECK(!small.sources[0].id.isEmpty() && small.sources[0].id != small.sources[1].id);
    CHECK(small.stacking.mode == -1 && small.stacking.smartThreshold == 15 && small.stacking.diffDod);

    // What is not a session, or a version this program does not read, is refused
    QFile bad(base.absoluteFilePath("bad.stack"));
    CHECK(bad.open(QIODevice::WriteOnly));
    bad.write("not json");
    bad.close();
    CHECK(!small.load(bad.fileName(), error));
    CHECK(small.sources.size() == 2);   // unchanged by a failed load

    QFile future(base.absoluteFilePath("future.stack"));
    CHECK(future.open(QIODevice::WriteOnly));
    future.write("{ \"version\": 99, \"sources\": [] }");
    future.close();
    CHECK(!small.load(future.fileName(), error));
    CHECK(!small.load(base.absoluteFilePath("missing.stack"), error));

    // The names of the captures kept in an LDX file
    QString path;
    qint32 track = -1;
    LdxCapture::split("dir/capture.ldx#3", path, track);
    CHECK(path == "dir/capture.ldx" && track == 3);
    LdxCapture::split("dir/capture.ldx", path, track);
    CHECK(path == "dir/capture.ldx" && track == 0);
    LdxCapture::split("dir/capture#3.tbc", path, track);          // a # that is not of a track
    CHECK(path == "dir/capture#3.tbc" && track == 0);
    CHECK(LdxCapture::isLdxName("a.ldx") && LdxCapture::isLdxName("a.LDX#2") && !LdxCapture::isLdxName("a.tbc"));
    CHECK(LdxCapture::trackName("a.ldx", 4) == "a.ldx#4" && LdxCapture::trackName("a.ldx", 0) == "a.ldx");
    StackSource linked;
    linked.file = "other.ldx";
    linked.track = 2;
    CHECK(linked.reference() == "other.ldx#2");
    linked.track = 0;
    CHECK(linked.reference() == "other.ldx");
    linked.file = "c.tbc";
    linked.track = 5;
    CHECK(linked.reference() == "c.tbc");                         // (a track only means something in an LDX file)

#ifdef LDDECODE_WITH_LDX
    // A session kept in an LDX file: its sources that are tracks become the file and the track
    {
        Ldx::Writer writer;
        Ldx::Track video;
        video.kind = Ldx::Kind::Tbc;
        writer.addTrack(video);
        const QByteArray session = "{ \"version\": 1, \"sources\": [ { \"id\": \"a\", \"track\": 1 }, { \"id\": \"b\", \"file\": \"far.ldx\", \"track\": 7, \"offset\": 2 },"
                                   " { \"id\": \"c\", \"file\": \"x.tbc\" } ] }";
        writer.addAttachment("session.stack", "application/json", session);
        const QString ldxFile = base.absoluteFilePath("kept.ldx");
        CHECK(writer.open(ldxFile, error));
        CHECK(writer.writeBlock(1, 0, QByteArray(10, 'x')));
        CHECK(writer.finish(error));

        CHECK(StackProject::isSession(ldxFile));
        CHECK(!StackProject::isSession(ldxFile + "#1"));
        CHECK(StackProject::isSession(fileName));
        StackProject kept;
        CHECK(kept.load(ldxFile, error));
        CHECK(kept.sources.size() == 3);
        CHECK(kept.sources[0].file == QDir::cleanPath(ldxFile) && kept.sources[0].track == 1);
        CHECK(kept.sources[0].reference() == QDir::cleanPath(ldxFile) + "#1");
        CHECK(kept.sources[1].file == base.absoluteFilePath("far.ldx") && kept.sources[1].track == 7 && kept.sources[1].offset == 2);
        CHECK(kept.sources[2].file == base.absoluteFilePath("x.tbc") && kept.sources[2].track == 0);
        // (an LDX file with no session is not one)
        Ldx::Writer plain;
        Ldx::Track only;
        only.kind = Ldx::Kind::Tbc;
        plain.addTrack(only);
        const QString plainFile = base.absoluteFilePath("plain.ldx");
        CHECK(plain.open(plainFile, error));
        CHECK(plain.finish(error));
        CHECK(!StackProject::isSession(plainFile));
        CHECK(!kept.load(plainFile, error));
    }

    // The mapping of a capture in an LDX file: a track that says which track it is the mapping of, found from each other
    {
        Ldx::Writer writer;
        Ldx::Track original, mapped, other, orphan;
        original.kind = mapped.kind = other.kind = orphan.kind = Ldx::Kind::Tbc;
        writer.addTrack(original);
        mapped.parameters["mappedFrom"] = static_cast<int>(original.number);
        writer.addTrack(mapped);
        writer.addTrack(other);
        orphan.parameters["mappedFrom"] = 9;        // (a track that is not in the file: it is not a mapping)
        writer.addTrack(orphan);
        const QString mapFile = base.absoluteFilePath("mapped.ldx");
        CHECK(writer.open(mapFile, error));
        for (qint32 id = 1; id <= 4; id++) CHECK(writer.writeBlock(id, 0, QByteArray(10, 'x')));
        CHECK(writer.finish(error));

        CHECK(LdxCapture::isMappedTrack(mapFile + "#2"));
        CHECK(!LdxCapture::isMappedTrack(mapFile + "#1") && !LdxCapture::isMappedTrack(mapFile + "#3") && !LdxCapture::isMappedTrack(mapFile + "#4"));
        CHECK(LdxCapture::mappedOfOriginal(mapFile) == mapFile + "#2");
        CHECK(LdxCapture::mappedOfOriginal(mapFile + "#1") == mapFile + "#2");
        CHECK(LdxCapture::mappedOfOriginal(mapFile + "#3").isEmpty());
        CHECK(LdxCapture::originalOfMapped(mapFile + "#2") == mapFile + "#1");
        CHECK(LdxCapture::originalOfMapped(mapFile + "#1") == mapFile + "#1");
        CHECK(LdxCapture::originalOfMapped(mapFile + "#4") == mapFile + "#4");
        // (a file name that is not an LDX one has none of it)
        CHECK(!LdxCapture::isMappedTrack("disc.tbc") && LdxCapture::mappedOfOriginal("disc.tbc").isEmpty() && LdxCapture::originalOfMapped("disc.tbc") == "disc.tbc");
    }

    // The tracks of a capture: its video, the metadata about it and its analogue audio (not the audio decoded from EFM)
    {
        Ldx::Writer writer;
        Ldx::Track video, metadata, audio, second, secondMetadata, efm;
        video.kind = second.kind = Ldx::Kind::Tbc;
        metadata.kind = secondMetadata.kind = Ldx::Kind::Metadata;
        audio.kind = efm.kind = Ldx::Kind::Pcm;
        writer.addTrack(video);
        metadata.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(metadata);
        audio.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(audio);
        writer.addTrack(second);
        secondMetadata.parameters["track"] = static_cast<int>(second.number);
        writer.addTrack(secondMetadata);
        efm.parameters["track"] = static_cast<int>(second.number);
        efm.parameters["source"] = "efm";
        writer.addTrack(efm);
        const QString tracksFile = base.absoluteFilePath("tracks.ldx");
        CHECK(writer.open(tracksFile, error));
        for (qint32 id = 1; id <= 6; id++) CHECK(writer.writeBlock(id, 0, QByteArray(10, 'x')));
        CHECK(writer.finish(error));
        qint32 v = -1, m = -1, a = -1;
        CHECK(LdxCapture::captureTracks(tracksFile, v, m, a) && v == 1 && m == 2 && a == 3);
        CHECK(LdxCapture::captureTracks(tracksFile + "#1", v, m, a) && v == 1 && m == 2 && a == 3);
        CHECK(LdxCapture::captureTracks(tracksFile + "#4", v, m, a) && v == 4 && m == 5 && a == 0);
        CHECK(!LdxCapture::captureTracks(tracksFile + "#2", v, m, a));
        CHECK(!LdxCapture::captureTracks("disc.tbc", v, m, a));
    }

    // A Y/C capture in an LDX file: the chroma track is about the luma one, and each is found from the other
    {
        Ldx::Writer writer;
        Ldx::Track luma, chroma;
        luma.kind = Ldx::Kind::Tbc;
        chroma.kind = Ldx::Kind::Tbc;
        writer.addTrack(luma);
        chroma.parameters["role"] = "chroma";
        chroma.parameters["track"] = static_cast<int>(luma.number);
        writer.addTrack(chroma);
        const QString ycFile = base.absoluteFilePath("yc.ldx");
        CHECK(writer.open(ycFile, error));
        CHECK(writer.writeBlock(1, 0, QByteArray(10, 'x')));
        CHECK(writer.writeBlock(2, 0, QByteArray(10, 'y')));
        CHECK(writer.finish(error));

        CHECK(LdxCapture::chromaOfLuma(ycFile) == ycFile + "#2");
        CHECK(LdxCapture::chromaOfLuma(ycFile + "#1") == ycFile + "#2");
        CHECK(LdxCapture::isChromaTrack(ycFile + "#2") && !LdxCapture::isChromaTrack(ycFile + "#1") && !LdxCapture::isChromaTrack(ycFile));
        CHECK(LdxCapture::lumaOfChroma(ycFile + "#2") == ycFile + "#1");
        CHECK(TbcFiles::chromaFileName(ycFile) == ycFile + "#2" && TbcFiles::lumaFileName(ycFile + "#2") == ycFile + "#1");
        QVector<qint32> tracks;
        QStringList names;
        CHECK(LdxCapture::listCaptures(ycFile, tracks, names, error));
        CHECK(tracks.size() == 1 && tracks[0] == 1);      // (the chroma goes with its luma)
    }

    // The same for a CVBS Y/C pair (two cvbs tracks)
    {
        Ldx::Writer writer;
        Ldx::Track luma, chroma;
        luma.kind = Ldx::Kind::Cvbs;
        chroma.kind = Ldx::Kind::Cvbs;
        writer.addTrack(luma);
        chroma.parameters["role"] = "chroma";
        chroma.parameters["track"] = static_cast<int>(luma.number);
        writer.addTrack(chroma);
        const QString ycFile = base.absoluteFilePath("cvbsyc.ldx");
        CHECK(writer.open(ycFile, error));
        CHECK(writer.writeBlock(1, 0, QByteArray(10, 'x')));
        CHECK(writer.writeBlock(2, 0, QByteArray(10, 'y')));
        CHECK(writer.finish(error));
        CHECK(LdxCapture::chromaOfLuma(ycFile) == ycFile + "#2" && LdxCapture::isChromaTrack(ycFile + "#2"));
        CHECK(LdxCapture::lumaOfChroma(ycFile + "#2") == ycFile + "#1");
        QVector<qint32> tracks;
        QStringList names;
        CHECK(LdxCapture::listCaptures(ycFile, tracks, names, error));
        CHECK(tracks.size() == 1 && tracks[0] == 1);
    }
#endif

    // The current metadata of a capture from the blocks of its track: a set, then fields replace, then a new set starts again
    {
        const QByteArray whole = "{\"videoParameters\":{\"numberOfSequentialFields\":2,\"system\":\"NTSC\"},\"fields\":[{\"seqNo\":1,\"syncConf\":10},{\"seqNo\":2,\"syncConf\":20}]}";
        LdxMetadataMerger merger;
        merger.add(whole);
        merger.add("{\"seqNo\":2,\"syncConf\":99}");
        merger.add("{\"seqNo\":3,\"syncConf\":30}");
        const QJsonObject now = QJsonDocument::fromJson(merger.current()).object();
        const QJsonArray fields = now.value("fields").toArray();
        CHECK(fields.size() == 3);
        CHECK(fields[0].toObject().value("syncConf").toInt() == 10 && fields[1].toObject().value("syncConf").toInt() == 99 && fields[2].toObject().value("syncConf").toInt() == 30);
        CHECK(now.value("videoParameters").toObject().value("numberOfSequentialFields").toInt() == 3);
        merger.add("{\"videoParameters\":{\"numberOfSequentialFields\":0,\"system\":\"NTSC\"}}");
        merger.add("{\"seqNo\":1,\"syncConf\":5}");
        const QJsonObject again = QJsonDocument::fromJson(merger.current()).object();
        CHECK(again.value("fields").toArray().size() == 1 && again.value("fields").toArray()[0].toObject().value("syncConf").toInt() == 5);
    }

    // The sources are lined up by the numbers of the disc: the timeline goes from the earliest frame to the latest of any source, with padded frames where none has
    {
        qint32 total = 0;
        qint32 discAtStart = 0;
        // a begins at picture 100 (10 frames), b at 105 (8 frames), c has no numbers (3 frames, offset 1): it begins at the number 0, plus its offset
        const QVector<qint32> placed = StackedVideo::alignedPositions({ 100, 105, -1 }, { 0, 0, 1 }, { 10, 8, 3 }, total, &discAtStart);
        CHECK(placed == QVector<qint32>({ 99, 104, 0 }) && total == 112 && discAtStart == 1);
        // an offset that puts a source before the earliest moves the start of the timeline
        const QVector<qint32> earlier = StackedVideo::alignedPositions({ 100, 105 }, { 0, -10 }, { 10, 8 }, total, &discAtStart);
        CHECK(earlier == QVector<qint32>({ 5, 0 }) && total == 15 && discAtStart == 95);
        const QVector<qint32> none = StackedVideo::alignedPositions({}, {}, {}, total, &discAtStart);
        CHECK(none.isEmpty() && total == 0);

        // From metadata: the metadata of the session has the frames of both, in the order of the disc
        const auto writeCapture = [&](const QString &name, qint32 firstPicture, qint32 frames, bool mapped = false) {
            LdDecodeMetaData metadata;
            LdDecodeMetaData::VideoParameters parameters;
            parameters.system = NTSC;
            parameters.fieldWidth = 910;
            parameters.fieldHeight = 263;
            parameters.isMapped = mapped;
            metadata.setVideoParameters(parameters);
            for (qint32 frame = 0; frame < frames; frame++) {
                const qint32 picture = firstPicture + frame;
                const qint32 bcd = ((picture / 10000) << 16) | (((picture / 1000) % 10) << 12) | (((picture / 100) % 10) << 8) | (((picture / 10) % 10) << 4) | (picture % 10);
                for (qint32 which = 0; which < 2; which++) {
                    LdDecodeMetaData::Field field;
                    field.isFirstField = which == 0;
                    field.fieldPhaseID = 1;
                    field.vbi.inUse = true;
                    field.vbi.vbiData = { 0, 0xF00000 | bcd, 0 };
                    metadata.appendField(field);
                }
            }
            const QString tbc = base.absoluteFilePath(name + ".tbc");
            QFile file(tbc);
            CHECK(file.open(QIODevice::WriteOnly));
            file.close();
            CHECK(metadata.write(tbc + ".json"));
            return tbc;
        };
        StackProject session;
        StackSource first;
        first.id = "a";
        first.file = writeCapture("disc-a", 100, 10);
        StackSource second;
        second.id = "b";
        second.file = writeCapture("disc-b", 105, 8);
        session.sources = { first, second };
        const QString sessionFile = base.absoluteFilePath("aligned.stack");
        CHECK(session.save(sessionFile, error));
        LdDecodeMetaData stacked;
        CHECK(StackedVideo::readMetadata(sessionFile, stacked, error));
        CHECK(stacked.getNumberOfFrames() == 13);
        // (frame 11 to 13 are those of b only, and every frame is whole)
        for (qint32 frame = 1; frame <= 13; frame++) {
            CHECK(!stacked.getField(stacked.getFirstFieldNumber(frame)).pad);
        }
        // With b moved 10 frames on, there is a gap of padded frames between them
        session.sources[1].offset = 10;
        CHECK(session.save(sessionFile, error));
        LdDecodeMetaData gap;
        CHECK(StackedVideo::readMetadata(sessionFile, gap, error));
        CHECK(gap.getNumberOfFrames() == 23);
        CHECK(!gap.getField(gap.getFirstFieldNumber(10)).pad && gap.getField(gap.getFirstFieldNumber(11)).pad && gap.getField(gap.getSecondFieldNumber(15)).pad);
        CHECK(!gap.getField(gap.getFirstFieldNumber(16)).pad);

        // A source that has a mapping beside it (name_map) is read from that; one that has none is read as it is
        StackSource plain;
        plain.id = "c";
        plain.file = writeCapture("disc-c", 105, 8);
        StackProject withoutMap;
        withoutMap.sources = { plain };
        CHECK(withoutMap.save(sessionFile, error));
        LdDecodeMetaData notMapped;
        CHECK(StackedVideo::readMetadata(sessionFile, notMapped, error) && notMapped.getNumberOfFrames() == 8);
        writeCapture("disc-c_map", 100, 12, true);
        LdDecodeMetaData viaMap;
        CHECK(StackedVideo::readMetadata(sessionFile, viaMap, error) && viaMap.getNumberOfFrames() == 12);
        // (a source that is mapped already is not replaced by the mapping of it)
        StackSource done;
        done.id = "d";
        done.file = writeCapture("disc-d", 100, 5, true);
        writeCapture("disc-d_map", 100, 9, true);
        StackProject mappedAlready;
        mappedAlready.sources = { done };
        CHECK(mappedAlready.save(sessionFile, error));
        LdDecodeMetaData same;
        CHECK(StackedVideo::readMetadata(sessionFile, same, error) && same.getNumberOfFrames() == 5);

        // followMap: the mapping beside a source is the one that is read, unless the source says not to follow it; a source that is a mapping already stays
        {
            StackSource follow = plain;
            const QString mappedFile = TbcFiles::mappedFileName(plain.file);
            CHECK(follow.resolvedReference() == mappedFile);
            follow.followMap = false;
            CHECK(follow.resolvedReference() == plain.file);
            StackSource onMapping;
            onMapping.file = mappedFile;
            CHECK(onMapping.resolvedReference() == mappedFile);
            onMapping.followMap = false;
            CHECK(onMapping.resolvedReference() == plain.file);
            CHECK(done.resolvedReference() == done.file);
            // (kept in the session only when it is off; a session that does not say follows)
            StackProject saved;
            follow.id = "a";
            StackSource other = plain;
            other.id = "b";
            saved.sources = { follow, other };
            CHECK(saved.save(sessionFile, error));
            StackProject reread;
            CHECK(reread.load(sessionFile, error));
            CHECK(reread.sources.size() == 2 && !reread.sources[0].followMap && reread.sources[1].followMap);
        }

        // What was measured is kept in the session, and is the SNR in the metadata of the stack when it is good for these captures, offsets and options
        {
            StackProject measuredSession;
            measuredSession.sources = { first, second };
            StackMeasure measure;
            measure.key = measuredSession.stackMeasureKey(false);
            for (qint32 t = 1; t <= 13; t++) {
                measure.white.append(t == 5 ? std::nanf("") : 30.0f + t);
                measure.black.append(20.0f + t);
                measure.residual.append(static_cast<float>(t));
            }
            measuredSession.measures = { measure };
            CHECK(measuredSession.save(sessionFile, error));
            StackProject back;
            CHECK(back.load(sessionFile, error));
            CHECK(back.measures.size() == 1 && back.measures[0].key == measure.key && back.measures[0].white.size() == 13);
            CHECK(!std::isfinite(back.measures[0].white[4]) && std::fabs(back.measures[0].white[2] - 33.0f) < 0.01f && back.measures[0].residual[12] == 13.0f);
            CHECK(back.measureFor(back.stackMeasureKey(false)) != nullptr && back.measureFor(back.stackMeasureKey(true)) == nullptr);
            LdDecodeMetaData withSnr;
            CHECK(StackedVideo::readMetadata(sessionFile, withSnr, error));
            const LdDecodeMetaData::Field frame3 = withSnr.getField(withSnr.getFirstFieldNumber(3));
            CHECK(frame3.vitsMetrics.inUse && std::fabs(frame3.vitsMetrics.wSNR - 33.0) < 0.01 && std::fabs(frame3.vitsMetrics.bPSNR - 23.0) < 0.01);
            // (a frame that has only one of them has that one)
            const LdDecodeMetaData::Field frame5 = withSnr.getField(withSnr.getFirstFieldNumber(5));
            CHECK(frame5.vitsMetrics.inUse && frame5.vitsMetrics.wSNR == 0.0 && std::fabs(frame5.vitsMetrics.bPSNR - 25.0) < 0.01);
            // Other offsets are other captures: the measure is not good for them, and there is no SNR
            measuredSession.sources[1].offset = 3;
            CHECK(measuredSession.save(sessionFile, error));
            LdDecodeMetaData without;
            CHECK(StackedVideo::readMetadata(sessionFile, without, error));
            CHECK(!without.getField(without.getFirstFieldNumber(3)).vitsMetrics.inUse);
        }
    }

    std::fprintf(stderr, "teststackproject: all tests passed\n");
    return 0;
}
