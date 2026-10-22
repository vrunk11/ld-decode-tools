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

        // The same dropouts. They come back in the reverse of the order of the file (by start), as they always did: nobody depends on it, it is a point for the audit
        CHECK(field.dropOuts.size() == expected.dropOuts.size());
        for (qint32 d = 0; d < expected.dropOuts.size(); d++) {
            const qint32 e = expected.dropOuts.size() - 1 - d;
            CHECK(field.dropOuts.startx(d) == expected.dropOuts.startx(e));
            CHECK(field.dropOuts.endx(d) == expected.dropOuts.endx(e));
            CHECK(field.dropOuts.fieldLine(d) == expected.dropOuts.fieldLine(e));
        }
    }

    std::cerr << "All tests passed\n";
    return 0;
}
