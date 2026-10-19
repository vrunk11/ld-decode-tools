/******************************************************************************
 * pintitlebar.h
 * ld-analyse - the title bar of the floating windows: a title, a pin and a close button
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PINTITLEBAR_H
#define PINTITLEBAR_H

#include <QAbstractButton>
#include <QWidget>

class QLabel;

// A button that is a drawing pin: upright and lit when it is pressed in (pinned), tilted and dim when it is not
class PinButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit PinButton(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(22, 22); }

protected:
    void paintEvent(QPaintEvent *event) override;
};

// The bar at the top of a window that has no frame of the system: it has the title, the pin (what it means is the window's: here it is that
// the window stays when the other controls go away) and a button that closes the window. Dragging the bar moves the window.
class PinTitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit PinTitleBar(const QString &title, QWidget *parent = nullptr);

    bool pinned() const;
    void setPinned(bool on);
    void setPinToolTip(const QString &text);

signals:
    void pinToggled(bool pinned);
    void closeRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    PinButton *pin;
    QLabel *titleLabel;
    bool dragging = false;
    QPoint dragFrom;
};

#endif // PINTITLEBAR_H
