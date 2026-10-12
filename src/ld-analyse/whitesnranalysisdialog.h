/******************************************************************************
 * whitesnranalysisdialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef WHITESNRANALYSISDIALOG_H
#define WHITESNRANALYSISDIALOG_H

#include <QDialog>
#include <QTimer>
#include <QShowEvent>
#include <cmath>
#include <QPen>
#include "plotwidget.h"
#include "snrsourcebar.h"

namespace Ui {
class WhiteSnrAnalysisDialog;
}

class WhiteSnrAnalysisDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WhiteSnrAnalysisDialog(QWidget *parent = nullptr);
    ~WhiteSnrAnalysisDialog();

    void startUpdate(qint32 _numberOfFrames);
    void addDataPoint(qint32 frameNumber, double whiteSnr);
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

    Ui::WhiteSnrAnalysisDialog *ui;
    SnrSourceBar *bar;
    bool liveMode = false;
    bool settingRange = false;
    PlotWidget *plot;
    PlotSeries *whiteSeries;
    PlotSeries *trendSeries;
    PlotMarker *plotMarker;

    double maxY;
    qint32 numberOfFrames;
    QVector<QPointF> whitePoints;
    QVector<QPointF> trendPoints;
    
    QTimer *updateTimer;
    qint32 pendingFrameNumber;
    bool hasPendingUpdate;
    QVector<double> tlPoint;
};

#endif // WHITESNRANALYSISDIALOG_H
