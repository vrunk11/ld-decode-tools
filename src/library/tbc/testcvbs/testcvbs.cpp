/******************************************************************************
 * testcvbs.cpp
 * ld-decode-tools TBC library - unit tests for the CVBS file format support
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QVector>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "cvbs.h"
#include "cvbsaudio.h"

// assert() is compiled out of release builds, so check explicitly
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ << ": check failed: " #condition "\n"; \
            std::exit(1); \
        } \
    } while (0)

static void testGeometry()
{
    std::cerr << "Testing geometry\n";

    // Frame sizes from the CVBS specification (video standard presets)
    CHECK(Cvbs::geometry(PAL).frameSamples == 709379);
    CHECK(Cvbs::geometry(NTSC).frameSamples == 477750);
    CHECK(Cvbs::geometry(PAL_M).frameSamples == 477225);

    // TBC field sizes
    CHECK(Cvbs::geometry(PAL).fieldLength() == 1135 * 313);
    CHECK(Cvbs::geometry(NTSC).fieldLength() == 910 * 263);
    CHECK(Cvbs::geometry(PAL_M).fieldLength() == 909 * 263);

    // PAL's two extra samples end the first field's last line
    const Cvbs::Geometry pal = Cvbs::geometry(PAL);
    CHECK(pal.lineOffset(312) == 312 * 1135);
    CHECK(pal.lineOffset(313) == 313 * 1135 + 2);
    CHECK(pal.lineOffset(624) + 1135 + 2 == pal.frameSamples);

    VideoSystem system;
    CHECK(Cvbs::systemForFieldLength(910 * 263, system) && system == NTSC);
    CHECK(!Cvbs::systemForFieldLength(1234, system));
}

static void testFileNames()
{
    std::cerr << "Testing file names\n";

    CHECK(Cvbs::isDataFileName("a/b.cvbs") && Cvbs::isDataFileName("b.CVBSY") && Cvbs::isDataFileName("b.cvbsc"));
    CHECK(!Cvbs::isDataFileName("b.tbc") && !Cvbs::isDataFileName("b.cvbs.db"));
    CHECK(Cvbs::baseName("dir/x.cvbsy") == "dir/x");
    CHECK(Cvbs::metaFileName("dir/x.cvbs") == "dir/x.meta");
    CHECK(Cvbs::dropoutsFileName("dir/x.cvbsc") == "dir/x.dropouts.meta");
    CHECK(Cvbs::companionFileName("dir/x.cvbsy", false) == "dir/x.cvbs.db");
    CHECK(Cvbs::companionFileName("dir/x.cvbs", true) == "dir/x.cvbs.json");
    CHECK(Cvbs::pairedDataFileName("x.cvbsy") == "x.cvbsc");
    CHECK(Cvbs::pairedDataFileName("x.cvbs").isEmpty());
    CHECK(Cvbs::dataFileForMetadata("x.dropouts.meta").isEmpty());
    CHECK(Cvbs::dataFileForMetadata("x.tbc.db").isEmpty());
}

static void testSampleConversion()
{
    std::cerr << "Testing sample conversion\n";

    for (VideoSystem system: {PAL, NTSC, PAL_M}) {
        for (Cvbs::SampleEncoding encoding: {Cvbs::SampleEncoding::U10_4FSC, Cvbs::SampleEncoding::U16_4FSC,
                                             Cvbs::SampleEncoding::TPG21_4FSC, Cvbs::SampleEncoding::S16_4FSC}) {
            const Cvbs::SampleConverter converter(encoding, system);
            // Every legal 10-bit level survives the trip both ways
            for (qint32 value = 4; value <= 1019; value++) {
                const quint16 tbc = static_cast<quint16>(value * 64);
                CHECK(converter.toTbc(converter.fromTbc(tbc)) == tbc);
            }
        }
    }

    // The encodings' defining formulas, on PAL white (844)
    const quint16 white = 844 * 64;
    CHECK(Cvbs::SampleConverter(Cvbs::SampleEncoding::U10_4FSC, PAL).fromTbc(white) == 844);
    CHECK(static_cast<quint16>(Cvbs::SampleConverter(Cvbs::SampleEncoding::U16_4FSC, PAL).fromTbc(white)) == white);
    CHECK(Cvbs::SampleConverter(Cvbs::SampleEncoding::TPG21_4FSC, PAL).fromTbc(white) == (844 - 508) * 64);
    CHECK(Cvbs::SampleConverter(Cvbs::SampleEncoding::S16_4FSC, PAL).fromTbc(white) == 18816);
    CHECK(Cvbs::SampleConverter(Cvbs::SampleEncoding::S16_4FSC, NTSC).fromTbc(800 * 64) == 17920);

    // U10 headroom below zero cannot be held by an unsigned TBC sample
    CHECK(Cvbs::SampleConverter(Cvbs::SampleEncoding::U10_4FSC, PAL).toTbc(-5) == 0);

    // A level map keeps the headroom below zero in U10
    Cvbs::SampleConverter mapped(Cvbs::SampleEncoding::U10_4FSC, NTSC);
    mapped.setLevelMap(1.0, -10.0 * 64);
    CHECK(mapped.fromTbc(5 * 64) == -5);
}

static void testFrameFields()
{
    std::cerr << "Testing frame and field conversion\n";

    for (VideoSystem system: {PAL, NTSC, PAL_M}) {
        const Cvbs::Geometry geometry = Cvbs::geometry(system);
        const Cvbs::SampleConverter converter(Cvbs::SampleEncoding::U10_4FSC, system);
        const quint16 pad = 256 * 64;

        QVector<quint16> field1(geometry.fieldLength()), field2(geometry.fieldLength());
        quint32 seed = 12345;
        for (qint32 i = 0; i < geometry.fieldLength(); i++) {
            seed = seed * 1103515245 + 12345;
            field1[i] = static_cast<quint16>((4 + (seed >> 8) % 1016) * 64);
            field2[i] = static_cast<quint16>((4 + (seed >> 16) % 1016) * 64);
        }
        // The second field's last TBC line is padding, not part of the frame
        const qint32 field2Samples = geometry.field2Lines * geometry.lineWidth;
        std::fill(field2.begin() + field2Samples, field2.end(), pad);

        QVector<qint16> frame(static_cast<qint32>(geometry.frameSamples));
        Cvbs::fieldsToFrame(field1.constData(), field2.constData(), geometry, converter, frame.data());

        // The second field starts after the first and PAL's extra samples
        CHECK(frame[0] == field1[0] / 64);
        CHECK(frame[static_cast<qint32>(geometry.lineOffset(geometry.field1Lines))] == field2[0] / 64);

        QVector<quint16> back1(geometry.fieldLength()), back2(geometry.fieldLength());
        Cvbs::frameToFields(frame.constData(), geometry, converter, pad, back1.data(), back2.data());
        CHECK(back1 == field1);
        CHECK(back2 == field2);
    }
}

static void testDropouts(const QString &directory)
{
    std::cerr << "Testing the dropout extension\n";

    for (VideoSystem system: {PAL, NTSC}) {
        const Cvbs::Geometry geometry = Cvbs::geometry(system);
        const qint32 numberOfFrames = 3;
        QVector<DropOuts> fields(numberOfFrames * 2);
        fields[0].append(10, 20, 1);                                  // first line
        fields[0].append(geometry.lineWidth - 5, geometry.lineWidth - 1, geometry.field1Lines);  // field 1's last line
        fields[1].append(0, 7, 5);
        fields[3].append(100, 200, geometry.field2Lines);             // field 2's last line
        fields[5].append(30, 40, 50);

        const QString fileName = directory + "/test.dropouts.meta";
        CHECK(Cvbs::writeDropouts(fileName, system, numberOfFrames,
                                  [&](qint32 index) -> const DropOuts & { return fields[index]; }));

        QVector<DropOuts> back(numberOfFrames * 2);
        CHECK(Cvbs::readDropouts(fileName, system, numberOfFrames,
                                 [&](qint32 index, const DropOuts &dropOuts) { back[index] = dropOuts; }));

        for (qint32 i = 0; i < fields.size(); i++) {
            CHECK(back[i].size() == fields[i].size());
            for (qint32 j = 0; j < fields[i].size(); j++) {
                CHECK(back[i].startx(j) == fields[i].startx(j));
                CHECK(back[i].endx(j) == fields[i].endx(j));
                CHECK(back[i].fieldLine(j) == fields[i].fieldLine(j));
            }
        }
    }
}

// count bytes of a WAV file starting at offset within its sample data
static QByteArray wavFileHeadOf(const QString &fileName, qint64 offset, qint32 count)
{
    QFile file(fileName);
    CHECK(file.open(QIODevice::ReadOnly));
    CHECK(file.seek(44 + offset));
    return file.read(count);
}

static void testAudio(const QString &directory)
{
    std::cerr << "Testing the audio copy\n";

    // 1000 stereo samples of 16-bit PCM
    const QString pcmFileName = directory + "/a.pcm";
    QVector<qint16> samples(2000);
    for (qint32 i = 0; i < samples.size(); i++) samples[i] = static_cast<qint16>((i * 977) % 65536 - 32768);
    {
        QFile pcm(pcmFileName);
        CHECK(pcm.open(QIODevice::WriteOnly));
        pcm.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
    }

    // Copy 600 samples from sample 100: the WAV declares the rate and holds 24 bits
    const QString wavFileName = CvbsAudio::wavFileName(directory + "/a", 0);
    CHECK(wavFileName.endsWith("/a_audio_0.wav"));
    CHECK(CvbsAudio::pcmToWav(pcmFileName, 100, 600, wavFileName, 44100) == 600);

    CvbsAudio::WavInfo info;
    CHECK(CvbsAudio::readWavInfo(wavFileName, info));
    CHECK(info.sampleRate == 44100 && info.bits == 24 && info.numberOfSamples == 600);

    // Asking for more than there is copies what there is
    CHECK(CvbsAudio::pcmToWav(pcmFileName, 900, 600, directory + "/b.wav", 48000) == 100);

    // And back to PCM gives the same samples
    const QString backFileName = directory + "/back.pcm";
    CHECK(CvbsAudio::wavToPcm(wavFileName, backFileName) == 600);
    QFile back(backFileName);
    CHECK(back.open(QIODevice::ReadOnly) && back.size() == 2400);
    const QByteArray data = back.readAll();
    CHECK(memcmp(data.constData(), samples.constData() + 200, 2400) == 0);

    // The reader gives 24-bit samples from any position, and silence past the end
    CvbsAudio::WavReader reader;
    CHECK(reader.open(wavFileName));
    CHECK(reader.info().numberOfSamples == 600);
    const QByteArray piece = reader.read(590, 20);
    CHECK(piece.size() == 120);
    CHECK(piece.left(60) == QByteArray(wavFileHeadOf(wavFileName, 590 * 6, 60)));
    CHECK(piece.mid(60) == QByteArray(60, '\0'));

    // The writer fills in the sizes when it closes
    CvbsAudio::WavWriter writer;
    const QString copyFileName = directory + "/copy.wav";
    CHECK(writer.open(copyFileName, 48000));
    CHECK(writer.write(reader.read(0, 300)));
    CHECK(writer.write(reader.read(300, 300)));
    CHECK(writer.close() && writer.numberOfSamples() == 600);
    CvbsAudio::WavInfo copyInfo;
    CHECK(CvbsAudio::readWavInfo(copyFileName, copyInfo));
    CHECK(copyInfo.sampleRate == 48000 && copyInfo.numberOfSamples == 600);
    QFile original(wavFileName);
    QFile copy(copyFileName);
    CHECK(original.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly));
    CHECK(original.readAll().mid(44) == copy.readAll().mid(44));
}

static void testFileMetadata(const QString &directory)
{
    std::cerr << "Testing the core metadata\n";

    Cvbs::FileMetadata written;
    written.system = PAL_M;
    written.sampleEncoding = Cvbs::SampleEncoding::S16_4FSC;
    written.signalStatePreset = "STANDARD_STABLE_LOCKED";
    written.sequenceContinuous = false;
    written.signalType = "yc";
    written.decoder = "vhs-decode";
    written.gitCommit = "abc123";
    written.numberOfFrames = 42;
    written.blackLevel = 240;
    written.captureNotes = "notes";

    const QString fileName = directory + "/test.meta";
    CHECK(written.write(fileName));
    CHECK(Cvbs::isCoreMetadataFile(fileName));

    Cvbs::FileMetadata read;
    CHECK(read.read(fileName));
    CHECK(read.system == PAL_M);
    CHECK(read.sampleEncoding == Cvbs::SampleEncoding::S16_4FSC);
    CHECK(read.isPhaseLocked() && read.isStandardStable());
    CHECK(read.sequenceContinuous.has_value() && !*read.sequenceContinuous);
    CHECK(read.signalType == "yc" && read.decoder == "vhs-decode");
    CHECK(read.gitBranch.isEmpty() && read.gitCommit == "abc123");
    CHECK(read.numberOfFrames == 42);
    CHECK(read.blackLevel.has_value() && *read.blackLevel == 240);
    CHECK(!read.hasNonstandardValues.has_value());
    CHECK(read.captureNotes == "notes");

    // Writing again updates the one row
    written.numberOfFrames = 7;
    CHECK(written.write(fileName));
    CHECK(read.read(fileName) && read.numberOfFrames == 7);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());

    testGeometry();
    testFileNames();
    testSampleConversion();
    testFrameFields();
    testDropouts(directory.path());
    testFileMetadata(directory.path());
    testAudio(directory.path());

    std::cerr << "Tests complete\n";
    return 0;
}
