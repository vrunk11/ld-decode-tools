/******************************************************************************
 * livesnr.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "livesnr.h"

#include <cmath>

LiveSnr::LiveSnr(SourceVideo &_source, QObject *parent)
    : QThread(parent), source(_source)
{
    enabled.storeRelaxed(1);
    stopRequested.storeRelaxed(0);
}

LiveSnr::~LiveSnr()
{
    stopAnalysis();
}

void LiveSnr::setVideoParameters(const LdDecodeMetaData::VideoParameters &parameters)
{
    QMutexLocker locker(&mutex);
    videoParameters = parameters;
    levelsChanged = true;
}

void LiveSnr::setEnabled(bool on)
{
    enabled.storeRelaxed(on ? 1 : 0);
}

void LiveSnr::stopAnalysis()
{
    stopRequested.storeRelaxed(1);
    if (isRunning()) wait();
}

// The SNR of a frame, from the measurements of its two fields, as ld-analyse does for a file
// (the average of the fields that have one)
void LiveSnr::measureFrame(qint32 frame)
{
    const qint32 frameIndex = frame - 1 - baseField / 2;
    if (frameIndex < 0) return;

    double blackTotal = 0, whiteTotal = 0;
    qint32 blackPoints = 0, whitePoints = 0;
    for (qint32 fieldNumber: { 2 * frame - 1, 2 * frame }) {
        const qint32 index = fieldNumber - 1 - baseField;
        if (index < 0 || index >= rawFields.size() || !rawValid[index]) continue;
        LdDecodeMetaData::Field field;
        applyVitsLevels(rawFields[index], videoParameters, field);
        if (field.vitsMetrics.bPSNR > 0) { blackTotal += field.vitsMetrics.bPSNR; blackPoints++; }
        if (field.vitsMetrics.wSNR > 0) { whiteTotal += field.vitsMetrics.wSNR; whitePoints++; }
    }
    while (frameBlack.size() <= frameIndex) { frameBlack.append(std::nan("")); frameWhite.append(std::nan("")); }
    frameBlack[frameIndex] = blackPoints ? blackTotal / blackPoints : std::nan("");
    frameWhite[frameIndex] = whitePoints ? whiteTotal / whitePoints : std::nan("");
}

void LiveSnr::run()
{
    while (!stopRequested.loadRelaxed()) {
        LdDecodeMetaData::VideoParameters parameters;
        {
            QMutexLocker locker(&mutex);
            parameters = videoParameters;
            if (levelsChanged) {
                // Work the whole history out again for the new levels
                for (qint32 frame = baseField / 2 + 1; frame <= (baseField + rawFields.size()) / 2; frame++) measureFrame(frame);
                levelsChanged = false;
            }
        }

        const qint32 available = source.getFieldsReadSoFar();
        if (!enabled.loadRelaxed()) {
            // Not measuring: what arrives meanwhile is not measured
            QMutexLocker locker(&mutex);
            nextField = qMax(nextField, available + 1);
            locker.unlock();
            msleep(30);
            continue;
        }

        bool didSomething = false;
        while (!stopRequested.loadRelaxed() && enabled.loadRelaxed()) {
            qint32 field;
            {
                QMutexLocker locker(&mutex);
                field = nextField;
            }
            if (field > available || parameters.fieldWidth <= 0) break;

            VitsRawMeasurements raw;
            bool valid = false;
            // (a field that the ring has already overwritten has nothing to measure)
            if (source.isFieldHeld(field)) {
                const SourceVideo::Data lines = source.getVideoField(field, 1, VITS_END_FIELD_LINE);
                raw = measureVitsRaw(lines, 1, parameters);
                valid = true;
            }

            QMutexLocker locker(&mutex);
            // A field that was skipped (measuring was off) leaves a gap, kept as not measured
            while (baseField + rawFields.size() < field - 1) { rawFields.append(VitsRawMeasurements()); rawValid.append(false); }
            rawFields.append(raw);
            rawValid.append(valid);
            if (field % 2 == 0) measureFrame(field / 2);

            // Beyond the longest history, let go of the oldest tenth
            if (frameBlack.size() > MAX_FRAMES) {
                const qint32 drop = MAX_FRAMES / 10;
                rawFields.remove(0, qMin(rawFields.size(), 2 * drop));
                rawValid.remove(0, qMin(rawValid.size(), 2 * drop));
                frameBlack.remove(0, drop);
                frameWhite.remove(0, drop);
                baseField += 2 * drop;
            }
            nextField = field + 1;
            didSomething = true;
        }

        if (!didSomething) msleep(15);
    }
}

qint32 LiveSnr::measuredFrames()
{
    QMutexLocker locker(&mutex);
    return frameBlack.size();
}

void LiveSnr::clear()
{
    QMutexLocker locker(&mutex);
    // Start again from the next whole frame; what is between is not kept
    baseField = (nextField - 1) / 2 * 2;
    rawFields.clear();
    rawValid.clear();
    frameBlack.clear();
    frameWhite.clear();
    while (baseField + rawFields.size() < nextField - 1) { rawFields.append(VitsRawMeasurements()); rawValid.append(false); }
}

QVector<QPointF> LiveSnr::history(bool black, qint32 maxPoints)
{
    QVector<double> values;
    qint32 base;
    {
        QMutexLocker locker(&mutex);
        values = black ? frameBlack : frameWhite;
        base = baseField / 2;
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
        if (valid) points.append(QPointF(base + start + 1 + (stride - 1) / 2.0, total / valid));
    }
    return points;
}
