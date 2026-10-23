/******************************************************************************
 * testmappedvideo.cpp
 * ld-decode-tools TBC library - unit test of a capture read through its map
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

#include "framemap.h"
#include "lddecodemetadata.h"
#include "fieldaudio.h"
#include "mappedmetadata.h"
#include "sourcevideo.h"
#include "stackedvideo.h"
#include "stackproject.h"

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

static MapFrame frame(qint32 disc, qint32 capture, MapBasis basis, bool unusable = false)
{
    MapFrame value;
    value.discFrame = disc;
    value.captureFrame = capture;
    value.basis = basis;
    value.unusable = unusable;
    return value;
}

static bool loaded2NotSet(const QString &file)
{
    StackProject project;
    QString error;
    return project.load(file, error) && !project.stacking.copyStreamsSet;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString fileName = directory.filePath("capture.tbc");

    // A capture of 6 frames: every sample of the field n (from 1) is 100 + n
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
        field.syncConf = 50 + n;
        capture.updateField(field, n);
    }

    // The view: the frames 1 and 2 of the capture, a padded frame, the frames 5 and 6, then the frame 3 that could not be numbered
    QVector<MapFrame> frames = { frame(10, 1, MapBasis::Measured), frame(11, 2, MapBasis::Measured), frame(12, 0, MapBasis::Padded),
                                 frame(13, 5, MapBasis::Sequence), frame(14, 6, MapBasis::Measured), frame(15, 3, MapBasis::Guessed, true) };
    FrameMap map = FrameMap::fromFrames(frames);
    map.discType = "CAV";
    map.captureSize = QFileInfo(fileName).size();
    map.captureFields = FRAMES * 2;

    // The video
    {
        SourceVideo video;
        CHECK(video.openMapped(fileName, WIDTH * HEIGHT, WIDTH, map, capture));
        CHECK(video.getNumberOfAvailableFields() == 12);
        const qint32 expected[12] = { 101, 102, 103, 104, 0, 0, 109, 110, 111, 112, 105, 106 };
        for (qint32 n = 1; n <= 12; n++) {
            const SourceVideo::Data data = video.getVideoField(n);
            CHECK(data.size() == WIDTH * HEIGHT);
            for (quint16 sample : data) CHECK(sample == expected[n - 1]);
        }
        // Some of the lines of a field, of a field of the capture and of a padded one
        const SourceVideo::Data lines = video.getVideoField(7, 2, 3);
        CHECK(lines.size() == 2 * WIDTH && lines[0] == 109);
        const SourceVideo::Data padded = video.getVideoField(5, 1, 4);
        CHECK(padded.size() == 4 * WIDTH && padded[0] == 0);
        video.close();
    }

    // The metadata
    {
        LdDecodeMetaData view;
        QString error;
        CHECK(MappedMetadata::build(capture, map, 882, view, error));
        CHECK(view.getNumberOfFields() == 12);
        CHECK(view.getVideoParameters().isMapped);
        CHECK(view.getField(1).syncConf == 51 && view.getField(2).syncConf == 52);                // frame 1 of the capture
        CHECK(view.getField(5).pad && view.getField(6).pad && !view.getField(4).pad);              // the padded frame
        CHECK(view.getField(5).audioSamples == 882);
        CHECK(view.getField(7).syncConf == 59 && view.getField(8).syncConf == 60);                // frame 5 of the capture
        // The number of the disc is in the VBI of the first field of each frame, padded or not
        CHECK(view.getField(1).vbi.vbiData[1] == MappedMetadata::frameToVbi(10));
        CHECK(view.getField(3).vbi.vbiData[1] == MappedMetadata::frameToVbi(11));
        CHECK(view.getField(5).vbi.vbiData[1] == MappedMetadata::frameToVbi(12));
        CHECK(view.getField(11).vbi.vbiData[1] == MappedMetadata::frameToVbi(15));
        // The frame that could not be numbered has a dropout over every line of its two fields
        CHECK(view.getField(11).dropOuts.size() == HEIGHT && view.getField(12).dropOuts.size() == HEIGHT);
        CHECK(view.getField(1).dropOuts.size() == 0);
        CHECK(view.getField(7).dropOuts.size() == 0);                                             // (a number deduced from the sequence is kept)

        // Measured only: the frame whose number came from the sequence is left out as well, the measured ones are not
        LdDecodeMetaData measured;
        CHECK(MappedMetadata::build(capture, map, 882, measured, error, true));
        CHECK(measured.getField(7).dropOuts.size() == HEIGHT && measured.getField(8).dropOuts.size() == HEIGHT);
        CHECK(measured.getField(1).dropOuts.size() == 0 && measured.getField(3).dropOuts.size() == 0 && measured.getField(9).dropOuts.size() == 0);

        // A map that asks for a frame the capture does not have is refused
        FrameMap bad = FrameMap::fromFrames({ frame(10, 7, MapBasis::Measured) });
        LdDecodeMetaData none;
        CHECK(!MappedMetadata::build(capture, bad, 882, none, error));
        SourceVideo nothing;
        CHECK(!nothing.openMapped(fileName, WIDTH * HEIGHT, WIDTH, bad, capture));
    }

    // A session keeps the map of a source, and gives it back; a source that does not follow its map is the capture
    {
        StackProject project;
        StackSource source;
        source.id = "a";
        source.file = fileName;
        source.map = map;
        source.hasMap = true;
        project.sources.append(source);
        const QString sessionFile = directory.filePath("session.stack");
        QString error;
        CHECK(project.save(sessionFile, error));

        StackProject loaded;
        CHECK(loaded.load(sessionFile, error));
        CHECK(loaded.sources.size() == 1 && loaded.sources[0].hasMap && loaded.sources[0].map == map);
        CHECK(loaded.sources[0].activeMap() != nullptr);
        loaded.sources[0].followMap = false;
        CHECK(loaded.sources[0].activeMap() == nullptr);
        // (a measure of the source is not the one of the capture without the map)
        StackSource plain = loaded.sources[0];
        plain.followMap = true;
        plain.hasMap = false;
        
        StackSource withMap = loaded.sources[0];
        withMap.followMap = true;
        CHECK(withMap.measureKey() != plain.measureKey());
    }

    // The analogue audio of the fields, plain and through the map: the audio of a field of the view is that of the field of the capture it is, a padded frame is silence
    {
        QString error;
        LdDecodeMetaData::PcmAudioParameters pcm;
        pcm.bits = 16;
        pcm.isSigned = true;
        pcm.isLittleEndian = true;
        pcm.sampleRate = 44100;
        capture.setPcmAudioParameters(pcm);
        for (qint32 n = 1; n <= FRAMES * 2; n++) {
            LdDecodeMetaData::Field field = capture.getField(n);
            field.audioSamples = 10;
            capture.updateField(field, n);
        }
        // 12 fields of 10 samples; the left sample is the number of the sample, the right one is minus that
        {
            QFile file(directory.filePath("capture.pcm"));
            CHECK(file.open(QIODevice::WriteOnly));
            for (qint32 i = 0; i < FRAMES * 2 * 10; i++) {
                const qint16 pair[2] = { static_cast<qint16>(i), static_cast<qint16>(-i) };
                file.write(reinterpret_cast<const char *>(pair), 4);
            }
        }
        CHECK(QFile::exists(fileName));
        // (the places of the audio of the fields are worked out when the metadata are read)
        capture.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        CHECK(capture.write(fileName + ".db"));
        LdDecodeMetaData audioCapture;
        CHECK(audioCapture.read(fileName + ".db"));

        FieldAudio plain;
        CHECK(plain.open(fileName, FieldAudio::rangesOf(audioCapture)));
        QByteArray third = plain.audioOf(3);
        CHECK(third.size() == 40 && reinterpret_cast<const qint16 *>(third.constData())[0] == 20);

        const auto table = MappedMetadata::viewFields(audioCapture, map, error);
        CHECK(table);
        FieldAudio view;
        CHECK(view.open(fileName, FieldAudio::rangesOfView(audioCapture, *table, 7)));
        // The view field 7 is the capture field 9 (samples 80...); the padded frame (view fields 5 and 6) is silence of 7 samples
        const QByteArray seventh = view.audioOf(7);
        CHECK(seventh.size() == 40 && reinterpret_cast<const qint16 *>(seventh.constData())[0] == 80 && reinterpret_cast<const qint16 *>(seventh.constData())[1] == -80);
        const QByteArray padded = view.audioOf(5);
        CHECK(padded.size() == 28);
        for (char byte : padded) CHECK(byte == 0);
        CHECK(view.audioOf(99).isEmpty() && view.audioOf(0).isEmpty());
    }

    // A session with one source on gives the metadata of that source (its dropouts and VITS), with two it gives those of a stack (neither)
    {
        for (qint32 n = 1; n <= FRAMES * 2; n++) {
            LdDecodeMetaData::Field field = capture.getField(n);
            field.vitsMetrics.inUse = true;
            field.vitsMetrics.wSNR = 40.0 + n;
            field.vitsMetrics.bPSNR = 30.0 + n;
            field.dropOuts = DropOuts();
            field.dropOuts.append(1, 5, 2);
            capture.updateField(field, n);
        }
        capture.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        CHECK(capture.write(fileName + ".db"));

        StackProject project;
        StackSource source;
        source.id = "a";
        source.file = fileName;
        source.map = map;
        source.hasMap = true;
        project.sources.append(source);
        const QString sessionFile = directory.filePath("one.stack");
        QString error;
        CHECK(project.save(sessionFile, error));

        LdDecodeMetaData one;
        CHECK(StackedVideo::readMetadata(sessionFile, one, error));
        CHECK(one.getNumberOfFields() == 12);
        CHECK(one.getField(1).dropOuts.size() == 1 && one.getField(1).vitsMetrics.inUse && one.getField(1).vitsMetrics.wSNR == 41.0);
        CHECK(one.getField(11).dropOuts.size() == HEIGHT);                   // (the frame that could not be numbered: a dropout over every line)

        StackSource second = source;
        second.id = "b";
        project.sources.append(second);
        const QString twoFile = directory.filePath("two.stack");
        CHECK(project.save(twoFile, error));
        LdDecodeMetaData two;
        CHECK(StackedVideo::readMetadata(twoFile, two, error));
        CHECK(two.getField(1).dropOuts.size() == 0 && !two.getField(1).vitsMetrics.inUse);

        // The streams to copy that the session chose are kept (none chosen is not the same as not chosen)
        {
            StackProject chosen = project;
            CHECK(!chosen.stacking.copyStreamsSet);
            chosen.stacking.copyStreamsSet = true;
            chosen.stacking.copyStreams = { { "b", 4 }, { "a", 7 } };
            const QString chosenFile = directory.filePath("chosen.stack");
            CHECK(chosen.save(chosenFile, error));
            StackProject back;
            CHECK(back.load(chosenFile, error) && back.stacking.copyStreamsSet && back.stacking.copyStreams == chosen.stacking.copyStreams);
            chosen.stacking.copyStreams.clear();
            CHECK(chosen.save(chosenFile, error) && back.load(chosenFile, error) && back.stacking.copyStreamsSet && back.stacking.copyStreams.isEmpty());
            CHECK(loaded2NotSet(directory.filePath("two.stack")));
        }

        // The capture of a session, for the tools that work on a capture
        QString captureFile;
        CHECK(StackProject::singleCapture(sessionFile, captureFile, error) && captureFile == fileName);
        CHECK(!StackProject::singleCapture(twoFile, captureFile, error) && error.contains("2 sources"));
    }

    std::printf("testmappedvideo: ok\n");
    return 0;
}
