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

#include <QJsonArray>

#include "cvbs.h"
#include "ldxcapture.h"
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

    // An LDX output: the fields as a TBC track, the analogue audio beside them, the history of the tool; read back as a capture
    {
        const QString ldx = directory.filePath("video.ldx");
        QJsonArray history;
        history.append(LdxCapture::historyEntry("testvideoio", "a test"));
        QByteArray audio(4 * 6, 0);
        for (int i = 0; i < audio.size(); i++) audio[i] = static_cast<char>(i * 5);
        {
            TargetVideo target;
            target.setLdxVideo(lineLength, lines, 1000000.0, "test capture", history);
            target.setLdxAudio(44100.0);
            CHECK(target.open(ldx, PAL));
            CHECK(target.isLdx());
            // (the audio of a frame is written before its fields: it begins with them)
            CHECK(target.writeAudio(audio.mid(0, 16)));
            CHECK(target.writeField(field(1)));
            CHECK(target.writeField(field(2)));
            CHECK(target.writeAudio(audio.mid(16, 8)));
            CHECK(target.writeField(field(3)));
            CHECK(target.close());
        }

        QString error;
        QVector<qint32> tracks;
        QStringList names;
        CHECK(LdxCapture::listCaptures(ldx, tracks, names, error) && tracks.size() == 1 && names[0] == "test capture");
        CHECK(LdxCapture::historyOf(ldx).size() >= 1);
        CHECK(!LdxCapture::isCvbs(ldx));

        SourceVideo source;
        CHECK(source.open(ldx, fieldLength, lineLength));
        CHECK(source.getNumberOfAvailableFields() == 3);
        CHECK(source.getVideoField(3) == field(3) && source.getVideoField(1) == field(1));
        CHECK(source.getVideoField(2, 2, 3) == field(2).mid(lineLength, 2 * lineLength));
        source.close();

        // (a field that the file does not have, for a program that shows video: blank, whole or by lines)
        SourceVideo lenient;
        lenient.setLenient(true);
        CHECK(lenient.open(ldx, fieldLength, lineLength));
        CHECK(lenient.getVideoField(9) == SourceVideo::Data(fieldLength, 0));
        CHECK(lenient.getVideoField(9, 1, 2) == SourceVideo::Data(2 * lineLength, 0));

        // The video does not have the length the metadata say: refused, not read
        SourceVideo wrong;
        wrong.setLenient(true);
        CHECK(wrong.open(ldx, fieldLength, lineLength));
        CHECK(wrong.getVideoField(1).size() == fieldLength);

        // The analogue audio of the capture comes back as it was written
        LdxCapture capture;
        CHECK(capture.open(ldx, error));
        CHECK(capture.numberOfFields() == 3);
        CHECK(capture.openAudio(ldx, error));
        // (each block is where it was written: the first one begins with the first field, at sample 0, the second with the third field, two fields
        // later: 2 x 882 samples at 44100 Hz and 50 fields a second)
        CHECK(capture.audioSampleRate() == 44100.0 && capture.audioBytesPerSample() == 4 && capture.audioSamples() == 2 * 882 + 2);
        const QVector<qint16> first = capture.getAudio(0, 4);
        CHECK(QByteArray(reinterpret_cast<const char *>(first.constData()), first.size() * 2) == audio.mid(0, 16));
        const QVector<qint16> third = capture.getAudio(2 * 882, 2);
        CHECK(QByteArray(reinterpret_cast<const char *>(third.constData()), third.size() * 2) == audio.mid(16, 8));
    }

    std::printf("testvideoio: ok\n");
    return 0;
}
