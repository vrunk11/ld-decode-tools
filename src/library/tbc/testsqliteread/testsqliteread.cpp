/******************************************************************************
 * testsqliteread.cpp
 * ld-decode-tools - unit tests for reading the field metadata of a SQLite file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#include "lddecodemetadata.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

namespace {

// Fields that have each kind of data, or none of it: the number of dropouts, whether the field has VITS metrics, VBI, VITC, closed captions and NTSC data
LdDecodeMetaData::Field makeField(qint32 number)
{
    LdDecodeMetaData::Field field;
    field.seqNo = number;
    field.isFirstField = number % 2 == 1;
    field.syncConf = 90 + number;
    field.medianBurstIRE = 20.5 + number;
    field.fieldPhaseID = 1 + number % 4;
    field.audioSamples = 1000 + number;
    field.pad = number % 5 == 0;
    field.diskLoc = number * 0.5;
    field.fileLoc = 1000000LL * number;
    field.decodeFaults = number % 3;
    field.efmTValues = 100 * number;

    if (number % 2 == 0) {
        field.vitsMetrics.inUse = true;
        field.vitsMetrics.wSNR = 30.25 + number;
        field.vitsMetrics.bPSNR = 28.5 + number;
    }
    if (number % 3 != 0) {
        field.vbi.inUse = true;
        field.vbi.vbiData = { 0x100 + number, 0xF80000 + number, 0x200 + number };
    }
    if (number % 4 == 1) {
        field.vitc.inUse = true;
        for (qint32 i = 0; i < 8; i++) field.vitc.vitcData[i] = number * 10 + i;
    }
    if (number % 6 == 1) {
        field.closedCaption.inUse = true;
        field.closedCaption.data0 = 0x14 + number;
        field.closedCaption.data1 = 0x2c + number;
    }
    if (number % 7 == 2) {
        field.ntsc.inUse = true;
        field.ntsc.isFmCodeDataValid = true;
        field.ntsc.fmCodeData = 12345 + number;
        field.ntsc.fieldFlag = true;
        field.ntsc.isVideoIdDataValid = true;
        field.ntsc.videoIdData = 321 + number;
        field.ntsc.whiteFlag = true;
    }
    // Different numbers of dropouts, none for some fields; the start of each differs (the file is read in the order of the starts)
    for (qint32 d = 0; d < (number * 3) % 7; d++) field.dropOuts.append(40 + d * 25 + number, 50 + d * 25 + number, 10 + d + number);
    return field;
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::cerr << "Testing the reading of the fields of a SQLite file\n";

    const qint32 fields = 40;
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString fileName = directory.filePath("capture.tbc.db");

    LdDecodeMetaData written;
    written.initialiseDefault(NTSC);
    written.ensureFields(fields);
    for (qint32 number = 1; number <= fields; number++) written.updateField(makeField(number), number);
    written.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
    CHECK(written.write(fileName));

    LdDecodeMetaData read;
    CHECK(read.read(fileName));
    CHECK(read.getNumberOfFields() == fields);

    for (qint32 number = 1; number <= fields; number++) {
        const LdDecodeMetaData::Field expected = makeField(number);
        const LdDecodeMetaData::Field &field = read.getField(number);
        CHECK(field.seqNo == number);
        CHECK(field.isFirstField == expected.isFirstField);
        CHECK(field.syncConf == expected.syncConf);
        CHECK(field.medianBurstIRE == expected.medianBurstIRE);
        CHECK(field.fieldPhaseID == expected.fieldPhaseID);
        CHECK(field.audioSamples == expected.audioSamples);
        CHECK(field.pad == expected.pad);
        CHECK(field.diskLoc == expected.diskLoc);
        CHECK(field.fileLoc == expected.fileLoc);
        CHECK(field.decodeFaults == expected.decodeFaults);
        CHECK(field.efmTValues == expected.efmTValues);

        CHECK(field.vitsMetrics.inUse == expected.vitsMetrics.inUse);
        if (expected.vitsMetrics.inUse) {
            CHECK(field.vitsMetrics.wSNR == expected.vitsMetrics.wSNR);
            CHECK(field.vitsMetrics.bPSNR == expected.vitsMetrics.bPSNR);
        }
        CHECK(field.vbi.inUse == expected.vbi.inUse);
        if (expected.vbi.inUse) CHECK(field.vbi.vbiData == expected.vbi.vbiData);
        CHECK(field.vitc.inUse == expected.vitc.inUse);
        if (expected.vitc.inUse) CHECK(field.vitc.vitcData == expected.vitc.vitcData);
        CHECK(field.closedCaption.inUse == expected.closedCaption.inUse);
        if (expected.closedCaption.inUse) {
            CHECK(field.closedCaption.data0 == expected.closedCaption.data0);
            CHECK(field.closedCaption.data1 == expected.closedCaption.data1);
        }
        CHECK(field.ntsc.inUse == expected.ntsc.inUse);
        if (expected.ntsc.inUse) {
            CHECK(field.ntsc.isFmCodeDataValid == expected.ntsc.isFmCodeDataValid);
            CHECK(field.ntsc.fmCodeData == expected.ntsc.fmCodeData);
            CHECK(field.ntsc.fieldFlag == expected.ntsc.fieldFlag);
            CHECK(field.ntsc.isVideoIdDataValid == expected.ntsc.isVideoIdDataValid);
            CHECK(field.ntsc.videoIdData == expected.ntsc.videoIdData);
            CHECK(field.ntsc.whiteFlag == expected.ntsc.whiteFlag);
        }

        // The same dropouts, in the order of the file (by start), as from JSON
        CHECK(field.dropOuts.size() == expected.dropOuts.size());
        for (qint32 d = 0; d < expected.dropOuts.size(); d++) {
            const qint32 e = d;
            CHECK(field.dropOuts.startx(d) == expected.dropOuts.startx(e));
            CHECK(field.dropOuts.endx(d) == expected.dropOuts.endx(e));
            CHECK(field.dropOuts.fieldLine(d) == expected.dropOuts.fieldLine(e));
        }
    }

    // Writing over a file that exists
    {
        const QString updateName = directory.filePath("update.tbc.db");
        LdDecodeMetaData original;
        original.initialiseDefault(NTSC);
        original.ensureFields(fields);
        for (qint32 number = 1; number <= fields; number++) original.updateField(makeField(number), number);
        original.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        LdDecodeMetaData::VideoParameters parameters = original.getVideoParameters();
        parameters.decoder = "vhs-decode";
        original.setVideoParameters(parameters);
        CHECK(original.write(updateName));

        // The decoder that made the capture is kept (it was always written as ld-decode)
        LdDecodeMetaData again;
        CHECK(again.read(updateName));
        CHECK(again.getVideoParameters().decoder == "vhs-decode");
        CHECK(again.write(updateName));
        LdDecodeMetaData third;
        CHECK(third.read(updateName));
        CHECK(third.getVideoParameters().decoder == "vhs-decode");

        // A field with dropouts that has none any more, and VBI that is cleared: the rows of the file go (they stayed)
        qint32 withDropouts = 0;
        for (qint32 number = 1; number <= fields; number++) {
            if (third.getField(number).dropOuts.size() > 0 && withDropouts == 0) withDropouts = number;
        }
        CHECK(withDropouts > 0);
        LdDecodeMetaData::Field cleared = third.getField(withDropouts);
        cleared.dropOuts = DropOuts();
        cleared.vbi.inUse = false;
        third.updateField(cleared, withDropouts);
        CHECK(third.write(updateName));
        LdDecodeMetaData fourth;
        CHECK(fourth.read(updateName));
        CHECK(fourth.getField(withDropouts).dropOuts.size() == 0);
        CHECK(!fourth.getField(withDropouts).vbi.inUse);
        for (qint32 number = 1; number <= fields; number++) {
            if (number == withDropouts) continue;
            CHECK(fourth.getField(number).dropOuts.size() == makeField(number).dropOuts.size());
            CHECK(fourth.getField(number).vbi.inUse == makeField(number).vbi.inUse);
        }

        // Only some of the tables: the others are left as the file has them, whatever the metadata in memory hold
        LdDecodeMetaData changed;
        CHECK(changed.read(updateName));
        for (qint32 number = 1; number <= fields; number++) {
            LdDecodeMetaData::Field field = changed.getField(number);
            field.vitsMetrics.inUse = true;
            field.vitsMetrics.wSNR = 40.5 + number;
            field.vitsMetrics.bPSNR = 30.25 + number;
            field.syncConf = 1;
            field.dropOuts = DropOuts();
            changed.updateField(field, number);
        }
        CHECK(changed.write(updateName, LdDecodeMetaData::VitsTable));
        LdDecodeMetaData partial;
        CHECK(partial.read(updateName));
        for (qint32 number = 1; number <= fields; number++) {
            const LdDecodeMetaData::Field &field = partial.getField(number);
            CHECK(field.vitsMetrics.inUse && field.vitsMetrics.wSNR == 40.5 + number && field.vitsMetrics.bPSNR == 30.25 + number);
            CHECK(field.syncConf == makeField(number).syncConf);                              // (the field record is not written)
            CHECK(field.dropOuts.size() == fourth.getField(number).dropOuts.size());          // (nor the dropouts)
        }
        CHECK(partial.getVideoParameters().decoder == "vhs-decode");
    }

    // The map of a capture kept with its metadata, in a SQLite file and in a JSON file
    {
        MapFrame a, b, c;
        a.discFrame = 100; a.captureFrame = 1; a.basis = MapBasis::Measured;
        b.discFrame = 101; b.captureFrame = 0; b.basis = MapBasis::Padded;
        c.discFrame = 102; c.captureFrame = 2; c.basis = MapBasis::Guessed; c.unusable = true;
        FrameMap map = FrameMap::fromFrames({ a, b, c });
        map.captureFields = 40;
        map.captureSize = 1234567;
        map.discType = "CLV";
        map.reverseFields = true;
        map.madeBy = "test";

        for (const char *name : { "mapped.tbc.db", "mapped.tbc.json" }) {
            const QString mapFile = directory.filePath(name);
            LdDecodeMetaData withMap;
            withMap.initialiseDefault(NTSC);
            withMap.ensureFields(fields);
            for (qint32 number = 1; number <= fields; number++) withMap.updateField(makeField(number), number);
            withMap.setFormat(QString(name).endsWith(".json") ? LdDecodeMetaData::MetadataFormat::Json : LdDecodeMetaData::MetadataFormat::Sqlite);
            CHECK(!withMap.hasDiscMap());
            withMap.setDiscMap(map);
            CHECK(withMap.write(mapFile));

            LdDecodeMetaData back;
            CHECK(back.read(mapFile));
            CHECK(back.hasDiscMap() && back.getDiscMap() == map);
            CHECK(back.getNumberOfFields() == fields);

            // Written again (as every tool that rewrites the metadata does), the map stays; taken away, it goes
            CHECK(back.write(mapFile));
            LdDecodeMetaData again;
            CHECK(again.read(mapFile));
            CHECK(again.hasDiscMap() && again.getDiscMap() == map);
            again.clearDiscMap();
            CHECK(again.write(mapFile));
            LdDecodeMetaData without;
            CHECK(without.read(mapFile));
            CHECK(!without.hasDiscMap());
        }

        // Only some of the tables written over a file that has a map: the map is not touched
        const QString keptFile = directory.filePath("kept.tbc.db");
        LdDecodeMetaData kept;
        kept.initialiseDefault(NTSC);
        kept.ensureFields(fields);
        for (qint32 number = 1; number <= fields; number++) kept.updateField(makeField(number), number);
        kept.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        kept.setDiscMap(map);
        CHECK(kept.write(keptFile));
        LdDecodeMetaData onlyVits;
        CHECK(onlyVits.read(keptFile));
        onlyVits.clearDiscMap();
        CHECK(onlyVits.write(keptFile, LdDecodeMetaData::VitsTable));
        LdDecodeMetaData check;
        CHECK(check.read(keptFile));
        CHECK(check.hasDiscMap() && check.getDiscMap() == map);

        // Saving the map in the metadata of a capture, and taking it away: a SQLite file and a JSON file, the fields untouched
        for (const char *format : { "db", "json" }) {
            const QString videoFile = directory.filePath(QString("saved_%1.tbc").arg(format));
            QFile video(videoFile);
            CHECK(video.open(QIODevice::WriteOnly));
            video.close();
            LdDecodeMetaData plain;
            plain.initialiseDefault(NTSC);
            plain.ensureFields(fields);
            for (qint32 number = 1; number <= fields; number++) plain.updateField(makeField(number), number);
            plain.setFormat(QString(format) == "json" ? LdDecodeMetaData::MetadataFormat::Json : LdDecodeMetaData::MetadataFormat::Sqlite);
            CHECK(plain.write(videoFile + "." + format));

            QString error;
            CHECK(LdDecodeMetaData::saveDiscMap(videoFile, &map, error));
            LdDecodeMetaData saved;
            CHECK(saved.read(LdDecodeMetaData::findMetadataFile(videoFile)));
            CHECK(saved.hasDiscMap() && saved.getDiscMap() == map && saved.getNumberOfFields() == fields);
            CHECK(saved.getField(3).dropOuts.size() == makeField(3).dropOuts.size());
            CHECK(QFileInfo::exists(videoFile + "." + format + ".bup"));

            CHECK(LdDecodeMetaData::saveDiscMap(videoFile, nullptr, error));
            LdDecodeMetaData removed;
            CHECK(removed.read(LdDecodeMetaData::findMetadataFile(videoFile)));
            CHECK(!removed.hasDiscMap() && removed.getNumberOfFields() == fields);
        }
    }

    // The time to write (SQLITE_BENCH=<fields> to measure it): a new file, then the file again in place, as the tools do
    if (const char *benchFields = std::getenv("SQLITE_BENCH")) {
        const qint32 count = std::atoi(benchFields);
        LdDecodeMetaData big;
        big.initialiseDefault(NTSC);
        big.ensureFields(count);
        for (qint32 number = 1; number <= count; number++) big.updateField(makeField(number), number);
        big.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        const QString bigName = directory.filePath("big.tbc.db");
        QElapsedTimer clock;
        clock.start();
        CHECK(big.write(bigName));
        std::cerr << "bench: " << count << " fields, a new file " << clock.elapsed() << " ms\n";
        clock.restart();
        CHECK(big.write(bigName));
        std::cerr << "bench: the file again in place " << clock.elapsed() << " ms\n";
        clock.restart();
        CHECK(big.write(bigName, LdDecodeMetaData::VitsTable));
        std::cerr << "bench: the VITS table alone, in place " << clock.elapsed() << " ms\n";
    }

    std::cerr << "All tests passed\n";
    return 0;
}
