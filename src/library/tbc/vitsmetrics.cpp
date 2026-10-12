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
// Get a specific slice of a field line and return all the values, as IRE
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

    // Convert data points to floating-point IRE values
    returnData.resize(length);
    for (qint32 i = startPointer; i < startPointer + length; i++) {
        returnData[i - startPointer] = (static_cast<double>(sourceField[i]) - static_cast<double>(videoParameters.black16bIre)) /
                ((static_cast<double>(videoParameters.white16bIre) - static_cast<double>(videoParameters.black16bIre)) / 100.0);
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

// Calculate the SNR or Percentage SNR
double calculateSnr(const QVector<double> &data, bool usePsnr)
{
    double signal = 0;
    if (usePsnr) signal = 100.0; else signal = calcMean(data); // Compute the arithmetic mean
    const double noise = calcStd(data); // Compute the standard deviation

    return 20.0 * log10(signal / noise);
}

// Round a double to x decimal places
double roundDouble(double in, qint32 decimalPlaces)
{
    const double multiplier = pow(10.0, decimalPlaces);
    return ceil(in * multiplier) / multiplier;
}
}

void computeVitsMetrics(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                        const LdDecodeMetaData::VideoParameters &videoParameters,
                        LdDecodeMetaData::Field &fieldMetadata)
{
    // Get multiple possible black and white measurement points based on video format, etc.
    QVector<QVector<double>> wlSlice;
    QVector<QVector<double>> blSlice;

    if (videoParameters.system == PAL) {
        // 625 lines (taken from ld-decode core.py)
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 19, 12, 8, videoParameters));
        blSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 22, 12, 50, videoParameters));
    } else {
        // 525 lines (taken from ld-decode core.py)
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 20, 14, 12, videoParameters));
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 20, 52, 8, videoParameters));
        wlSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 13, 13, 15, videoParameters));
        blSlice.append(getFieldLineSlice(fieldLines, firstLineInData, 1, 10, 20, videoParameters));
    }

    // Only pick the white slice if it has a mean value between 90 and 110 IRE
    qint32 wlSliceToUse = -1;
    for (qint32 i = 0; i < wlSlice.size(); i++) {
        if (wlSlice[i].isEmpty()) continue;
        const double wlMean = calcMean(wlSlice[i]);
        if (wlMean >= 90 && wlMean <= 110) {
            wlSliceToUse = i;
            break;
        }
    }

    // Always use the first black slice (there is only ever one to choose from)
    // Doing it this way in case more sources are added in the future
    const qint32 blSliceToUse = blSlice[0].isEmpty() ? -1 : 0;

    // Only calculate the wSNR if we have a valid slice
    double wSNR = 0;
    if (wlSliceToUse != -1) wSNR = calculateSnr(wlSlice[wlSliceToUse], true);

    // Only calculate the bPSNR if we have a valid slice
    double bPSNR = 0;
    if (blSliceToUse != -1) bPSNR = calculateSnr(blSlice[blSliceToUse], true);

    // Update the metadata for the field
    fieldMetadata.vitsMetrics.wSNR = roundDouble(wSNR, 1);
    fieldMetadata.vitsMetrics.bPSNR = roundDouble(bPSNR, 1);
}
