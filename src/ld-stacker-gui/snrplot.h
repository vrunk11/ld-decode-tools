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

#include <QCheckBox>
#include <QColor>
#include <QVector>
#include <QWidget>

// One curve of the plot: a value per frame (index 0 is frame 1), NaN for a frame without one
struct SnrCurve {
    QVector<float> values;
    QColor color;
    bool thin = false;       // drawn finer, behind the others (what is compared with)
    bool visible = true;
};

// SNR curves over the frames, with a marker on the frame being viewed. Clicking or dragging on it asks for a
// frame.
class SnrPlot : public QWidget
{
    Q_OBJECT

public:
    explicit SnrPlot(QWidget *parent = nullptr);

    void setCurves(const QVector<SnrCurve> &curves, const QString &label);
    void setMarker(qint32 frame);

    QSize sizeHint() const override { return QSize(400, 70); }
    QSize minimumSizeHint() const override { return QSize(100, 40); }

signals:
    void frameRequested(qint32 frame);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    qint32 frameAt(qint32 x) const;

    QVector<SnrCurve> curves;
    qint32 totalFrames = 0;
    QString text;
    qint32 markerFrame = 0;
};

// The plot with what is on it to switch: the white SNR and the black PSNR, each its own colour, and, in the
// window of a source, the stack's behind them
class SnrGraph : public QWidget
{
    Q_OBJECT

public:
    explicit SnrGraph(bool withStack, QWidget *parent = nullptr);

    // The SNR of a source or of the stack; stack* are those of the stack, drawn behind (the window of a source)
    void setData(const QString &label, const QVector<float> &white, const QVector<float> &black,
                 const QVector<float> &stackWhite = QVector<float>(), const QVector<float> &stackBlack = QVector<float>());
    void setMarker(qint32 frame);

    static QColor whiteColor() { return QColor("#378ADD"); }
    static QColor blackColor() { return QColor("#E8833A"); }

signals:
    void frameRequested(qint32 frame);

private:
    void redraw();

    SnrPlot *plot = nullptr;
    QCheckBox *whiteCheck = nullptr;
    QCheckBox *blackCheck = nullptr;
    QCheckBox *stackCheck = nullptr;
    QString label;
    QVector<float> white, black, stackWhite, stackBlack;
};

#endif // SNRPLOT_H
