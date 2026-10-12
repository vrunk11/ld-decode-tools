/******************************************************************************
 * livesnr.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LIVESNR_H
#define LIVESNR_H

#include <QAtomicInt>
#include <QMutex>
#include <QPointF>
#include <QThread>
#include <QVector>

#include "lddecodemetadata.h"
#include "sourcevideo.h"
#include "vitsmetrics.h"

// Measures the black and white SNR of every frame of a live input as it arrives, on a thread of
// its own, so that there is a history of what was received whether or not the frames were shown.
// The measurements are kept apart from the black and white levels, so adjusting the levels works
// the whole history out again.
//
// It only looks at the top lines of each field, and never waits for the input: it asks how
// much has arrived, and sleeps if there is nothing new. It must be stopped before the source is
// closed.
class LiveSnr : public QThread
{
    Q_OBJECT
public:
    explicit LiveSnr(SourceVideo &source, QObject *parent = nullptr);
    ~LiveSnr() override;

    void setVideoParameters(const LdDecodeMetaData::VideoParameters &videoParameters);
    // Measuring can be switched off and on: it carries on from the newest field
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled.loadRelaxed() != 0; }
    void stopAnalysis();
    // Forget the history so far: it starts again from the newest field
    void clear();

    // The longest history kept, in frames (two hours of NTSC video); the oldest is let go beyond it
    static constexpr qint32 MAX_FRAMES = 216000;

    // The SNR of the frames so far (x is the frame number, from 1). With more than maxPoints
    // frames they are averaged in blocks, so the history of a long input stays light.
    QVector<QPointF> history(bool black, qint32 maxPoints);
    qint32 measuredFrames();

protected:
    void run() override;

private:
    SourceVideo &source;
    QMutex mutex;
    LdDecodeMetaData::VideoParameters videoParameters;
    QAtomicInt enabled;
    QAtomicInt stopRequested;
    bool levelsChanged = false;

    QVector<VitsRawMeasurements> rawFields;     // by field, from field baseField + 1
    QVector<bool> rawValid;
    QVector<double> frameBlack, frameWhite;     // by frame from baseField / 2 + 1, NaN where not measured
    qint32 nextField = 1;
    qint32 baseField = 0;                       // fields before the first one kept (even)

    void measureFrame(qint32 frame);            // from rawFields; mutex held
};

#endif // LIVESNR_H
