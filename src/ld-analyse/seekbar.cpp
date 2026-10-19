/******************************************************************************
 * seekbar.cpp
 * ld-analyse - the bar of the position of the picture, to go through the capture with the mouse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "seekbar.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

SeekBar::SeekBar(QWidget *parent) : QSlider(Qt::Horizontal, parent)
{
    setFocusPolicy(Qt::NoFocus);
}

// The value at a point of the bar: the middle of the handle goes to the point, from one end of the groove to the other
qint32 SeekBar::valueAt(const QPoint &point) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const qint32 span = qMax(1, groove.width() - handle.width());
    return QStyle::sliderValueFromPosition(minimum(), maximum(), point.x() - groove.x() - handle.width() / 2, span, option.upsideDown);
}

void SeekBar::follow(const QPoint &point)
{
    const qint32 now = valueAt(point);
    if (now == value()) return;
    setValue(now);
    emit scrubbed(now);
}

void SeekBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    scrubbing = true;
    setSliderDown(true);
    const qint32 now = valueAt(event->position().toPoint());
    setValue(now);
    emit scrubbed(now);
    event->accept();
}

void SeekBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!scrubbing) return;
    follow(event->position().toPoint());
    event->accept();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !scrubbing) return;
    scrubbing = false;
    setSliderDown(false);
    emit scrubFinished();
    event->accept();
}

void SeekBar::cancelScrub()
{
    if (!scrubbing) return;
    scrubbing = false;
    setSliderDown(false);
    emit scrubFinished();
}
