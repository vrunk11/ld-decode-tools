/******************************************************************************
 * testframebackend.cpp
 * ld-stacker-gui - unit test of the reading of a source through its map
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

#include "framebackend.h"
#include "framemap.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static constexpr qint32 WIDTH = 16;
static constexpr qint32 HEIGHT = 6;
static constexpr qint32 FRAMES = 6;

static MapFrame frame(qint32 disc, qint32 capture, MapBasis basis)
{
    MapFrame value;
    value.discFrame = disc;
    value.captureFrame = capture;
    value.basis = basis;
    return value;
}

// What the interface is told of a source that is loaded (one connection for the whole test)
static SourceInfo lastInfo;
static bool gotInfo = false;

static void waitForSource()
{
    QElapsedTimer clock;
    clock.start();
    while (!gotInfo && clock.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(gotInfo);
}

static SourceInfo load(FrameBackend &backend, const QString &file, const QByteArray &mapJson, bool followMap = true)
{
    gotInfo = false;
    QMetaObject::invokeMethod(&backend, "addSource", Qt::DirectConnection, Q_ARG(QString, "a"), Q_ARG(QString, file),
                              Q_ARG(bool, false), Q_ARG(QByteArray, mapJson), Q_ARG(bool, followMap));
    waitForSource();
    return lastInfo;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString fileName = directory.filePath("capture.tbc");
    {
        QFile file(fileName);
        CHECK(file.open(QIODevice::WriteOnly));
        for (qint32 field = 1; field <= FRAMES * 2; field++) {
            QVector<quint16> samples(WIDTH * HEIGHT, static_cast<quint16>(100 + field));
            file.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
        }
    }
    LdDecodeMetaData capture;
    capture.initialiseDefault(PAL);
    LdDecodeMetaData::VideoParameters parameters = capture.getVideoParameters();
    parameters.fieldWidth = WIDTH;
    parameters.fieldHeight = HEIGHT;
    capture.setVideoParameters(parameters);
    capture.ensureFields(FRAMES * 2);
    for (qint32 n = 1; n <= FRAMES * 2; n++) {
        LdDecodeMetaData::Field field = capture.getField(n);
        field.isFirstField = n % 2 == 1;
        capture.updateField(field, n);
    }
    capture.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
    CHECK(capture.write(directory.filePath("capture.tbc.db")));

    // A map of 5 frames: two frames of the capture, a padded one, the frames 5 and 6
    FrameMap map = FrameMap::fromFrames({ frame(10, 1, MapBasis::Measured), frame(11, 2, MapBasis::Measured), frame(12, 0, MapBasis::Padded),
                                          frame(13, 5, MapBasis::Sequence), frame(14, 6, MapBasis::Measured) });
    map.discType = "CAV";
    map.captureSize = QFileInfo(fileName).size();
    map.captureFields = FRAMES * 2;

    FrameBackend backend;
    QObject::connect(&backend, &FrameBackend::sourceLoaded, &backend, [](const SourceInfo &info) { lastInfo = info; gotInfo = true; });

    // Without the map: the capture as it is
    const SourceInfo plain = load(backend, fileName, QByteArray());
    CHECK(plain.ok && !plain.throughMap && plain.frames == FRAMES);
    QMetaObject::invokeMethod(&backend, "removeSource", Qt::DirectConnection, Q_ARG(QString, "a"));

    // With it: the capture through its map, as a mapped source
    const SourceInfo mapped = load(backend, fileName, map.toJsonBytes());
    CHECK(mapped.ok && mapped.throughMap && mapped.isMapped);
    CHECK(mapped.frames == 5 && mapped.paddedFrames == 1);
    QMetaObject::invokeMethod(&backend, "removeSource", Qt::DirectConnection, Q_ARG(QString, "a"));

    // The map kept in the metadata of the source: used when the source follows the map, known when it does not
    {
        capture.setDiscMap(map);
        CHECK(capture.write(directory.filePath("capture.tbc.db")));
        const SourceInfo embedded = load(backend, fileName, QByteArray());
        CHECK(embedded.ok && embedded.throughMap && embedded.mapInSource && embedded.frames == 5);
        QMetaObject::invokeMethod(&backend, "removeSource", Qt::DirectConnection, Q_ARG(QString, "a"));

        const SourceInfo notFollowed = load(backend, fileName, QByteArray(), false);
        CHECK(notFollowed.ok && !notFollowed.throughMap && notFollowed.mapInSource && notFollowed.frames == FRAMES);
        QMetaObject::invokeMethod(&backend, "removeSource", Qt::DirectConnection, Q_ARG(QString, "a"));
        capture.clearDiscMap();
        CHECK(capture.write(directory.filePath("capture.tbc.db")));
    }

    // A map made for another capture is refused, with the reason
    FrameMap other = map;
    other.captureSize += 1;
    const SourceInfo refused = load(backend, fileName, other.toJsonBytes());
    CHECK(!refused.ok && refused.error.contains("another capture"));

    std::printf("testframebackend: ok\n");
    return 0;
}
