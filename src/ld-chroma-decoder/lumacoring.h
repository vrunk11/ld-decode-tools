/******************************************************************************
 * lumacoring.h
 * ld-chroma-decoder - analogue-style noise coring of one line of luma
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LUMACORING_H
#define LUMACORING_H

#include <cmath>
#include <vector>

#include <QtGlobal>

// Core one line of luma: the high-pass of the line, clipped to +/- coringLevel, is taken off the
// line. The filter is a symmetric FIR (its delay is delay samples); it is fed with zeros outside
// the active area, and keeps its state from one call to the next, so the caller says whether the
// state carries over from line to line (a copy of a fresh filter for each line, or the same one).
// hpY is the work buffer, at least activeVideoEnd + delay long.
template <typename Filter>
void coreLumaLine(double *Y, qint32 activeVideoStart, qint32 activeVideoEnd,
                  Filter &yFilter, qint32 delay, double coringLevel, std::vector<double> &hpY)
{
    // Feed zeros into the filter outside the active area
    for (qint32 h = activeVideoStart - delay; h < activeVideoStart; h++) {
        yFilter.feed(0.0);
    }
    for (qint32 h = activeVideoStart; h < activeVideoEnd; h++) {
        hpY[h] = yFilter.feed(Y[h]);
    }
    for (qint32 h = activeVideoEnd; h < activeVideoEnd + delay; h++) {
        hpY[h] = yFilter.feed(0.0);
    }

    for (qint32 h = activeVideoStart; h < activeVideoEnd; h++) {
        // Offset to cover the filter delay
        double a = hpY[h + delay];

        // Clip the filter strength
        if (std::fabs(a) > coringLevel) {
            a = (a > 0) ? coringLevel : -coringLevel;
        }

        Y[h] -= a;
    }
}

#endif // LUMACORING_H
