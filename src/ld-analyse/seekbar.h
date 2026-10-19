/******************************************************************************
 * seekbar.h
 * ld-analyse - the bar of the position of the picture, to go through the capture with the mouse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SEEKBAR_H
#define SEEKBAR_H

#include <QSlider>

// A slider that goes to the place that is clicked (the usual one moves a page toward it), and follows the mouse while it is held. It says
// what the person did (scrubbed), and what the owner sets with setValue is not said back.
class SeekBar : public QSlider
{
    Q_OBJECT
public:
    explicit SeekBar(QWidget *parent = nullptr);
    bool isScrubbing() const { return scrubbing; }
    // Forget a drag that never got its mouse release (a window took the mouse): the bar is not held any more
    void cancelScrub();

signals:
    void scrubbed(qint32 value);       // the person moved it (a click, or a drag)
    void scrubFinished();              // the mouse was let go

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    bool scrubbing = false;
    qint32 valueAt(const QPoint &point) const;
    void follow(const QPoint &point);
};

#endif // SEEKBAR_H
