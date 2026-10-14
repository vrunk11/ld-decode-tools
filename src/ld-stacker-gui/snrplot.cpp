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

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <cmath>

SnrPlot::SnrPlot(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(40);
    setCursor(Qt::CrossCursor);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
}

void SnrPlot::setCurves(const QVector<SnrCurve> &newCurves, const QString &label)
{
    curves = newCurves;
    text = label;
    totalFrames = 0;
    for (const SnrCurve &curve: curves) totalFrames = qMax(totalFrames, static_cast<qint32>(curve.values.size()));
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

    // A line every quarter
    painter.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
    for (qint32 i = 1; i < 4; i++) {
        const qreal y = area.top() + area.height() * i / 4.0;
        painter.drawLine(QPointF(0, y), QPointF(width(), y));
    }

    // The range of what is shown
    bool any = false;
    double low = 0, high = 0;
    for (const SnrCurve &curve: curves) {
        if (!curve.visible) continue;
        for (float value: curve.values) {
            if (!std::isfinite(value)) continue;
            if (!any) { low = high = value; any = true; }
            low = qMin<double>(low, value);
            high = qMax<double>(high, value);
        }
    }

    if (totalFrames > 0 && any) {
        double span = high - low;
        double lo = low;
        if (span < 1.0) { lo -= (1.0 - span) / 2; span = 1.0; }
        lo -= span * 0.05;   // a little room above and below
        span *= 1.1;

        // What the lines are worth, in dB
        QFont small = font();
        small.setPointSizeF(qMax(7.0, small.pointSizeF() - 1));
        painter.setFont(small);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        for (qint32 i = 1; i < 4; i++) {
            const qreal y = area.top() + area.height() * i / 4.0;
            const double value = lo + (area.bottom() - y) / area.height() * span;
            painter.drawText(QRectF(0, y - 14, width() - 4, 14), Qt::AlignRight | Qt::AlignBottom, QString("%1 dB").arg(value, 0, 'f', 1));
        }

        const auto toPoint = [&](qint32 index, float value) {
            const qreal x = totalFrames > 1 ? static_cast<qreal>(index) * (width() - 1) / (totalFrames - 1) : 0;
            const qreal y = area.bottom() - (value - lo) / span * area.height();
            return QPointF(x, y);
        };

        // The thin ones first, so that they are behind
        for (const bool thin: { true, false }) {
            for (const SnrCurve &curve: curves) {
                if (!curve.visible || curve.thin != thin) continue;
                QColor color = curve.color;
                if (thin) color.setAlpha(150);
                painter.setPen(QPen(color, thin ? 1.0 : 1.5));

                // A line, broken where a frame has no value
                QPainterPath path;
                bool drawing = false;
                for (qint32 i = 0; i < curve.values.size(); i++) {
                    if (!std::isfinite(curve.values[i])) { drawing = false; continue; }
                    const QPointF at = toPoint(i, curve.values[i]);
                    if (!drawing) { path.moveTo(at); drawing = true; } else { path.lineTo(at); }
                }
                painter.drawPath(path);
            }
        }
    }

    if (totalFrames > 0 && markerFrame >= 1) {
        const qreal x = totalFrames > 1 ? (markerFrame - 1) * (width() - 1) / (totalFrames - 1) : 0;
        painter.setPen(QPen(palette().color(QPalette::Text), 2));
        painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    }

    if (!text.isEmpty()) {
        painter.setFont(font());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF(6, 2, width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, text + tr("  (dB)"));
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

// ---------------------------------------------------------------------------------------------

SnrGraph::SnrGraph(bool withStack, QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QHBoxLayout *row = new QHBoxLayout;
    row->setContentsMargins(6, 2, 6, 2);
    row->setSpacing(12);
    whiteCheck = new QCheckBox(tr("White SNR"));
    whiteCheck->setChecked(true);
    whiteCheck->setStyleSheet(QString("QCheckBox { color: %1; }").arg(whiteColor().name()));
    blackCheck = new QCheckBox(tr("Black PSNR"));
    blackCheck->setChecked(true);
    blackCheck->setStyleSheet(QString("QCheckBox { color: %1; }").arg(blackColor().name()));
    row->addWidget(whiteCheck);
    row->addWidget(blackCheck);
    if (withStack) {
        stackCheck = new QCheckBox(tr("Stack behind"));
        stackCheck->setChecked(true);
        row->addWidget(stackCheck);
    }
    row->addStretch(1);
    layout->addLayout(row);

    plot = new SnrPlot;
    layout->addWidget(plot, 1);

    connect(plot, &SnrPlot::frameRequested, this, &SnrGraph::frameRequested);
    connect(whiteCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
    connect(blackCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
    if (stackCheck) connect(stackCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
}

void SnrGraph::setData(const QString &newLabel, const QVector<float> &newWhite, const QVector<float> &newBlack,
                       const QVector<float> &newStackWhite, const QVector<float> &newStackBlack)
{
    label = newLabel;
    white = newWhite;
    black = newBlack;
    stackWhite = newStackWhite;
    stackBlack = newStackBlack;
    redraw();
}

void SnrGraph::setMarker(qint32 frame)
{
    plot->setMarker(frame);
}

void SnrGraph::redraw()
{
    QVector<SnrCurve> curves;
    if (stackCheck && stackCheck->isChecked()) {
        curves.append({ stackWhite, whiteColor(), true, whiteCheck->isChecked() });
        curves.append({ stackBlack, blackColor(), true, blackCheck->isChecked() });
    }
    curves.append({ white, whiteColor(), false, whiteCheck->isChecked() });
    curves.append({ black, blackColor(), false, blackCheck->isChecked() });
    plot->setCurves(curves, label);
}
