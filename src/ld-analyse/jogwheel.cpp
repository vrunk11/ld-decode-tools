/******************************************************************************
 * jogwheel.cpp
 * ld-analyse - the jog and shuttle wheel of the remote control
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "jogwheel.h"

#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>

JogWheel::JogWheel(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(false);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Turn it with the mouse (or its wheel). In the jog mode a step of the turn is a picture; "
                  "in the shuttle mode the angle from the centre is the speed, and it stops when it is let go."));
    springBack = new QVariantAnimation(this);
    springBack->setDuration(180);
    springBack->setEasingCurve(QEasingCurve::OutCubic);
    connect(springBack, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        shown = value.toDouble();
        update();
    });
}

void JogWheel::setJogMode(bool newJog)
{
    if (newJog == jog) return;
    jog = newJog;
    // (the shuttle ring is at its centre when it is taken: nothing of the other mode goes on)
    if (dragging) {
        dragging = false;
        setCursor(Qt::OpenHandCursor);
        if (travelled != 0.0) emit shuttled(0.0);
    }
    travelled = 0.0;
    pending = 0.0;
    shown = 0.0;
    update();
}

QSize JogWheel::sizeHint() const
{
    return QSize(170, 150);
}

QSize JogWheel::minimumSizeHint() const
{
    return QSize(110, 100);
}

QPointF JogWheel::centre() const
{
    return QPointF(width() / 2.0, height() / 2.0 + 4);
}

double JogWheel::radius() const
{
    return qMin(width() / 2.0 - 4, height() / 2.0 - 8);
}

// The angle of a point round the centre, in degrees, clockwise from the top
double JogWheel::angleAt(const QPointF &point) const
{
    const QPointF from = point - centre();
    return qRadiansToDegrees(qAtan2(from.x(), -from.y()));
}

void JogWheel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    springBack->stop();
    dragging = true;
    lastAngle = angleAt(event->position());
    setCursor(Qt::ClosedHandCursor);
}

void JogWheel::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging) return;
    const QPointF from = event->position() - centre();
    // (at the very centre a direction means nothing)
    if (qAbs(from.x()) + qAbs(from.y()) < 6) return;
    const double angle = angleAt(event->position());
    double delta = angle - lastAngle;
    while (delta > 180.0) delta -= 360.0;
    while (delta <= -180.0) delta += 360.0;
    lastAngle = angle;

    if (jog) {
        shown += delta;
        pending += delta;
        const qint32 steps = static_cast<qint32>(pending / DEGREES_PER_STEP);
        if (steps != 0) {
            pending -= steps * DEGREES_PER_STEP;
            emit jogged(steps);
        }
    } else {
        const double now = qBound(-SHUTTLE_TRAVEL, travelled + delta, SHUTTLE_TRAVEL);
        if (now != travelled) {
            travelled = now;
            shown = travelled;
            emit shuttled(travelled / SHUTTLE_TRAVEL);
        }
    }
    update();
}

void JogWheel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !dragging) return;
    dragging = false;
    setCursor(Qt::OpenHandCursor);
    if (!jog) {
        // The shuttle ring springs back to its centre, and what it was showing stops
        travelled = 0.0;
        springBack->setStartValue(shown);
        springBack->setEndValue(0.0);
        springBack->start();
        emit shuttled(0.0);
    }
}

void JogWheel::wheelEvent(QWheelEvent *event)
{
    // A notch of the wheel of the mouse is a step (a picture); the shuttle ring has no steps, so it is the jog then too
    const qint32 notches = event->angleDelta().y() / 120;
    if (notches == 0) return;
    shown += notches * DEGREES_PER_STEP;
    emit jogged(notches);
    update();
    event->accept();
}

void JogWheel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF middle = centre();
    const double outer = radius();
    if (outer < 10) return;

    // The labels of the two ways: reverse on the left, forward on the right
    painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
    QFont small = font();
    small.setPointSizeF(qMax(6.0, small.pointSizeF() * 0.8));
    painter.setFont(small);
    painter.drawText(QRectF(0, 0, width() / 2.0, 14), Qt::AlignLeft | Qt::AlignVCenter, tr(" REVERSE"));
    painter.drawText(QRectF(width() / 2.0, 0, width() / 2.0, 14), Qt::AlignRight | Qt::AlignVCenter, tr("FORWARD "));

    // The wheel: a dark disc with a ridge of ticks that turns
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(28, 28, 30));
    painter.drawEllipse(middle, outer, outer);
    painter.setBrush(QColor(52, 52, 56));
    painter.drawEllipse(middle, outer * 0.94, outer * 0.94);
    painter.setBrush(QColor(34, 34, 36));
    painter.drawEllipse(middle, outer * 0.86, outer * 0.86);

    painter.save();
    painter.translate(middle);
    painter.rotate(shown);
    painter.setPen(QPen(QColor(78, 78, 84), 1.4));
    for (qint32 tick = 0; tick < 360; tick += 6) {
        painter.save();
        painter.rotate(tick);
        painter.drawLine(QPointF(0, -outer * 0.87), QPointF(0, -outer * 0.93));
        painter.restore();
    }
    // The dimple that a finger goes to
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 22));
    painter.drawEllipse(QPointF(0, -outer * 0.58), outer * 0.17, outer * 0.17);
    painter.setBrush(QColor(60, 60, 64));
    painter.drawEllipse(QPointF(0, -outer * 0.58), outer * 0.13, outer * 0.13);
    painter.restore();

    // The mark of the centre, fixed
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Highlight));
    const QPointF mark[3] = { QPointF(middle.x(), middle.y() - outer - 3), QPointF(middle.x() - 4, middle.y() - outer - 9),
                              QPointF(middle.x() + 4, middle.y() - outer - 9) };
    painter.drawPolygon(mark, 3);
}
