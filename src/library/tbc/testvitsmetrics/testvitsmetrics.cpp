/******************************************************************************
 * testvitsmetrics.cpp
 * ld-decode-tools - unit tests for the VITS measurements and SnrAverage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "vitsmetrics.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static bool same(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

static void testAverage()
{
    std::cerr << "Testing SnrAverage with values\n";

    // No field: NaN, and nothing there
    SnrAverage none;
    CHECK(!none.hasWhite() && !none.hasBlack());
    CHECK(std::isnan(none.white()) && std::isnan(none.black()));

    // The average of the fields that have a value
    SnrAverage both;
    both.add(40.0, 30.0);
    both.add(42.0, 34.0);
    CHECK(both.hasWhite() && both.hasBlack());
    CHECK(same(both.white(), 41.0) && same(both.black(), 32.0));

    // A value that is 0 (a measure that could not be made), negative, NaN or infinite is not there: the other field is the frame's
    SnrAverage partial;
    partial.add(40.0, 0.0);
    partial.add(std::nan(""), 34.0);
    CHECK(same(partial.white(), 40.0) && same(partial.black(), 34.0));
    partial.add(-3.0, std::numeric_limits<double>::infinity());
    CHECK(same(partial.white(), 40.0) && same(partial.black(), 34.0));

    // White and black are apart: a frame with a white SNR and no black PSNR
    SnrAverage whiteOnly;
    whiteOnly.add(38.5, 0.0);
    CHECK(whiteOnly.hasWhite() && !whiteOnly.hasBlack());
    CHECK(std::isnan(whiteOnly.black()));

    // From the metrics of a field
    LdDecodeMetaData::VitsMetrics metrics;
    metrics.wSNR = 36.0;
    metrics.bPSNR = 44.0;
    SnrAverage fromMetrics;
    fromMetrics.add(metrics);
    CHECK(same(fromMetrics.white(), 36.0) && same(fromMetrics.black(), 44.0));
}

static void testMeasured()
{
    std::cerr << "Testing SnrAverage with measured fields\n";

    LdDecodeMetaData metadata;
    metadata.initialiseDefault(PAL);
    LdDecodeMetaData::VideoParameters parameters = metadata.getVideoParameters();

    // Two fields of measurements: what it makes of them is the average of what applyVitsLevels makes of each
    VitsRawMeasurements first, second;
    first.hasBlack = true;
    first.blackStd = 120.0;
    first.white = { { 52000.0, 300.0 } };
    second.hasBlack = true;
    second.blackStd = 90.0;
    second.white = { { 51000.0, 250.0 } };

    LdDecodeMetaData::Field fieldA, fieldB;
    applyVitsLevels(first, parameters, fieldA, vitsIreSpan(parameters.system));
    applyVitsLevels(second, parameters, fieldB, vitsIreSpan(parameters.system));
    CHECK(fieldA.vitsMetrics.wSNR > 0 && fieldB.vitsMetrics.bPSNR > 0);

    SnrAverage average;
    average.add(first, parameters);
    average.add(second, parameters);
    CHECK(same(average.white(), (fieldA.vitsMetrics.wSNR + fieldB.vitsMetrics.wSNR) / 2));
    CHECK(same(average.black(), (fieldA.vitsMetrics.bPSNR + fieldB.vitsMetrics.bPSNR) / 2));

    // A field with nothing measured adds nothing
    SnrAverage nothing;
    nothing.add(VitsRawMeasurements(), parameters);
    CHECK(!nothing.hasWhite() && !nothing.hasBlack());
}

int main()
{
    testAverage();
    testMeasured();
    std::cerr << "All tests passed\n";
    return 0;
}
