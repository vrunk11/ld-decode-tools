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

#include "stackproject.h"

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

    std::fprintf(stderr, "teststackproject: all tests passed\n");
    return 0;
}
