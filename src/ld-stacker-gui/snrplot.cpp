/******************************************************************************
 * snrplot.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "snrplot.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

SnrPlot::SnrPlot(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(50);
    setCursor(Qt::CrossCursor);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
}

void SnrPlot::setSeries(const QVector<float> &values, const QColor &color, const QString &label)
{
    points.clear();
    for (qint32 i = 0; i < values.size(); i++) {
        if (std::isfinite(values[i])) points.append(QPointF(i + 1, values[i]));
    }
    totalFrames = values.size();
    lineColor = color;
    text = label;
    sparse = false;

    low = 0;
    high = 1;
    if (!points.isEmpty()) {
        low = high = points.first().y();
        for (const QPointF &point: points) {
            low = qMin(low, point.y());
            high = qMax(high, point.y());
        }
    }
    update();
}

void SnrPlot::setSparseSeries(const QMap<qint32, float> &values, qint32 total, const QColor &color, const QString &label)
{
    points.clear();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (std::isfinite(it.value())) points.append(QPointF(it.key(), it.value()));
    }
    totalFrames = total;
    lineColor = color;
    text = label;
    sparse = true;

    low = 0;
    high = 1;
    if (!points.isEmpty()) {
        low = high = points.first().y();
        for (const QPointF &point: points) {
            low = qMin(low, point.y());
            high = qMax(high, point.y());
        }
    }
    update();
}

void SnrPlot::setMarker(qint32 frame)
{
    markerFrame = frame;
    update();
}

qint32 SnrPlot::frameAt(qint32 x) const
{
    if (totalFrames < 1) return 1;
    const double fraction = qBound(0.0, static_cast<double>(x) / qMax(1, width() - 1), 1.0);
    return 1 + qRound(fraction * (totalFrames - 1));
}

void SnrPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF area = rect().adjusted(0, 4, 0, -4);
    const QColor gridColor = palette().color(QPalette::Mid);

    // A line every quarter
    painter.setPen(QPen(gridColor, 1, Qt::DotLine));
    for (qint32 i = 1; i < 4; i++) {
        const qreal y = area.top() + area.height() * i / 4.0;
        painter.drawLine(QPointF(0, y), QPointF(width(), y));
    }

    if (totalFrames > 0 && !points.isEmpty()) {
        double span = high - low;
        double lo = low;
        if (span < 1.0) { lo -= (1.0 - span) / 2; span = 1.0; }
        // A little room above and below
        lo -= span * 0.05;
        span *= 1.1;

        const auto toPoint = [&](const QPointF &point) {
            const qreal x = totalFrames > 1 ? (point.x() - 1) * (width() - 1) / (totalFrames - 1) : 0;
            const qreal y = area.bottom() - (point.y() - lo) / span * area.height();
            return QPointF(x, y);
        };

        painter.setPen(QPen(lineColor, 1.5));
        if (sparse) {
            // Measured as the frames were viewed: dots, joined where they are next to each other
            QPointF previous;
            qint32 previousFrame = -1;
            for (const QPointF &point: points) {
                const QPointF at = toPoint(point);
                if (previousFrame >= 0 && point.x() - previousFrame <= 1) painter.drawLine(previous, at);
                painter.drawEllipse(at, 1.5, 1.5);
                previous = at;
                previousFrame = static_cast<qint32>(point.x());
            }
        } else {
            QPainterPath path;
            bool first = true;
            for (const QPointF &point: points) {
                const QPointF at = toPoint(point);
                if (first) { path.moveTo(at); first = false; } else { path.lineTo(at); }
            }
            painter.drawPath(path);
        }
    }

    if (totalFrames > 0 && markerFrame >= 1) {
        const qreal x = totalFrames > 1 ? (markerFrame - 1) * (width() - 1) / (totalFrames - 1) : 0;
        painter.setPen(QPen(palette().color(QPalette::Text), 2));
        painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }

    if (!text.isEmpty()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF(6, 2, width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

void SnrPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) emit frameRequested(frameAt(static_cast<qint32>(event->position().x())));
}

void SnrPlot::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) emit frameRequested(frameAt(static_cast<qint32>(event->position().x())));
}
