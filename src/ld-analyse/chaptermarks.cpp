/******************************************************************************
 * chaptermarks.cpp
 * ld-analyse - small marks on a slider where the chapters begin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "chaptermarks.h"

#include <QEvent>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

ChapterMarks::ChapterMarks(QSlider *parentSlider) : QWidget(parentSlider), slider(parentSlider)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(slider->rect());
    slider->installEventFilter(this);
    // (the range changes as a capture is loaded, or the view changes: the marks are drawn again)
    connect(slider, &QSlider::rangeChanged, this, [this]() { update(); });
    show();
}

void ChapterMarks::setMarks(const QVector<qint32> &newValues)
{
    if (newValues == values) return;
    values = newValues;
    update();
}

// The groove of the slider, and the width of its handle, as the style of the slider has them
QRect ChapterMarks::grooveRect(qint32 &handleWidth) const
{
    QStyleOptionSlider option;
    option.initFrom(slider);
    option.orientation = slider->orientation();
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    option.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
    const QRect groove = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, slider);
    const QRect handle = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
    handleWidth = handle.width();
    return groove;
}

QVector<qint32> ChapterMarks::markPositions() const
{
    QVector<qint32> positions;
    if (slider->maximum() <= slider->minimum()) return positions;
    qint32 handleWidth = 0;
    const QRect groove = grooveRect(handleWidth);
    const qint32 span = qMax(1, groove.width() - handleWidth);
    for (const qint32 value: values) {
        if (value < slider->minimum() || value > slider->maximum()) continue;
        positions.append(groove.x() + handleWidth / 2 + QStyle::sliderPositionFromValue(slider->minimum(), slider->maximum(), value, span));
    }
    return positions;
}

void ChapterMarks::paintEvent(QPaintEvent *)
{
    const QVector<qint32> positions = markPositions();
    if (positions.isEmpty()) return;
    qint32 handleWidth = 0;
    const QRect groove = grooveRect(handleWidth);
    QPainter painter(this);
    // A small line across the groove, a little longer than it is thick, in the colour of the text of the slider
    QColor ink = palette().color(QPalette::WindowText);
    ink.setAlpha(190);
    painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::FlatCap));
    const qint32 middle = groove.center().y();
    const qint32 half = qMax(4, groove.height() / 2 + 2);
    for (const qint32 x: positions) painter.drawLine(x, middle - half, x, middle + half);
}

bool ChapterMarks::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == slider && event->type() == QEvent::Resize) setGeometry(slider->rect());
    return QWidget::eventFilter(watched, event);
}
