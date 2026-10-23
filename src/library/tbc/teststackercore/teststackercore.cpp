/******************************************************************************
 * teststackercore.cpp
 * ld-decode-tools - unit tests for StackerCore
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "stackercore.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static constexpr qint32 WIDTH = 24;
static constexpr qint32 HEIGHT = 8;

// A sequence of numbers that is always the same (a linear congruential generator), so that the inputs of the tests are
struct Numbers {
    quint32 state = 12345;
    quint32 next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

struct Fnv {
    quint64 hash = 14695981039346656037ull;
    void add(quint64 value)
    {
        for (qint32 i = 0; i < 8; i++) {
            hash ^= (value >> (8 * i)) & 0xFF;
            hash *= 1099511628211ull;
        }
    }
};

// A set of sources: samples that mostly agree with a gradient and some that do not (spikes), and dropouts here and there on each, some of them on
// the same places on all
struct Sources {
    QVector<SourceVideo::Data> fields;
    QVector<LdDecodeMetaData::Field> metadata;
    QVector<qint32> available;
};

static Sources makeSources(qint32 count, quint32 seed)
{
    Numbers numbers;
    numbers.state = seed;
    Sources sources;
    for (qint32 s = 0; s < count; s++) {
        SourceVideo::Data data(WIDTH * HEIGHT);
        for (qint32 y = 0; y < HEIGHT; y++) {
            for (qint32 x = 0; x < WIDTH; x++) {
                quint32 value = 8000 + 900 * static_cast<quint32>(x) + 1500 * static_cast<quint32>(y) + numbers.next() % 400;
                if (numbers.next() % 23 == 0) value = 1000 + numbers.next() % 60000;
                data[y * WIDTH + x] = static_cast<quint16>(value);
            }
        }
        LdDecodeMetaData::Field field;
        for (qint32 d = 0; d < 14; d++) {
            const qint32 line = 1 + static_cast<qint32>(numbers.next() % HEIGHT);
            const qint32 start = static_cast<qint32>(numbers.next() % WIDTH);
            field.dropOuts.append(start, qMin(WIDTH - 1, start + static_cast<qint32>(numbers.next() % 5)), line);
        }
        // the same places on all of them
        field.dropOuts.append(10, 12, 3);
        field.dropOuts.append(6, 20, 5);
        sources.fields.append(data);
        sources.metadata.append(field);
        sources.available.append(s);
    }
    return sources;
}

// A number that stands for the whole result of a stack: the samples, and the dropouts that are left
static quint64 digest(const SourceVideo::Data &output, const DropOuts &dropOuts)
{
    Fnv fnv;
    for (qint32 i = 0; i < output.size(); i++) fnv.add(output[i]);
    fnv.add(static_cast<quint64>(dropOuts.size()));
    for (qint32 i = 0; i < dropOuts.size(); i++) {
        fnv.add(static_cast<quint64>(dropOuts.startx(i)));
        fnv.add(static_cast<quint64>(dropOuts.endx(i)));
        fnv.add(static_cast<quint64>(dropOuts.fieldLine(i)));
    }
    return fnv.hash;
}

// The result of all the combinations for a mode: 1, 2, 3, 4 and 5 sources, with and without the differential dropout detection and the pass-through of the
// dropouts, and two smart thresholds
static quint64 digestOfMode(qint32 mode)
{
    LdDecodeMetaData metadata;
    metadata.initialiseDefault(NTSC);
    LdDecodeMetaData::VideoParameters parameters = metadata.getVideoParameters();
    parameters.fieldWidth = WIDTH;
    parameters.fieldHeight = HEIGHT;
    parameters.colourBurstStart = 4;
    parameters.colourBurstEnd = 8;
    parameters.activeVideoStart = 8;
    parameters.activeVideoEnd = WIDTH - 2;

    Fnv total;
    StackerCore core;
    for (qint32 count: { 1, 2, 3, 4, 5 }) {
        const Sources sources = makeSources(count, 7u + static_cast<quint32>(count));
        for (bool noDiffDod: { false, true }) {
            for (bool passThrough: { false, true }) {
                for (qint32 threshold: { 15 * 256, 40 * 256 }) {
                    SourceVideo::Data output(WIDTH * HEIGHT);
                    DropOuts dropOuts;
                    core.stackField(1, sources.fields, parameters, sources.metadata, sources.available, noDiffDod, passThrough, output, dropOuts, mode, threshold, false);
                    total.add(digest(output, dropOuts));
                }
            }
        }
    }
    return total.hash;
}

static void testModes()
{
    std::cerr << "Testing StackerCore::stackField in every mode\n";

    // The results of the code as it was before it was changed: one number for each mode (mean, median, smart mean, smart neighbour, neighbour)
    // (the number of the smart neighbour mode is that of the code after the centre was made signed: the centres under the threshold, 3840, give another mean)
    const quint64 expected[5] = { 8674266558336975877ull, 2541962126510689597ull, 15763113360523792581ull, 14941868726732689349ull, 9716157817551807533ull };
    bool print = false;
    for (qint32 mode = 0; mode < 5; mode++) {
        const quint64 got = digestOfMode(mode);
        std::cerr << "mode " << mode << ": " << got << "\n";
        if (expected[mode] == 0) print = true;
        else CHECK(got == expected[mode]);
    }
    CHECK(!print);
}

static void testSimple()
{
    std::cerr << "Testing StackerCore with values that can be checked by hand\n";

    LdDecodeMetaData metadata;
    metadata.initialiseDefault(NTSC);
    LdDecodeMetaData::VideoParameters parameters = metadata.getVideoParameters();
    parameters.fieldWidth = 4;
    parameters.fieldHeight = 2;
    parameters.colourBurstStart = 0;
    StackerCore core;

    // 3 sources, one sample bad in each: the median (mode 1), the mean (mode 0) and the mean of the samples near the median (mode 2)
    QVector<SourceVideo::Data> fields = { SourceVideo::Data(8, 1000), SourceVideo::Data(8, 1010), SourceVideo::Data(8, 5000) };
    QVector<LdDecodeMetaData::Field> fieldMetadata(3);
    QVector<qint32> available = { 0, 1, 2 };

    SourceVideo::Data output(8);
    DropOuts dropOuts;
    core.stackField(1, fields, parameters, fieldMetadata, available, true, false, output, dropOuts, 1, 15 * 256, false);
    CHECK(output[0] == 1010 && dropOuts.size() == 0);
    core.stackField(1, fields, parameters, fieldMetadata, available, true, false, output, dropOuts, 0, 15 * 256, false);
    CHECK(output[0] == 2336);
    core.stackField(1, fields, parameters, fieldMetadata, available, true, false, output, dropOuts, 2, 15 * 256, false);
    CHECK(output[0] == 1005);
    // the smart neighbour mode with a centre under the threshold (the sync tips of a capture are): the mean of the samples near it, as for any other centre. It was the centre itself,
    // because the centre was unsigned and centre - threshold wrapped round (the results of the code before are those of the other modes, which did not change)
    core.stackField(1, fields, parameters, fieldMetadata, available, true, false, output, dropOuts, 3, 15 * 256, false);
    CHECK(output[0] == 1005);

    // The automatic mode goes by the number of sources
    CHECK(StackerCore::autoMode(1) == 0 && StackerCore::autoMode(2) == 0);
    CHECK(StackerCore::autoMode(3) == 2 && StackerCore::autoMode(4) == 2);
    CHECK(StackerCore::autoMode(5) == 3 && StackerCore::autoMode(9) == 3);

    // A sample that is a dropout in every source is a dropout in the stack
    for (LdDecodeMetaData::Field &field: fieldMetadata) field.dropOuts.append(1, 2, 1);
    DropOuts left;
    core.stackField(1, fields, parameters, fieldMetadata, available, true, true, output, left, 0, 15 * 256, false);
    CHECK(left.size() > 0);
}

int main()
{
    testSimple();
    testModes();
    std::cerr << "All tests passed\n";
    return 0;
}
