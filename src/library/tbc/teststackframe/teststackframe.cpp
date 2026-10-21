/******************************************************************************
 * teststackframe.cpp
 * ld-decode-tools - unit tests for StackFrame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#include "stackframe.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static constexpr qint32 WIDTH = 16;
static constexpr qint32 HEIGHT = 6;

// A small capture of 2 frames (4 fields): every sample of a first field is `value`, every sample of a second field `value + 10`, and the file can hold
// fewer fields than the metadata say
struct TestSource {
    LdDecodeMetaData metadata;
    SourceVideo video;
};

static void makeSource(TestSource &source, const QString &fileName, quint16 value, qint32 fieldsInFile = 4, qint32 width = WIDTH, qint32 height = HEIGHT)
{
    QFile file(fileName);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 field = 0; field < fieldsInFile; field++) {
        QVector<quint16> samples(width * height, static_cast<quint16>(value + (field % 2) * 10));
        file.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
    }
    file.close();

    source.metadata.initialiseDefault(PAL);
    LdDecodeMetaData::VideoParameters parameters = source.metadata.getVideoParameters();
    parameters.fieldWidth = width;
    parameters.fieldHeight = height;
    source.metadata.setVideoParameters(parameters);
    source.metadata.ensureFields(4);
    CHECK(source.video.open(fileName, width * height, width));
}

static StackFrame::Input input(TestSource &source, qint32 frame)
{
    StackFrame::Input in;
    in.metadata = &source.metadata;
    in.video = &source.video;
    in.frame = frame;
    return in;
}

static void testFrames()
{
    std::cerr << "Testing StackFrame::ownFrame and isPadded\n";

    // A source that begins 2 frames later has its first frame on the frame 3 of the timeline
    CHECK(StackFrame::ownFrame(3, 2, 10) == 1);
    CHECK(StackFrame::ownFrame(2, 2, 10) == -1);
    CHECK(StackFrame::ownFrame(12, 2, 10) == 10);
    CHECK(StackFrame::ownFrame(13, 2, 10) == -1);
    CHECK(StackFrame::ownFrame(1, -3, 10) == 4);
    CHECK(StackFrame::ownFrame(7, 0, 10) == 7);

    QTemporaryDir directory;
    TestSource source;
    makeSource(source, directory.filePath("a.tbc"), 100);
    CHECK(!StackFrame::isPadded(source.metadata, 1));
    CHECK(!StackFrame::isPadded(source.metadata, 2));
    LdDecodeMetaData::Field field = source.metadata.getField(2);
    field.pad = true;
    source.metadata.updateField(field, 2);
    CHECK(StackFrame::isPadded(source.metadata, 1));
    CHECK(!StackFrame::isPadded(source.metadata, 2));
}

static void testReadAndStack()
{
    std::cerr << "Testing StackFrame::read and StackFrame::stack\n";

    QTemporaryDir directory;
    TestSource a, b, c;
    makeSource(a, directory.filePath("a.tbc"), 1000);
    makeSource(b, directory.filePath("b.tbc"), 2000);
    makeSource(c, directory.filePath("c.tbc"), 6000);

    StackerCore core;
    StackFrame::Fields fields;
    StackFrame::Result result;

    // All three give frame 1: the mean of the first fields is (1000 + 2000 + 6000) / 3, of the second fields 10 more
    QVector<StackFrame::Input> inputs = { input(a, 1), input(b, 1), input(c, 1) };
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 0, 1, 2 }));
    CHECK(fields.videoParameters.fieldWidth == WIDTH && fields.videoParameters.fieldHeight == HEIGHT);
    CHECK(fields.first[1].size() == WIDTH * HEIGHT && fields.second[1].size() == WIDTH * HEIGHT);
    StackFrame::stack(core, fields, StackFrame::BothFields, 1, 0, 15, true, false, result);
    CHECK(result.first.size() == WIDTH * HEIGHT && result.second.size() == WIDTH * HEIGHT);
    CHECK(result.first[0] == 3000 && result.first[WIDTH * HEIGHT - 1] == 3000);
    CHECK(result.second[0] == 3010 && result.second[WIDTH * HEIGHT - 1] == 3010);

    // The second frame is on the fields 3 and 4 of each
    CHECK(StackFrame::read({ input(a, 2), input(b, 2), input(c, 2) }, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable.size() == 3);

    // A source that has no frame here, or none that is not padded, is left out: the mean of 1000 and 6000
    LdDecodeMetaData::Field padded = b.metadata.getField(1);
    padded.pad = true;
    b.metadata.updateField(padded, 1);
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 0, 2 }));
    CHECK(fields.first[1].isEmpty());
    result = StackFrame::Result();
    StackFrame::stack(core, fields, StackFrame::BothFields, 1, 0, 15, true, false, result);
    CHECK(result.first[0] == 3500);
    inputs[2].frame = -1;
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 0 }));

    // None can be used
    inputs[0].frame = -1;
    CHECK(!StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable.isEmpty());

    // The fields stacked together are those stacked one at a time (the preview of the GUI reads both, a session one)
    {
        StackFrame::Fields both, onlyFirst, onlySecond;
        StackFrame::Result together, firstAlone, secondAlone;
        CHECK(StackFrame::read({ input(a, 1), input(c, 1) }, StackFrame::BothFields, -1, both));
        CHECK(StackFrame::read({ input(a, 1), input(c, 1) }, StackFrame::FirstField, -1, onlyFirst));
        CHECK(StackFrame::read({ input(a, 1), input(c, 1) }, StackFrame::SecondField, -1, onlySecond));
        StackFrame::stack(core, both, StackFrame::BothFields, 1, 2, 15, true, false, together);
        StackFrame::stack(core, onlyFirst, StackFrame::FirstField, 1, 2, 15, true, false, firstAlone);
        StackFrame::stack(core, onlySecond, StackFrame::SecondField, 1, 2, 15, true, false, secondAlone);
        CHECK(together.first == firstAlone.first && together.second == secondAlone.second);
    }

    // Only one of the fields, and only the first lines
    inputs = { input(a, 1), input(c, 1) };
    CHECK(StackFrame::read(inputs, StackFrame::SecondField, -1, fields));
    CHECK(fields.first[0].isEmpty() && fields.second[0].size() == WIDTH * HEIGHT);
    result = StackFrame::Result();
    StackFrame::stack(core, fields, StackFrame::SecondField, 1, 0, 15, true, false, result);
    CHECK(result.first.isEmpty() && result.second.size() == WIDTH * HEIGHT && result.second[0] == 3510);
    CHECK(StackFrame::read(inputs, StackFrame::FirstField, 2, fields));
    CHECK(fields.first[0].size() == WIDTH * 2 && fields.videoParameters.fieldHeight == 2);
    result = StackFrame::Result();
    StackFrame::stack(core, fields, StackFrame::FirstField, 1, 0, 15, true, false, result);
    CHECK(result.first.size() == WIDTH * 2 && result.first[0] == 3500);
}

static void testUnusable()
{
    std::cerr << "Testing StackFrame::read with sources that cannot be used\n";

    QTemporaryDir directory;
    TestSource a, shortFile, otherSize, noVideo;
    makeSource(a, directory.filePath("a.tbc"), 1000);
    // the metadata say 4 fields, the file holds 2: the second frame is not in it
    makeSource(shortFile, directory.filePath("s.tbc"), 2000, 2);
    // another size
    makeSource(otherSize, directory.filePath("o.tbc"), 3000, 4, WIDTH + 2, HEIGHT);
    makeSource(noVideo, directory.filePath("n.tbc"), 4000);

    StackFrame::Fields fields;
    QVector<StackFrame::Input> inputs = { input(a, 2), input(shortFile, 2), input(otherSize, 2), input(noVideo, 2) };
    inputs[3].video = nullptr;
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 0 }));

    // frame 1 is in the short file
    inputs[1].frame = 1;
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 0, 1 }));
}

static void testParameters()
{
    std::cerr << "Testing the video parameters of StackFrame::read\n";

    QTemporaryDir directory;
    TestSource a, b;
    makeSource(a, directory.filePath("a.tbc"), 1000);
    makeSource(b, directory.filePath("b.tbc"), 2000);
    LdDecodeMetaData::VideoParameters parameters = a.metadata.getVideoParameters();
    parameters.black16bIre = 1111;
    a.metadata.setVideoParameters(parameters);
    parameters.black16bIre = 2222;
    b.metadata.setVideoParameters(parameters);

    // The first source gives them, even when it has no frame here
    StackFrame::Fields fields;
    QVector<StackFrame::Input> inputs = { input(a, 1), input(b, 1) };
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.videoParameters.black16bIre == 1111);
    inputs[0].frame = -1;
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.usable == QVector<qint32>({ 1 }));
    CHECK(fields.videoParameters.black16bIre == 1111);
    // a source that is not there at all: the first that is used
    inputs[0].metadata = nullptr;
    CHECK(StackFrame::read(inputs, StackFrame::BothFields, -1, fields));
    CHECK(fields.videoParameters.black16bIre == 2222);
}

static void setPhases(LdDecodeMetaData &metadata, qint32 fields, qint32 padded, qint32 pivotPhase)
{
    metadata.ensureFields(fields);
    for (qint32 n = 1; n <= fields; n++) {
        LdDecodeMetaData::Field field = metadata.getField(n);
        field.pad = n <= padded;
        field.fieldPhaseID = n == padded + 1 ? pivotPhase : 0;
        metadata.updateField(field, n);
    }
}

static void testPhases()
{
    std::cerr << "Testing StackFrame::repairFieldPhases\n";

    // The first 3 fields are padded and the field 4 has the phase 5: the others follow from it, in 8 for PAL and in 4 for NTSC and PAL-M
    {
        LdDecodeMetaData metadata;
        metadata.initialiseDefault(PAL);
        setPhases(metadata, 20, 3, 5);
        StackFrame::repairFieldPhases(metadata);
        CHECK(metadata.getField(4).fieldPhaseID == 5);
        CHECK(metadata.getField(1).fieldPhaseID == 2 && metadata.getField(3).fieldPhaseID == 4);
        CHECK(metadata.getField(8).fieldPhaseID == 1 && metadata.getField(9).fieldPhaseID == 2);
        CHECK(metadata.getField(20).fieldPhaseID == 5);
    }
    for (VideoSystem system: { NTSC, PAL_M }) {
        LdDecodeMetaData metadata;
        metadata.initialiseDefault(system);
        setPhases(metadata, 20, 3, 3);
        StackFrame::repairFieldPhases(metadata);
        CHECK(metadata.getField(4).fieldPhaseID == 3);
        CHECK(metadata.getField(1).fieldPhaseID == 4 && metadata.getField(2).fieldPhaseID == 1);
        CHECK(metadata.getField(5).fieldPhaseID == 4 && metadata.getField(6).fieldPhaseID == 1);
        CHECK(metadata.getField(20).fieldPhaseID == 3);
    }

    // Nothing but padded fields, or the last one the only that is not: left as it is
    {
        LdDecodeMetaData metadata;
        metadata.initialiseDefault(NTSC);
        setPhases(metadata, 6, 6, 2);
        StackFrame::repairFieldPhases(metadata);
        CHECK(metadata.getField(1).fieldPhaseID == 0 && metadata.getField(6).fieldPhaseID == 0);
        setPhases(metadata, 6, 5, 2);
        StackFrame::repairFieldPhases(metadata);
        CHECK(metadata.getField(6).fieldPhaseID == 2 && metadata.getField(1).fieldPhaseID == 0);
    }
}

int main()
{
    testFrames();
    testReadAndStack();
    testUnusable();
    testParameters();
    testPhases();
    std::cerr << "All tests passed\n";
    return 0;
}
