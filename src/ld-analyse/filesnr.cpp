/******************************************************************************
 * filesnr.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "filesnr.h"

#include <cmath>

#include "sourcevideo.h"

FileSnr::FileSnr(const QString &_fileName, qint32 _fieldLength, qint32 _fieldWidth,
                 const QVector<QPair<qint32, qint32>> &_frameFields, QObject *parent)
    : QThread(parent), fileName(_fileName), fieldLength(_fieldLength), fieldWidth(_fieldWidth),
      frameFields(_frameFields)
{
    stopRequested.storeRelaxed(0);
}

FileSnr::~FileSnr()
{
    stopAnalysis();
}

void FileSnr::stopAnalysis()
{
    stopRequested.storeRelaxed(1);
    if (isRunning()) wait();
}

void FileSnr::setVideoParameters(const LdDecodeMetaData::VideoParameters &parameters)
{
    QMutexLocker locker(&mutex);
    videoParameters = parameters;
    levelsChanged = true;
}

// The SNR of a frame from its measurements, as ld-analyse does for the metadata (the average of
// the fields that have one)
void FileSnr::computeFrame(qint32 index)
{
    SnrAverage snr;
    snr.add(rawFirst[index], videoParameters);
    snr.add(rawSecond[index], videoParameters);
    frameBlack[index] = snr.black();
    frameWhite[index] = snr.white();
}

void FileSnr::run()
{
    SourceVideo source;
    if (!source.open(fileName, fieldLength, fieldWidth)) return;

    LdDecodeMetaData::VideoParameters parameters;
    {
        QMutexLocker locker(&mutex);
        parameters = videoParameters;
    }

    // Measure the frames in order
    for (qint32 index = 0; index < frameFields.size() && !stopRequested.loadRelaxed(); index++) {
        VitsRawMeasurements measured[2];
        const qint32 fieldNumbers[2] = { frameFields[index].first, frameFields[index].second };
        for (qint32 which = 0; which < 2; which++) {
            if (fieldNumbers[which] >= 1 && fieldNumbers[which] <= source.getNumberOfAvailableFields()) {
                measured[which] = measureVitsRaw(source.getVideoField(fieldNumbers[which], 1, VITS_END_FIELD_LINE), 1, parameters);
            }
        }

        QMutexLocker locker(&mutex);
        rawFirst.append(measured[0]);
        rawSecond.append(measured[1]);
        frameBlack.append(std::nan(""));
        frameWhite.append(std::nan(""));
        computeFrame(index);
        // The levels may have been changed meanwhile: that is worked out below, for the lot
        if (levelsChanged) {
            parameters = videoParameters;
            for (qint32 i = 0; i < rawFirst.size(); i++) computeFrame(i);
            levelsChanged = false;
        }
    }

    // Done: wait for the levels to be changed, and work it out again, until stopped
    while (!stopRequested.loadRelaxed()) {
        {
            QMutexLocker locker(&mutex);
            if (levelsChanged) {
                for (qint32 i = 0; i < rawFirst.size(); i++) computeFrame(i);
                levelsChanged = false;
            }
        }
        msleep(50);
    }
}

qint32 FileSnr::measuredFrames()
{
    QMutexLocker locker(&mutex);
    return frameBlack.size();
}

QVector<QPointF> FileSnr::history(bool black, qint32 maxPoints)
{
    QVector<double> values;
    {
        QMutexLocker locker(&mutex);
        values = black ? frameBlack : frameWhite;
    }

    QVector<QPointF> points;
    const qint32 count = values.size();
    const qint32 stride = qMax(1, (count + maxPoints - 1) / qMax(1, maxPoints));
    for (qint32 start = 0; start < count; start += stride) {
        double total = 0;
        qint32 valid = 0;
        for (qint32 i = start; i < qMin(count, start + stride); i++) {
            if (!std::isnan(values[i])) { total += values[i]; valid++; }
        }
        if (valid) points.append(QPointF(start + 1 + (stride - 1) / 2.0, total / valid));
    }
    return points;
}
