/******************************************************************************
 * pintitlebar.cpp
 * ld-analyse - the title bar of the floating windows: a title, a pin and a close button
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "pintitlebar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

PinButton::PinButton(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
}

void PinButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // The button: a box that is a little lit under the mouse, and the colour of the accent when it is pressed in
    const bool on = isChecked();
    QColor box(255, 255, 255, underMouse() ? 55 : 24);
    if (on) box = QColor(0xf2, 0xb8, 0x4a);
    painter.setPen(QPen(on ? QColor(0xf2, 0xb8, 0x4a) : QColor(255, 255, 255, underMouse() ? 140 : 80), 1));
    painter.setBrush(box);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 4, 4);

    // The pin: a head, a bar under it and the point; upright when pinned, tilted when not
    painter.translate(width() / 2.0, height() / 2.0);
    if (!on) painter.rotate(40);
    const QColor ink = on ? QColor(0x10, 0x10, 0x12) : QColor(230, 230, 234, 200);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawRoundedRect(QRectF(-3.5, -7, 7, 3.2), 1, 1);       // (the head)
    const QPointF body[4] = { QPointF(-2.2, -3.8), QPointF(2.2, -3.8), QPointF(3.4, 0.8), QPointF(-3.4, 0.8) };
    painter.drawPolygon(body, 4);
    painter.drawRoundedRect(QRectF(-4.6, 0.8, 9.2, 1.6), 0.8, 0.8);   // (the bar)
    painter.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(0, 2.4), QPointF(0, 7));              // (the point)
}

PinTitleBar::PinTitleBar(const QString &title, QWidget *parent) : QWidget(parent)
{
    setObjectName("pinTitleBar");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("QWidget#pinTitleBar { background: rgba(255, 255, 255, 18); border-top-left-radius: 5px; border-top-right-radius: 5px; }"
                  "QLabel { color: #b8b8be; background: transparent; font-size: 11px; font-weight: 600; }"
                  "QToolButton { color: #b8b8be; background: transparent; border: none; font-size: 15px; padding: 0 4px; }"
                  "QToolButton:hover { color: #ffffff; background: rgba(220, 60, 60, 190); border-radius: 4px; }");
    QHBoxLayout *row = new QHBoxLayout(this);
    row->setContentsMargins(6, 3, 4, 3);
    row->setSpacing(6);
    pin = new PinButton(this);
    titleLabel = new QLabel(title, this);
    QToolButton *close = new QToolButton(this);
    close->setText(QString(QChar(0x2715)));
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close"));
    row->addWidget(pin);
    row->addWidget(titleLabel, 1);
    row->addWidget(close);
    connect(pin, &QAbstractButton::toggled, this, &PinTitleBar::pinToggled);
    connect(close, &QToolButton::clicked, this, &PinTitleBar::closeRequested);
}

bool PinTitleBar::pinned() const
{
    return pin->isChecked();
}

void PinTitleBar::setPinned(bool on)
{
    pin->setChecked(on);
}

void PinTitleBar::setPinToolTip(const QString &text)
{
    pin->setToolTip(text);
}

// The bar is the handle of the window: it goes where the mouse takes it
void PinTitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    dragging = true;
    dragFrom = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
    event->accept();
}

void PinTitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging) return;
    window()->move(event->globalPosition().toPoint() - dragFrom);
    event->accept();
}

void PinTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) dragging = false;
}
