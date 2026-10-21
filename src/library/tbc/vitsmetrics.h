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
// and put them in fieldMetadata.vitsMetrics (0 for one that could not be measured).
//
// blackToWhiteIre is how many IRE there are from the black level to the white level. ld-process-vits
// takes it to be 100, which is so for PAL; for NTSC the black level is at 7.5 IRE (the setup) and
// there are 92.5, which is what ld-decode measures with and gives SNRs 0.68 dB higher (see
// vitsIreSpan).
void applyVitsLevels(const VitsRawMeasurements &raw, const LdDecodeMetaData::VideoParameters &videoParameters,
                     LdDecodeMetaData::Field &fieldMetadata, double blackToWhiteIre = 100.0);

// The IRE from the black level to the white level of a video system: 100 for PAL, 92.5 for the
// systems with a 7.5 IRE setup
double vitsIreSpan(VideoSystem system);

// Measure the white SNR and black PSNR of a field from its test signal lines, as ld-process-vits
// does, and put them in fieldMetadata.vitsMetrics
void computeVitsMetrics(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                        const LdDecodeMetaData::VideoParameters &videoParameters,
                        LdDecodeMetaData::Field &fieldMetadata);

// The SNR of a frame from its fields: the average of the fields that have a value. A value that is not finite or not above 0 (0 is what applyVitsLevels
// puts for a measure that could not be made) is not a measure and is left out. NaN if no field has one.
class SnrAverage
{
public:
    // The white SNR and black PSNR of a field
    void add(double whiteSnr, double blackPsnr);
    void add(const LdDecodeMetaData::VitsMetrics &metrics) { add(metrics.wSNR, metrics.bPSNR); }
    // A field that was measured (measureVitsRaw), with the levels of videoParameters and the span of IRE of its video system
    void add(const VitsRawMeasurements &raw, const LdDecodeMetaData::VideoParameters &videoParameters);
    // A field from its test signal lines (as for measureVitsRaw)
    void add(const SourceVideo::Data &fieldLines, qint32 firstLineInData, const LdDecodeMetaData::VideoParameters &videoParameters);

    bool hasWhite() const { return whitePoints > 0; }
    bool hasBlack() const { return blackPoints > 0; }
    double white() const;
    double black() const;

private:
    double whiteTotal = 0;
    double blackTotal = 0;
    qint32 whitePoints = 0;
    qint32 blackPoints = 0;
};

#endif // VITSMETRICS_H
