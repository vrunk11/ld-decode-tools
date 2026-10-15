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

#include "ldxcapture.h"
#include "tbcfiles.h"
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
#endif

    std::fprintf(stderr, "teststackproject: all tests passed\n");
    return 0;
}
