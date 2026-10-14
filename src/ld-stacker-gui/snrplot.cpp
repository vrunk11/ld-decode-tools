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

#include <QFontMetrics>
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
    setMouseTracking(true);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
}

void SnrPlot::setCurves(const QVector<SnrCurve> &newCurves)
{
    curves = newCurves;
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
    const double fraction = qBound(0.0, static_cast<double>(x) / qMax(1, plotWidth() - 1), 1.0);
    return 1 + qRound(fraction * (totalFrames - 1));
}

void SnrPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qint32 plotW = plotWidth();
    const QRectF area(0, 4, plotW, height() - 8);
    const QColor textColor = palette().color(QPalette::Text);

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

    double lo = 0, span = 1;
    if (any) {
        span = high - low;
        lo = low;
        if (span < 1.0) { lo -= (1.0 - span) / 2; span = 1.0; }
        lo -= span * 0.05;   // a little room above and below
        span *= 1.1;
    }

    // A grid that can be seen on any theme, with what each line is worth in dB in the margin on the right
    QColor gridColor = textColor;
    gridColor.setAlpha(70);
    QFont axisFont = font();
    axisFont.setPointSizeF(qMax(7.0, axisFont.pointSizeF() - 1));
    for (qint32 i = 0; i <= 4; i++) {
        const qreal y = area.top() + area.height() * i / 4.0;
        painter.setPen(QPen(gridColor, 1));
        painter.drawLine(QPointF(0, y), QPointF(plotW, y));
        if (any && i > 0 && i < 4) {
            const double value = lo + (area.bottom() - y) / area.height() * span;
            painter.setFont(axisFont);
            painter.setPen(textColor);
            painter.drawText(QRectF(plotW + 4, y - 8, AXIS_WIDTH - 6, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QString("%1 dB").arg(value, 0, 'f', 1));
        }
    }
    painter.setPen(QPen(gridColor, 1));
    painter.drawLine(QPointF(plotW, 0), QPointF(plotW, height()));

    if (totalFrames > 0 && any) {
        const auto toPoint = [&](qint32 index, float value) {
            const qreal x = totalFrames > 1 ? static_cast<qreal>(index) * (plotW - 1) / (totalFrames - 1) : 0;
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

    // The frame viewed
    const auto xOf = [&](qint32 frame) {
        return totalFrames > 1 ? (frame - 1) * (plotW - 1) / static_cast<qreal>(totalFrames - 1) : 0.0;
    };
    if (totalFrames > 0 && markerFrame >= 1) {
        painter.setPen(QPen(textColor, 2));
        painter.drawLine(QPointF(xOf(markerFrame), 0), QPointF(xOf(markerFrame), height()));
    }

    // The values under the cursor
    if (totalFrames > 0 && hoverFrame >= 1) {
        QColor hoverColor = textColor;
        hoverColor.setAlpha(140);
        painter.setPen(QPen(hoverColor, 1, Qt::DashLine));
        painter.drawLine(QPointF(xOf(hoverFrame), 0), QPointF(xOf(hoverFrame), height()));

        QStringList lines;
        lines << tr("Frame %1").arg(hoverFrame);
        for (const SnrCurve &curve: curves) {
            if (!curve.visible || hoverFrame > curve.values.size() || curve.name.isEmpty()) continue;
            const float value = curve.values[hoverFrame - 1];
            lines << (std::isfinite(value) ? tr("%1: %2 dB").arg(curve.name).arg(value, 0, 'f', 2) : tr("%1: -").arg(curve.name));
        }

        painter.setFont(font());
        const QFontMetrics metrics(font());
        qint32 boxWidth = 0;
        for (const QString &line: lines) boxWidth = qMax(boxWidth, metrics.horizontalAdvance(line));
        boxWidth += 12;
        const qint32 boxHeight = metrics.height() * lines.size() + 8;
        // On the side of the cursor that has the room
        const qreal x = xOf(hoverFrame);
        const qreal boxX = x + 10 + boxWidth < plotW ? x + 10 : x - 10 - boxWidth;
        const QRectF box(boxX, 4, boxWidth, boxHeight);
        QColor fill = palette().color(QPalette::Window);
        fill.setAlpha(225);
        painter.setPen(QPen(textColor, 1));
        painter.setBrush(fill);
        painter.drawRect(box);
        painter.setPen(textColor);
        for (qint32 i = 0; i < lines.size(); i++) {
            painter.drawText(QRectF(box.left() + 6, box.top() + 4 + i * metrics.height(), boxWidth, metrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
        }
    }
}

void SnrPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) emit frameRequested(frameAt(static_cast<qint32>(event->position().x())));
}

void SnrPlot::mouseMoveEvent(QMouseEvent *event)
{
    const qint32 x = static_cast<qint32>(event->position().x());
    hoverFrame = (x >= 0 && x < plotWidth()) ? frameAt(x) : 0;
    update();
    if (event->buttons() & Qt::LeftButton) emit frameRequested(frameAt(x));
}

void SnrPlot::leaveEvent(QEvent *)
{
    hoverFrame = 0;
    update();
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
    // What the graph is of, beside the check boxes and not on the curves
    titleLabel = new QLabel;
    titleLabel->setForegroundRole(QPalette::PlaceholderText);
    row->addWidget(titleLabel);
    layout->addLayout(row);

    plot = new SnrPlot;
    layout->addWidget(plot, 1);

    connect(plot, &SnrPlot::frameRequested, this, &SnrGraph::frameRequested);
    connect(whiteCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
    connect(blackCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
    if (stackCheck) connect(stackCheck, &QCheckBox::toggled, this, [this]() { redraw(); });
}

void SnrGraph::setData(const QString &label, const QVector<float> &newWhite, const QVector<float> &newBlack,
                       const QVector<float> &newStackWhite, const QVector<float> &newStackBlack)
{
    titleLabel->setText(label.isEmpty() ? tr("dB") : tr("%1 (dB)").arg(label));
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
        curves.append({ stackWhite, whiteColor(), true, whiteCheck->isChecked(), tr("Stack white") });
        curves.append({ stackBlack, blackColor(), true, blackCheck->isChecked(), tr("Stack black") });
    }
    curves.append({ white, whiteColor(), false, whiteCheck->isChecked(), tr("White SNR") });
    curves.append({ black, blackColor(), false, blackCheck->isChecked(), tr("Black PSNR") });
    plot->setCurves(curves);
}
