/******************************************************************************
 * testlumacoring.cpp
 * ld-decode-tools - unit tests for coreLumaLine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "deemp.h"
#include "lumacoring.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

namespace {

// The noise coring as it was written in Comb, PalColour and MonoDecoder
template <typename Filter>
void originalCoring(double *Y, qint32 start, qint32 end, Filter &yFilter, qint32 delay, double nr_y)
{
    std::vector<double> hpY(end + delay);
    for (qint32 h = start - delay; h < start; h++) yFilter.feed(0.0);
    for (qint32 h = start; h < end; h++) hpY[h] = yFilter.feed(Y[h]);
    for (qint32 h = end; h < end + delay; h++) hpY[h] = yFilter.feed(0.0);
    for (qint32 h = start; h < end; h++) {
        double a = hpY[h + delay];
        if (fabs(a) > nr_y) a = (a > 0) ? nr_y : -nr_y;
        Y[h] -= a;
    }
}

template <typename Filter>
void compare(const Filter &filter, qint32 delay)
{
    const qint32 width = 400, start = 40, end = 380;
    unsigned seed = 12345;
    auto next = [&seed]() { seed = seed * 1103515245u + 12345u; return static_cast<double>((seed >> 8) & 0xFFFF); };

    // The same filter for all the lines (the state carries over), and a copy of a fresh one for each
    for (const bool carry : { true, false }) {
        for (const double level : { 100.0, 800.0, 6000.0 }) {
            Filter oldFilter(filter), newFilter(filter);
            std::vector<double> hpY(width + delay);
            for (int line = 0; line < 6; line++) {
                std::vector<double> a(width), b(width);
                for (int x = 0; x < width; x++) a[x] = b[x] = next();
                if (!carry) { oldFilter = filter; newFilter = filter; }
                originalCoring(a.data(), start, end, oldFilter, delay, level);
                coreLumaLine(b.data(), start, end, newFilter, delay, level, hpY);
                for (int x = 0; x < width; x++) CHECK(a[x] == b[x]);
            }
        }
    }
}

}

int main()
{
    std::cerr << "Testing coreLumaLine\n";
    compare(f_nr, static_cast<qint32>(c_nr_b.size()) / 2);
    compare(f_nrpal, static_cast<qint32>(c_nrpal_b.size()) / 2);
    std::cerr << "All tests passed\n";
    return 0;
}
