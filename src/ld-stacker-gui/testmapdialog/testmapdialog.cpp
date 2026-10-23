/******************************************************************************
 * testmapdialog.cpp
 * ld-stacker-gui - unit test of the mapping of a source with ld-discmap
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "framemap.h"
#include "lddecodemetadata.h"
#include "mapdialog.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString capture = directory.filePath("capture.tbc");
    CHECK(QFile::copy(QStringLiteral(TEST_DATA_DIR "/ntsc/ve-snw-cut.tbc"), capture));
    CHECK(QFile::copy(QStringLiteral(TEST_DATA_DIR "/ntsc/ve-snw-cut.tbc.db"), capture + ".db"));
    CHECK(QFile::copy(QStringLiteral(TEST_DATA_DIR "/ntsc/ve-snw-cut.pcm"), directory.filePath("capture.pcm")));

    MapDialog::Job job;
    job.id = "a";
    job.title = "capture";
    job.input = capture;
    MapDialog dialog({ job }, false);
    dialog.setSaveInSources(true);
    bool finished = false, success = false;
    QObject::connect(&dialog, &MapDialog::allFinished, &dialog, [&](bool ok) { finished = true; success = ok; });
    dialog.start();
    QElapsedTimer clock;
    clock.start();
    while (!finished && clock.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(finished && success);

    // The map is given back, and nothing is written beside the capture
    FrameMap map;
    QString error;
    CHECK(FrameMap::fromJsonBytes(dialog.mapFor("a"), map, error));
    CHECK(map.frameCount() == 29 && map.runs().size() == 1);
    CHECK(map.isFor(map.captureFields, QFileInfo(capture).size()));
    CHECK(!QFileInfo::exists(directory.filePath("capture_map.tbc")));
    CHECK(dialog.mappedIds() == QStringList({ "a" }));

    // And kept in the metadata of the source: the capture, read again, has the map
    CHECK(dialog.savedIds() == QStringList({ "a" }));
    LdDecodeMetaData saved;
    CHECK(saved.read(capture + ".db"));
    CHECK(saved.hasDiscMap() && saved.getDiscMap() == map);

    std::printf("testmapdialog: ok\n");
    return 0;
}
