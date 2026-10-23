/******************************************************************************
 * testvideoio.cpp
 * ld-decode-tools TBC library - unit test of writing video (TargetVideo) and reading it back (SourceVideo)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "cvbs.h"
#include "sourcevideo.h"
#include "targetvideo.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static const qint32 lineLength = 5;
static const qint32 lines = 4;
static const qint32 fieldLength = lineLength * lines;

static QVector<quint16> field(int seed)
{
    QVector<quint16> data(fieldLength);
    for (int i = 0; i < fieldLength; i++) data[i] = static_cast<quint16>(seed * 1000 + i);
    return data;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    // Three fields to a TBC file, read back whole and by lines
    const QString tbc = directory.filePath("video.tbc");
    {
        TargetVideo target;
        CHECK(target.open(tbc, PAL));
        for (int i = 1; i <= 3; i++) CHECK(target.writeField(field(i)));
        CHECK(target.close());
    }
    CHECK(QFile(tbc).size() == 3 * fieldLength * 2);

    {
        SourceVideo source;
        CHECK(source.open(tbc, fieldLength, lineLength));
        CHECK(source.isSourceValid() && source.getNumberOfAvailableFields() == 3 && source.getFieldLength() == fieldLength);
        CHECK(source.hasField(1) && source.hasField(3) && !source.hasField(4) && !source.hasField(0));
        CHECK(source.getVideoField(2) == field(2));
        CHECK(source.getVideoField(1) == field(1));                  // (backwards)
        CHECK(source.getVideoField(3, 2, 3) == field(3).mid(lineLength, 2 * lineLength));
        CHECK(source.getVideoField(3, 1, 1) == field(3).mid(0, lineLength));
        source.close();
    }

    // A field that is not there, for a program that shows video: blank, of the size asked for
    {
        SourceVideo source;
        source.setLenient(true);
        CHECK(source.open(tbc, fieldLength, lineLength));
        const SourceVideo::Data whole = source.getVideoField(4);
        CHECK(whole.size() == fieldLength && whole == SourceVideo::Data(fieldLength, 0));
        const SourceVideo::Data part = source.getVideoField(4, 2, 3);
        CHECK(part.size() == 2 * lineLength && part == SourceVideo::Data(2 * lineLength, 0));
        CHECK(source.getVideoField(2) == field(2));             // (the position in the file was not lost)
        source.close();
    }

    // A file cut short inside a field
    const QString cut = directory.filePath("cut.tbc");
    {
        QFile file(tbc), out(cut);
        CHECK(file.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly));
        out.write(file.read(2 * fieldLength * 2 + 6));
    }
    {
        SourceVideo source;
        source.setLenient(true);
        CHECK(source.open(cut, fieldLength, lineLength));
        CHECK(source.getNumberOfAvailableFields() == 2);
        CHECK(source.getVideoField(2) == field(2));
        CHECK(source.getVideoField(3) == SourceVideo::Data(fieldLength, 0));
    }

    // A file that is not there
    {
        SourceVideo source;
        CHECK(!source.open(directory.filePath("nothing.tbc"), fieldLength, lineLength));
    }

    // CVBS: a frame of two fields written and read back as two fields (the samples are kept to the precision of the encoding)
    {
        const qint32 cvbsFieldLength = Cvbs::geometry(PAL).fieldLength();
        QVector<quint16> first(cvbsFieldLength), second(cvbsFieldLength);
        for (qint32 i = 0; i < cvbsFieldLength; i++) {
            first[i] = static_cast<quint16>(((i * 7) % 900 + 64) << 6);
            second[i] = static_cast<quint16>(((i * 13) % 900 + 64) << 6);
        }
        const QString cvbs = directory.filePath("video.cvbs");
        {
            TargetVideo target;
            CHECK(target.open(cvbs, PAL));
            CHECK(target.isCvbs());
            CHECK(target.writeField(first) && target.writeField(second));
            CHECK(target.close());
        }
        SourceVideo source;
        CHECK(source.open(cvbs, cvbsFieldLength, -1));
        CHECK(source.getNumberOfAvailableFields() == 2);
        const SourceVideo::Data a = source.getVideoField(1), b = source.getVideoField(2);
        CHECK(a.size() == cvbsFieldLength && b.size() == cvbsFieldLength);
        qint32 worst = 0;
        for (qint32 i = 0; i < cvbsFieldLength; i++) {
            worst = qMax(worst, qAbs(static_cast<qint32>(a[i]) - static_cast<qint32>(first[i])));
        }
        CHECK(worst <= 64);

        // (the second field has one line less: its last line is padding, which a frame does not hold)
        const Cvbs::Geometry geometry = Cvbs::geometry(PAL);
        const qint32 secondSamples = geometry.lineWidth * geometry.field2Lines;
        worst = 0;
        for (qint32 i = 0; i < secondSamples; i++) worst = qMax(worst, qAbs(static_cast<qint32>(b[i]) - static_cast<qint32>(second[i])));
        CHECK(worst <= 64);

        // Half a frame is dropped, and close says so
        const QString half = directory.filePath("half.cvbs");
        TargetVideo target;
        CHECK(target.open(half, PAL));
        CHECK(target.writeField(first));
        CHECK(!target.close());
    }

    std::printf("testvideoio: ok\n");
    return 0;
}
