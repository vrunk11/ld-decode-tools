/******************************************************************************
 * vitsmetrics.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019-2020 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VITSMETRICS_H
#define VITSMETRICS_H

#include <QVector>

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// The lines of a field that the VITS measurements read (1-based, inclusive)
constexpr qint32 VITS_START_FIELD_LINE = 1;
constexpr qint32 VITS_END_FIELD_LINE = 22;

// What is measured on the test signal lines of a field, in sample values: it does not depend on
// the black and white levels, which are applied afterwards (applyVitsLevels). Keeping these
// makes it cheap to work the SNR out again when the levels are changed.
struct VitsRawMeasurements {
    bool hasBlack = false;
    double blackStd = 0;                    // standard deviation of the black slice

    struct WhiteSlice {
        double mean = 0;
        double std = 0;
    };
    QVector<WhiteSlice> white;              // the candidate white slices, in order of preference
};

// Measure the test signal lines of a field. fieldLines holds the field lines from firstLineInData
// (at most VITS_START_FIELD_LINE) to VITS_END_FIELD_LINE or later.
VitsRawMeasurements measureVitsRaw(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                                   const LdDecodeMetaData::VideoParameters &videoParameters);

// Work out the white SNR and black PSNR from the measurements, for the levels in videoParameters,
// and put them in fieldMetadata.vitsMetrics (0 for one that could not be measured)
void applyVitsLevels(const VitsRawMeasurements &raw, const LdDecodeMetaData::VideoParameters &videoParameters,
                     LdDecodeMetaData::Field &fieldMetadata);

// Measure the white SNR and black PSNR of a field from its test signal lines, as ld-process-vits
// does, and put them in fieldMetadata.vitsMetrics
void computeVitsMetrics(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                        const LdDecodeMetaData::VideoParameters &videoParameters,
                        LdDecodeMetaData::Field &fieldMetadata);

#endif // VITSMETRICS_H
