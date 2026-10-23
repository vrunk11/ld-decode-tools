/******************************************************************************
 * testplotwidget.cpp
 * ld-analyse - unit test for the reduction of the points of a curve
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QElapsedTimer>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "plotwidget.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static QVector<QPointF> makeCurve(int count)
{
    QVector<QPointF> data;
    data.reserve(count);
    for (int i = 0; i < count; ++i) {
        // A slow wave and a fast one, and one spike
        double y = 40.0 + 10.0 * std::sin(i * 0.0007) + 3.0 * std::sin(i * 0.9);
        if (i == count / 3) y = 90.0;
        data.append(QPointF(i, y));
    }
    return data;
}

static PlotSeries *plot(PlotWidget &widget, const QVector<QPointF> &data, PlotSeries::PlotStyle style)
{
    widget.clearSeries();
    PlotSeries *series = widget.addSeries("test");
    series->setStyle(style);
    series->setData(data);
    widget.setAxisRange(Qt::Horizontal, 0, data.size());
    widget.setAxisRange(Qt::Vertical, 0, 100);
    widget.replot();
    QCoreApplication::processEvents();
    return series;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    PlotWidget widget;
    widget.resize(900, 300);
    widget.show();
    QCoreApplication::processEvents();

    // Few points: every one is kept
    {
        const QVector<QPointF> few = makeCurve(40);
        PlotSeries *series = plot(widget, few, PlotSeries::Lines);
        CHECK(series->path().elementCount() == few.size());
    }

    // Many points: at most four for each column of pixels, and the extremes are kept
    for (PlotSeries::PlotStyle style : {PlotSeries::Lines, PlotSeries::Bars}) {
        const QVector<QPointF> many = makeCurve(100000);
        PlotSeries *series = plot(widget, many, style);
        const int width = static_cast<int>(widget.width()) + 1;
        const int limit = (style == PlotSeries::Lines ? 4 : 4) * width;
        CHECK(series->path().elementCount() <= limit);
        CHECK(series->path().elementCount() > 100);

        // The spike (the highest value, 90) is the top of the drawing: the same as the whole curve gives
        const QRectF drawn = series->path().boundingRect();
        double highest = 0.0, lowest = 100.0;
        for (const QPointF &point : many) { highest = qMax(highest, point.y()); lowest = qMin(lowest, point.y()); }
        const double top = widget.mapFromData(QPointF(0.0, highest)).y();
        const double bottom = widget.mapFromData(QPointF(0.0, lowest)).y();
        CHECK(std::fabs(drawn.top() - top) < 0.01);
        if (style == PlotSeries::Lines) CHECK(std::fabs(drawn.bottom() - bottom) < 0.01);
    }

    // The cost of a repaint does not grow with the points
    {
        const QVector<QPointF> many = makeCurve(200000);
        QElapsedTimer timer;
        timer.start();
        PlotSeries *series = plot(widget, many, PlotSeries::Lines);
        series->update();
        widget.grab();
        CHECK(timer.elapsed() < 2000);
    }

    std::printf("testplotwidget: ok\n");
    return 0;
}
