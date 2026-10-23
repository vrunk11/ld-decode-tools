/******************************************************************************
 * snranalysisdialog.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "snranalysisdialog.h"
#include "ui_snranalysisdialog.h"

#include <QPen>
#include "snrcolors.h"
#include <QTimer>
#include <algorithm>
#include <algorithm>

SnrAnalysisDialog::SnrAnalysisDialog(Kind _kind, QWidget *parent) :
    QDialog(parent),
    kind(_kind),
    minSnr(_kind == Black ? 20.0 : 14.0),
    topSnr(_kind == Black ? 48.0 : 42.0),
    ui(new Ui::SnrAnalysisDialog)
{
    ui->setupUi(this);
    setWindowTitle(kind == Black ? "Black SNR Analysis" : "White SNR Analysis");
    setWindowFlags(Qt::Window);

    bar = new SnrSourceBar(this);
    ui->verticalLayout->insertWidget(0, bar);

    // Set up the plot widget
    plot = new PlotWidget(this);
    plot->updateTheme();
    ui->verticalLayout->addWidget(plot);

    // Set up series and marker
    series = plot->addSeries(kind == Black ? "Black SNR" : "White SNR");
    // The colours of ld-stacker-gui, so that a curve is the same curve in both (and they read on a light and on a dark background)
    series->setPen(QPen(kind == Black ? snrBlackColor() : snrWhiteColor(), 2));
    
    trendSeries = plot->addSeries("Trend line");
    trendSeries->setPen(QPen(Qt::red, 2));
    
    plotMarker = plot->addMarker();
    plotMarker->setStyle(PlotMarker::VLine);
    plotMarker->setPen(QPen(Qt::blue, 2));

    // Set the maximum Y scale to 48
    maxY = topSnr;

    // Set the default number of frames
    numberOfFrames = 0;

    // Set up update throttling timer
    updateTimer = new QTimer(this);
    updateTimer->setSingleShot(true);
    updateTimer->setInterval(16); // ~60fps max update rate
    connect(updateTimer, &QTimer::timeout, this, &SnrAnalysisDialog::onUpdateTimerTimeout);
    hasPendingUpdate = false;
    pendingFrameNumber = 0;

    // Connect to plot area changed signal
    connect(plot, &PlotWidget::plotAreaChanged, this, &SnrAnalysisDialog::onPlotAreaChanged);
}

SnrAnalysisDialog::~SnrAnalysisDialog()
{
    removeChartContents();
    delete ui;
}

// Get ready for an update
void SnrAnalysisDialog::startUpdate(qint32 _numberOfFrames)
{
    liveMode = false;
    removeChartContents();
    numberOfFrames = _numberOfFrames;
    tlPoint.resize(numberOfFrames + 1);
    snrPoints.reserve(numberOfFrames);
}

// Remove the axes and series from the chart, giving ownership back to this object
void SnrAnalysisDialog::removeChartContents()
{
    maxY = topSnr;
    snrPoints.clear();
    tlPoint.clear();
    trendPoints.clear();
    plot->replot();
}

// Add a data point to the chart
void SnrAnalysisDialog::addDataPoint(qint32 frameNumber, double snr)
{
    if (!std::isnan(snr)) {
        // Clamp SNR values to minimum threshold (minSnr)
        double clampedSnr = std::max(snr, minSnr);
        snrPoints.append(QPointF(static_cast<qreal>(frameNumber), static_cast<qreal>(clampedSnr)));
        if (clampedSnr > maxY) maxY = ceil(clampedSnr); // Round up

        // Add to trendline data (use original unclamped value for trend calculation)
        tlPoint[frameNumber] = snr;
    } else {
        // Add to trendline data (mark as null value)
        tlPoint[frameNumber] = -1;
    }
}

// Finish the update and render the graph
void SnrAnalysisDialog::finishUpdate(qint32 _currentFrameNumber)
{
    // Set up plot properties
    if (kind == White) plot->updateTheme(); // Auto-detect theme and set appropriate background
    plot->setGridEnabled(true);
    plot->setZoomEnabled(true);
    plot->setPanEnabled(true);
    
    // Set axis titles and ranges
    plot->setAxisTitle(Qt::Horizontal, "Frame number");
    plot->setAxisTitle(Qt::Vertical, "SNR (in dB)");
    plot->setAxisRange(Qt::Horizontal, 0, numberOfFrames);
    plot->setAxisRange(Qt::Vertical, minSnr, maxY);

    // Set the series data
    series->setData(snrPoints);

    // Generate and set the trend line
    generateTrendLine();
    trendSeries->setData(trendPoints);

    // Set the frame marker position
    plotMarker->setPosition(QPointF(static_cast<double>(_currentFrameNumber), (maxY + minSnr) / 2));

    // Render the plot
    plot->replot();
}

// Method to update the frame marker (throttled for performance)
void SnrAnalysisDialog::updateFrameMarker(qint32 _currentFrameNumber)
{
    // Always store the pending frame number
    pendingFrameNumber = _currentFrameNumber;
    hasPendingUpdate = true;
    
    // Skip timer start if dialog is not visible - update will happen on show
    if (!isVisible()) return;
    
    // Start or restart the timer
    if (!updateTimer->isActive()) {
        updateTimer->start();
    }
}

void SnrAnalysisDialog::onUpdateTimerTimeout()
{
    if (!hasPendingUpdate) return;
    
    plotMarker->setPosition(QPointF(static_cast<double>(pendingFrameNumber), (maxY + minSnr) / 2));
    // No need to call plot->replot() - marker update() handles the redraw
    
    hasPendingUpdate = false;
}

void SnrAnalysisDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    
    // Force immediate marker update if we have a pending position
    if (hasPendingUpdate) {
        onUpdateTimerTimeout();
    }
}

void SnrAnalysisDialog::onPlotAreaChanged()
{
    // The graph was zoomed or panned by the user, not by an update: stop following the video
    if (liveMode && !settingRange) bar->setFollowing(false);
    // Handle plot area changes if needed
    // The PlotWidget handles zoom/pan internally
}

// Method to generate the trendline points
void SnrAnalysisDialog::generateTrendLine()
{
    // Only add a trend line if there are 5000 or more frames
    if (numberOfFrames < 5000) return;

    qint32 elements = 0;
    qint32 count = 0;
    double avgSum = 0;
    qint32 target = numberOfFrames / 500; // Number of frames to average

    trendPoints.clear();
    
    for (qint32 f = 0; f < numberOfFrames; f++) {
        if (tlPoint[f] != -1) {
            avgSum += tlPoint[f];
            elements++;
        }
        count++;

        if (count == target) {
            if (avgSum > 0 && elements > 0) {
                avgSum = avgSum / static_cast<double>(elements);
                // Clamp trend line points to minimum threshold (minSnr)
                double clampedAvg = std::max(avgSum, minSnr);
                trendPoints.append(QPointF(f-target, clampedAvg));
            }
            avgSum = 0;
            count = 0;
            elements = 0;
        }
    }
}

// A live input: the history of the SNR of what has been received. The graph is left as the user
// has put it unless it is following the video.
void SnrAnalysisDialog::setLiveHistory(const QVector<QPointF> &points, qint32 frames)
{
    liveMode = true;
    numberOfFrames = frames;
    maxY = topSnr;
    snrPoints.clear();
    for (const QPointF &point: points) {
        if (!std::isfinite(point.y())) continue;     // (an infinite one would make the scale of the graph infinite)
        // Clamp SNR values to minimum threshold (minSnr)
        const double clamped = std::max(point.y(), minSnr);
        snrPoints.append(QPointF(point.x(), clamped));
        if (clamped > maxY) maxY = ceil(clamped);
    }
    series->setData(snrPoints);
    trendSeries->setData(QVector<QPointF>());
    if (bar->isFollowing()) {
        settingRange = true;
        // (from the first frame in the history, which is later after Clear or beyond the longest history)
        const double first = snrPoints.isEmpty() ? 0 : std::floor(snrPoints.first().x());
        plot->setAxisRange(Qt::Horizontal, first, std::max(static_cast<double>(frames), first + 1));
        plot->setAxisRange(Qt::Vertical, minSnr, maxY);
        settingRange = false;
    }
    plot->replot();
}
