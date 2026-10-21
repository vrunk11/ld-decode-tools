/******************************************************************************
 * testaspectratio.cpp
 * ld-decode-tools - unit tests for AspectRatio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>

#include "aspectratio.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

int main()
{
    std::cerr << "Testing AspectRatio\n";

    CHECK(AspectRatio::widthAdjustment(PAL, true) == 103);
    CHECK(AspectRatio::widthAdjustment(PAL, false) == -196);
    CHECK(AspectRatio::widthAdjustment(NTSC, true) == 122);
    CHECK(AspectRatio::widthAdjustment(NTSC, false) == -150);
    // PAL-M has 525 lines
    CHECK(AspectRatio::widthAdjustment(PAL_M, true) == 122);
    CHECK(AspectRatio::widthAdjustment(PAL_M, false) == -150);

    std::cerr << "All tests passed\n";
    return 0;
}
