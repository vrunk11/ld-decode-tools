/******************************************************************************
 * vitsmetrics.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019-2020 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "vitsmetrics.h"

#include <cmath>

#include "tbc/logging.h"

namespace {
// Get a specific slice of a field line and return all the values, as sample values
QVector<double> getFieldLineSlice(const SourceVideo::Data &sourceField, qint32 firstLineInData, qint32 fieldLine,
                                  qint32 startUs, qint32 lengthUs,
                                  const LdDecodeMetaData::VideoParameters &videoParameters)
{
    QVector<double> returnData;

    // Range-check the field line
    if (fieldLine < firstLineInData || fieldLine > videoParameters.fieldHeight) {
        qWarning() << "Cannot generate field-line data, line number is out of bounds! Scan line =" << fieldLine;
        return returnData;
    }

    // Calculate the number of samples per uS for the field
    double samplesPerUs = 0;
    if (videoParameters.system == PAL) samplesPerUs = static_cast<double>(videoParameters.fieldWidth) / 64.0;
    else samplesPerUs = static_cast<double>(videoParameters.fieldWidth) / 63.5;

    // Get the start and end sample positions
    const double startSampleDouble = startUs * samplesPerUs;
    const double lengthSampleDouble = lengthUs * samplesPerUs;

    const qint32 startPointer = ((fieldLine - firstLineInData) * videoParameters.fieldWidth) + static_cast<qint32>(startSampleDouble);
    const qint32 length = static_cast<qint32>(lengthSampleDouble);
    if (startPointer < 0 || startPointer + length > sourceField.size()) return returnData;

    returnData.resize(length);
    for (qint32 i = startPointer; i < startPointer + length; i++) {
        returnData[i - startPointer] = static_cast<double>(sourceField[i]);
    }

    return returnData;
}

// The arithmetic mean is the sum of the elements divided by the number of elements.
double calcMean(const QVector<double> &data)
{
    double result = 0;
    for (qint32 i = 0; i < data.size(); i++) result += data[i];
    return result / static_cast<double>(data.size());
}

// The standard deviation is the square root of the average of the squared deviations from the mean
double calcStd(const QVector<double> &data)
{
    double sum = 0.0;
    for (qint32 i = 0; i < data.size(); ++i) sum += data[i];
    const double mean = sum / static_cast<double>(data.size());

    double standardDeviation = 0.0;
    for (qint32 i = 0; i < data.size(); ++i) standardDeviation += pow(data[i] - mean, 2.0);

    return sqrt(standardDeviation / static_cast<double>(data.size()));
}

// Round a double to x decimal places
double roundDouble(double in, qint32 decimalPlaces)
{
    const double multiplier = pow(10.0, decimalPlaces);
    return ceil(in * multiplier) / multiplier;
}
}

VitsRawMeasurements measureVitsRaw(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                                   const LdDecodeMetaData::VideoParameters &videoParameters)
{
    // Get multiple possible black and white measurement points based on video format, etc.
    QVector<QVector<double>> wlSlice;
    QVector<double> blSlice;

    if (videoParameters.system == PAL) {
        // 625 lines (taken from ld-decode core.py)
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 19, 12, 8, videoParameters));
        blSlice = getFieldLineSlice(fieldLines, firstLineInData, 22, 12, 50, videoParameters);
    } else {
        // 525 lines (taken from ld-decode core.py)
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 20, 14, 12, videoParameters));
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 20, 52, 8, videoParameters));
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 13, 13, 15, videoParameters));
        blSlice = getFieldLineSlice(fieldLines, firstLineInData, 1, 10, 20, videoParameters);
    }

    VitsRawMeasurements raw;
    raw.hasBlack = !blSlice.isEmpty();
    if (raw.hasBlack) raw.blackStd = calcStd(blSlice);
    for (const QVector<double> &slice: wlSlice) {
        VitsRawMeasurements::WhiteSlice measured;
        if (!slice.isEmpty()) {
            measured.mean = calcMean(slice);
            measured.std = calcStd(slice);
        } else {
            measured.mean = std::nan("");
        }
        raw.white.append(measured);
    }
    return raw;
}

void applyVitsLevels(const VitsRawMeasurements &raw, const LdDecodeMetaData::VideoParameters &videoParameters,
                     LdDecodeMetaData::Field &fieldMetadata)
{
    // The measurements are in sample values; the SNR is of the signal between the levels (100 IRE)
    // over the noise, as in IRE
    const double ireScale = (static_cast<double>(videoParameters.white16bIre) - static_cast<double>(videoParameters.black16bIre)) / 100.0;
    const double black = static_cast<double>(videoParameters.black16bIre);

    // Only pick the white slice if it has a mean value between 90 and 110 IRE
    double wSNR = 0;
    for (const VitsRawMeasurements::WhiteSlice &slice: raw.white) {
        if (std::isnan(slice.mean)) continue;
        const double meanIre = (slice.mean - black) / ireScale;
        if (meanIre >= 90 && meanIre <= 110) {
            wSNR = 20.0 * log10(100.0 / (slice.std / ireScale));
            break;
        }
    }

    // The black slice is always the first (and only) one
    double bPSNR = 0;
    if (raw.hasBlack) bPSNR = 20.0 * log10(100.0 / (raw.blackStd / ireScale));

    // Update the metadata for the field
    fieldMetadata.vitsMetrics.wSNR = roundDouble(wSNR, 1);
    fieldMetadata.vitsMetrics.bPSNR = roundDouble(bPSNR, 1);
}

void computeVitsMetrics(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                        const LdDecodeMetaData::VideoParameters &videoParameters,
                        LdDecodeMetaData::Field &fieldMetadata)
{
    applyVitsLevels(measureVitsRaw(fieldLines, firstLineInData, videoParameters), videoParameters, fieldMetadata);
}
