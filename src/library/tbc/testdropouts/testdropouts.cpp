/******************************************************************************
 * testdropouts.cpp
 * ld-decode-tools - unit tests for DropOuts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>

#include "dropouts.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static void testTotalLength()
{
    std::cerr << "Testing DropOuts::totalLength\n";

    DropOuts none;
    CHECK(none.totalLength() == 0);

    // The length of a dropout is endx - startx, as the metadata count them: a one sample dropout is 0
    DropOuts dropOuts;
    dropOuts.append(10, 19, 5);
    dropOuts.append(100, 100, 6);
    dropOuts.append(200, 250, 300);
    CHECK(dropOuts.totalLength() == 9 + 0 + 50);

    // one that ends before it starts counts nothing
    dropOuts.append(50, 40, 7);
    CHECK(dropOuts.totalLength() == 59);
}

static void testVisibleLength()
{
    std::cerr << "Testing DropOuts::visibleLength\n";

    // Lines 20 to 250 of the field, samples 100 to 900
    const qint32 firstLine = 20, lastLine = 250, activeStart = 100, activeEnd = 900;

    DropOuts dropOuts;
    dropOuts.append(300, 340, 100);     // inside: 40
    dropOuts.append(50, 150, 100);      // starts before the picture: 150 - 100 = 50
    dropOuts.append(850, 1000, 100);    // ends after it: 900 - 850 = 50
    dropOuts.append(10, 60, 100);       // before it: 0
    dropOuts.append(950, 990, 100);     // after it: 0
    dropOuts.append(20, 2000, 100);     // all of it: 800
    dropOuts.append(300, 340, 19);      // above the picture: not counted
    dropOuts.append(300, 340, 251);     // under it: not counted
    dropOuts.append(300, 340, 20);      // the first line: 40
    dropOuts.append(300, 340, 250);     // the last line: 40
    dropOuts.append(300, 300, 100);     // one sample: 0
    CHECK(dropOuts.visibleLength(firstLine, lastLine, activeStart, activeEnd) == 40 + 50 + 50 + 800 + 40 + 40);
    CHECK(DropOuts().visibleLength(firstLine, lastLine, activeStart, activeEnd) == 0);
}

static void testWholeField()
{
    std::cerr << "Testing DropOuts::coversWholeField\n";

    const qint32 width = 20, height = 6;

    // A dropout over the whole width of each line
    DropOuts whole;
    for (qint32 line = 1; line <= height; line++) whole.append(0, width - 1, line);
    CHECK(whole.coversWholeField(width, height));
    // (it can be wider)
    DropOuts wider;
    for (qint32 line = 1; line <= height; line++) wider.append(0, width + 5, line);
    CHECK(wider.coversWholeField(width, height));
    CHECK(!DropOuts().coversWholeField(width, height));

    // A line short of it, or one that does not reach the ends
    DropOuts missingLine;
    for (qint32 line = 1; line < height; line++) missingLine.append(0, width - 1, line);
    CHECK(!missingLine.coversWholeField(width, height));
    DropOuts notToTheEnd;
    for (qint32 line = 1; line <= height; line++) notToTheEnd.append(0, width - 2, line);
    CHECK(!notToTheEnd.coversWholeField(width, height));
    DropOuts notFromTheStart;
    for (qint32 line = 1; line <= height; line++) notFromTheStart.append(1, width - 1, line);
    CHECK(!notFromTheStart.coversWholeField(width, height));

    // The same line twice does not make up for a line that is missing; lines outside the field do not count
    DropOuts twice;
    for (qint32 line = 1; line < height; line++) twice.append(0, width - 1, line);
    twice.append(0, width - 1, 1);
    twice.append(0, width - 1, height + 1);
    CHECK(!twice.coversWholeField(width, height));
}

int main()
{
    testTotalLength();
    testVisibleLength();
    testWholeField();
    std::cerr << "All tests passed\n";
    return 0;
}
