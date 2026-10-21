/******************************************************************************
 * snranalysisdialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SNRANALYSISDIALOG_H
#define SNRANALYSISDIALOG_H

#include <cmath>

#include <QDialog>
#include <QTimer>
#include <QShowEvent>
#include "plotwidget.h"
#include "snrsourcebar.h"
#include "lddecodemetadata.h"

namespace Ui {
class SnrAnalysisDialog;
}

class SnrAnalysisDialog : public QDialog
{
    Q_OBJECT

public:
    // The graph of the black or of the white SNR of the frames
    enum Kind { Black, White };

    explicit SnrAnalysisDialog(Kind kind, QWidget *parent = nullptr);
    ~SnrAnalysisDialog();

    void startUpdate(qint32 _numberOfFrames);
    void addDataPoint(qint32 frameNumber, double snr);
    void finishUpdate(qint32 _currentFrameNumber);
    void updateFrameMarker(qint32 _currentFrameNumber);

    // Above the graph: where the values come from, and for a live input the switches
    SnrSourceBar *sourceBar() { return bar; }
    // A live input: show this history (frame number, SNR) of what has been received
    void setLiveHistory(const QVector<QPointF> &points, qint32 frames);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onPlotAreaChanged();
    void onUpdateTimerTimeout();

private:
    void removeChartContents();
    void generateTrendLine();

    Ui::SnrAnalysisDialog *ui;
    const Kind kind;
    // The lowest SNR drawn (lower values are clamped to it) and the top of the axis, in dB
    const double minSnr;
    const double topSnr;
    SnrSourceBar *bar;
    bool liveMode = false;
    bool settingRange = false;
    PlotWidget *plot;
    PlotSeries *series;
    PlotSeries *trendSeries;
    PlotMarker *plotMarker;

    double maxY;
    qint32 numberOfFrames;
    QVector<QPointF> snrPoints;
    QVector<QPointF> trendPoints;
    
    QTimer *updateTimer;
    qint32 pendingFrameNumber;
    bool hasPendingUpdate;
    QVector<double> tlPoint;
};

#endif // SNRANALYSISDIALOG_H
