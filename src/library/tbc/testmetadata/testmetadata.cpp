/************************************************************************

    testmetadata.cpp

    Unit tests for metadata classes
    Copyright (C) 2022 Adam Sampson
    Copyright (C) 2025 Simon Inns

    This file is part of ld-decode-tools.

    ld-decode-tools is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include <QCoreApplication>
#include <QCommandLineParser>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "lddecodemetadata.h"

// Run unit tests for VideoSystem
void testVideoSystem() {
    std::cerr << "Testing VideoSystem\n";

    VideoSystem system;
    bool b;

    b = parseVideoSystemName("PAL", system);
    assert(b);
    assert(system == PAL);
    b = parseVideoSystemName("NTSC", system);
    assert(b);
    assert(system == NTSC);
    b = parseVideoSystemName("PAL_M", system);
    assert(b);
    assert(system == PAL_M);
    b = parseVideoSystemName("", system);
    assert(!b);
    b = parseVideoSystemName("SPURIOUS", system);
    assert(!b);
}

// Metadata of fields that arrive one by one, as from a pipe: one field as JSON and back, and the provider of ensureFields
void testFieldProvider() {
    std::cerr << "Testing field metadata that comes field by field\n";

    LdDecodeMetaData::Field sent;
    sent.seqNo = 3;
    sent.isFirstField = false;
    sent.syncConf = 77;
    sent.fieldPhaseID = 2;
    sent.medianBurstIRE = 20.5;
    const QByteArray json = LdDecodeMetaData::fieldToJson(sent);
    LdDecodeMetaData::Field got;
    if (!LdDecodeMetaData::fieldFromJson(json, got) || got.seqNo != 3 || got.syncConf != 77 || got.fieldPhaseID != 2
        || got.isFirstField || got.medianBurstIRE != 20.5) {
        std::cerr << "FAIL: a field does not come back from its JSON\n";
        std::exit(1);
    }
    LdDecodeMetaData::Field bad;
    if (LdDecodeMetaData::fieldFromJson(QByteArray("{not json"), bad)) {
        std::cerr << "FAIL: JSON that is not a field was taken\n";
        std::exit(1);
    }

    LdDecodeMetaData metaData;
    metaData.initialiseDefault(PAL);
    metaData.setFieldProvider([&](qint32 seqNo, LdDecodeMetaData::Field &field) {
        if (seqNo != 3) return false;           // only the third field has metadata
        field = sent;
        return true;
    });
    metaData.ensureFields(4);
    if (metaData.getNumberOfFields() != 4 || metaData.getField(3).syncConf != 77 || metaData.getField(3).fieldPhaseID != 2
        || metaData.getField(2).syncConf == 77 || metaData.getField(4).seqNo != 4) {
        std::cerr << "FAIL: ensureFields did not take the fields from the provider\n";
        std::exit(1);
    }
}

// (assert is removed by NDEBUG in a release build, and the test would then check nothing)
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

// Run unit tests for the frame rate and the CLV time codes
void testFrameRateAndTimecodes() {
    std::cerr << "Testing frame rates and CLV time codes\n";

    // The time of the video: 25, or 30000 / 1001 for the two 525-line systems
    CHECK(frameRateNumerator(PAL) == 25 && frameRateDenominator(PAL) == 1);
    CHECK(frameRateNumerator(NTSC) == 30000 && frameRateDenominator(NTSC) == 1001);
    CHECK(frameRateNumerator(PAL_M) == 30000 && frameRateDenominator(PAL_M) == 1001);
    CHECK(videoFrameRate(PAL) == 25.0);
    CHECK(std::fabs(videoFrameRate(NTSC) - 29.97002997) < 1e-8);
    CHECK(videoFrameRate(PAL_M) == videoFrameRate(NTSC));

    // What a time code counts in a second: the round figure
    CHECK(timecodeFramesPerSecond(PAL) == 25);
    CHECK(timecodeFramesPerSecond(NTSC) == 30);
    CHECK(timecodeFramesPerSecond(PAL_M) == 30);

    // A time code to a frame number and back
    const struct { VideoSystem system; qint32 fps; } systems[] = { { PAL, 25 }, { NTSC, 30 }, { PAL_M, 30 } };
    for (const auto &entry : systems) {
        LdDecodeMetaData metaData;
        metaData.initialiseDefault(entry.system);

        LdDecodeMetaData::ClvTimecode time;
        time.hours = 1;
        time.minutes = 23;
        time.seconds = 45;
        time.pictureNumber = 6;
        const qint32 expected = ((1 * 60 + 23) * 60 + 45) * entry.fps + 6;
        CHECK(metaData.convertClvTimecodeToFrameNumber(time) == expected);

        const LdDecodeMetaData::ClvTimecode back = metaData.convertFrameNumberToClvTimecode(expected);
        CHECK(back.hours == 1 && back.minutes == 23 && back.seconds == 45 && back.pictureNumber == 6);

        // The first frame, and a time code with a part that is missing
        time.hours = time.minutes = time.seconds = time.pictureNumber = 0;
        CHECK(metaData.convertClvTimecodeToFrameNumber(time) == 0);
        time.minutes = -1;
        CHECK(metaData.convertClvTimecodeToFrameNumber(time) == -1);
    }
}

// Run unit tests for the accessors of the fields
void testFieldAccessors() {
    std::cerr << "Testing the accessors of the fields\n";

    LdDecodeMetaData metaData;
    metaData.initialiseDefault(NTSC);
    metaData.ensureFields(4);

    // Each kind of data of a field is kept for the field it was given for, and not for the others
    LdDecodeMetaData::VitsMetrics vits;
    vits.inUse = true;
    vits.wSNR = 36.5;
    vits.bPSNR = 41.5;
    metaData.updateFieldVitsMetrics(vits, 3);
    CHECK(metaData.getFieldVitsMetrics(3).wSNR == 36.5 && metaData.getFieldVitsMetrics(3).bPSNR == 41.5);
    CHECK(!metaData.getFieldVitsMetrics(2).inUse && !metaData.getFieldVitsMetrics(4).inUse);

    LdDecodeMetaData::Vbi vbi;
    vbi.inUse = true;
    vbi.vbiData = { 11, 22, 33 };
    metaData.updateFieldVbi(vbi, 2);
    CHECK(metaData.getFieldVbi(2).vbiData[1] == 22 && !metaData.getFieldVbi(1).inUse);

    LdDecodeMetaData::Ntsc ntsc;
    ntsc.inUse = true;
    ntsc.fmCodeData = 77;
    metaData.updateFieldNtsc(ntsc, 4);
    CHECK(metaData.getFieldNtsc(4).fmCodeData == 77 && !metaData.getFieldNtsc(3).inUse);

    LdDecodeMetaData::Vitc vitc;
    vitc.inUse = true;
    vitc.vitcData = { 1, 2, 3, 4, 5, 6, 7, 8 };
    metaData.updateFieldVitc(vitc, 1);
    CHECK(metaData.getFieldVitc(1).vitcData[7] == 8 && !metaData.getFieldVitc(2).inUse);

    LdDecodeMetaData::ClosedCaption caption;
    caption.inUse = true;
    caption.data0 = 4;
    caption.data1 = 5;
    metaData.updateFieldClosedCaption(caption, 2);
    CHECK(metaData.getFieldClosedCaption(2).data1 == 5 && !metaData.getFieldClosedCaption(1).inUse);

    DropOuts dropOuts;
    dropOuts.append(1, 2, 3);
    metaData.updateFieldDropOuts(dropOuts, 4);
    CHECK(metaData.getFieldDropOuts(4).size() == 1 && metaData.getFieldDropOuts(3).size() == 0);
    metaData.clearFieldDropOuts(4);
    CHECK(metaData.getFieldDropOuts(4).size() == 0);

    // A whole field
    LdDecodeMetaData::Field field = metaData.getField(2);
    field.pad = true;
    field.fieldPhaseID = 3;
    metaData.updateField(field, 2);
    CHECK(metaData.getField(2).pad && metaData.getField(2).fieldPhaseID == 3 && !metaData.getField(1).pad);
}

int main(int argc, char *argv[])
{
    // Initialise Qt
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("testmetadata");
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("testmetadata - unit tests for ld-decode's metadata library");
    parser.addHelpOption();
    parser.addVersionOption();

    // Test options
    QCommandLineOption statsOption(QStringList() << "s" << "stats",
                                   "parse all fields and show statistics");
    parser.addOption(statsOption);
    QCommandLineOption exitOption(QStringList() << "x" << "exit",
                                  "call exit(0) after parsing, to analyse memory usage");
    parser.addOption(exitOption);

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", "Input SQLite file (omit to run unit tests)");

    // Positional argument to specify output video file
    parser.addPositionalArgument("output", "Output SQLite file (omit to only read input)");

    // Parse the command line
    parser.process(app);

    // Process the positional args
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 0) {
        // Run unit tests
        testVideoSystem();
        testFieldProvider();
        testFrameRateAndTimecodes();
        testFieldAccessors();
        return 0;
    }
    if (positionalArguments.count() > 2) {
        qCritical("You may specify one input file and (optionally) one output file");
        return 1;
    }

    // Read the input file
    LdDecodeMetaData metaData;
    if (!metaData.read(positionalArguments.at(0))) {
        qCritical("Unable to read input file");
        return 1;
    }

    // Show statistics
    if (parser.isSet(statsOption)) {
        // These are not very useful, but they ensure that LdDecodeMetaData has
        // actually fully parsed all of the metadata.

        qint32 numFields = metaData.getNumberOfFields();
        qint32 numMetrics = 0;
        double meanWSNR = 0.0;
        qint32 numDropOuts = 0;

        for (qint32 i = 1; i <= numFields; i++) {
            const LdDecodeMetaData::Field &field = metaData.getField(i);

            if (field.vitsMetrics.inUse) {
                ++numMetrics;
                meanWSNR += field.vitsMetrics.wSNR;
            }

            numDropOuts += field.dropOuts.size();
        }

        if (numMetrics > 0) meanWSNR /= numMetrics;

        std::cout << "fields=" << numFields << " metrics=" << numMetrics << " wSNR=" << meanWSNR << " dropouts=" << numDropOuts << "\n";
    }

    // Force an exit if requested
    if (parser.isSet(exitOption)) {
        // We use the C exit function so that C++ destructors don't get called.
        // We can then use a memory debugger like valgrind to analyse the
        // memory that was "leaked", and see how much the parser allocated.
        exit(0);
    }

    // Write the output file, if given
    if (positionalArguments.count() == 2) {
        if (!metaData.write(positionalArguments.at(1))) {
            qCritical("Unable to write output file");
            return 1;
        }
    }

    return 0;
}