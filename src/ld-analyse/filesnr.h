/******************************************************************************
 * filesnr.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FILESNR_H
#define FILESNR_H

#include <QAtomicInt>
#include <QMutex>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QThread>
#include <QVector>

#include "lddecodemetadata.h"
#include "vitsmetrics.h"

// Measures the black and white SNR of every frame of a file from the video itself, on a thread of
// its own, for when the metadata is missing or not to be trusted. It reads the file through a
// SourceVideo of its own, so it does not disturb the one the window uses. The measurements are kept
// apart from the black and white levels, so adjusting the levels works the whole lot out again.
class FileSnr : public QThread
{
    Q_OBJECT
public:
    // frameFields: for each frame, the numbers (from 1) of its first and second fields
    FileSnr(const QString &fileName, qint32 fieldLength, qint32 fieldWidth,
            const QVector<QPair<qint32, qint32>> &frameFields, QObject *parent = nullptr);
    ~FileSnr() override;

    void setVideoParameters(const LdDecodeMetaData::VideoParameters &videoParameters);
    void stopAnalysis();

    // The SNR of the frames measured so far (x is the frame number, from 1), averaged in blocks if
    // there are more than maxPoints
    QVector<QPointF> history(bool black, qint32 maxPoints);
    // Frames measured, and in all
    qint32 measuredFrames();
    qint32 totalFrames() const { return frameFields.size(); }

protected:
    void run() override;

private:
    QString fileName;
    qint32 fieldLength;
    qint32 fieldWidth;
    QVector<QPair<qint32, qint32>> frameFields;

    QMutex mutex;
    LdDecodeMetaData::VideoParameters videoParameters;
    QAtomicInt stopRequested;
    bool levelsChanged = false;

    QVector<VitsRawMeasurements> rawFirst, rawSecond;   // by frame, as far as measured
    QVector<double> frameBlack, frameWhite;             // by frame, NaN where not measured

    void computeFrame(qint32 index);                    // mutex held
};

#endif // FILESNR_H
