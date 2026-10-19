/******************************************************************************
 * chaptermarks.h
 * ld-analyse - small marks on a slider where the chapters begin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHAPTERMARKS_H
#define CHAPTERMARKS_H

#include <QVector>
#include <QWidget>

class QSlider;

// A transparent layer over a slider that draws a small mark at each place of its range that is given (the start of a chapter). It is a child of
// the slider, takes its size, and lets the mouse go through to it. The slider is not changed.
class ChapterMarks : public QWidget
{
    Q_OBJECT
public:
    explicit ChapterMarks(QSlider *slider);

    // The places, as values of the slider (those out of its range are left out)
    void setMarks(const QVector<qint32> &values);
    const QVector<qint32> &marks() const { return values; }
    // Where the marks are drawn, in pixels from the left of the slider (for the marks that are in its range), in the order they were given
    QVector<qint32> markPositions() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QSlider *slider;
    QVector<qint32> values;
    QRect grooveRect(qint32 &handleWidth) const;
};

#endif // CHAPTERMARKS_H
