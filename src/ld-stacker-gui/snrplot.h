/******************************************************************************
 * snrplot.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SNRPLOT_H
#define SNRPLOT_H

#include <QColor>
#include <QMap>
#include <QVector>
#include <QWidget>

// The SNR over the frames, as a line, with a marker on the frame being viewed. Clicking or
// dragging on it asks for a frame.
class SnrPlot : public QWidget
{
    Q_OBJECT

public:
    explicit SnrPlot(QWidget *parent = nullptr);

    // A value per frame (index 0 is frame 1), NaN for a frame without one
    void setSeries(const QVector<float> &values, const QColor &color, const QString &label);
    // Only some frames known (the stack is measured as it is viewed)
    void setSparseSeries(const QMap<qint32, float> &values, qint32 totalFrames, const QColor &color, const QString &label);
    void setMarker(qint32 frame);

    QSize sizeHint() const override { return QSize(400, 70); }
    QSize minimumSizeHint() const override { return QSize(100, 50); }

signals:
    void frameRequested(qint32 frame);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    qint32 frameAt(qint32 x) const;

    QVector<QPointF> points;   // frame, value
    qint32 totalFrames = 0;
    QColor lineColor;
    QString text;
    qint32 markerFrame = 0;
    double low = 0, high = 1;
    bool sparse = false;
};

#endif // SNRPLOT_H
