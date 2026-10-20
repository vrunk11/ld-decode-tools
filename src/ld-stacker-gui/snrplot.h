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
#include <QLabel>
#include <QString>
#include <QVector>
#include <QWidget>

// One curve of the plot: a value per frame (index 0 is frame 1), NaN for a frame without one
struct SnrCurve {
    QVector<float> values;
    QColor color;
    bool thin = false;       // drawn finer, behind the others (what is compared with)
    bool visible = true;
    QString name;            // as the value under the cursor calls it
};

// SNR curves over the frames, in dB, with the scale on the right, a marker on the frame being viewed and the
// values under the cursor. Clicking or dragging on it asks for a frame.
class SnrPlot : public QWidget
{
    Q_OBJECT

public:
    explicit SnrPlot(QWidget *parent = nullptr);

    void setCurves(const QVector<SnrCurve> &curves);
    void setMarker(qint32 frame);
    // The range of the vertical scale, in dB: the one given (the same for all the graphs, so that they can be compared) or, when it is not fixed, the
    // one of the curves that are shown. A little room is left above and below either way.
    void setRange(bool fixed, double low = 0, double high = 0);

    QSize sizeHint() const override { return QSize(400, 70); }
    QSize minimumSizeHint() const override { return QSize(100, 40); }

signals:
    void frameRequested(qint32 frame);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool fixedRange = false;
    double fixedLow = 0, fixedHigh = 0;
    static constexpr qint32 AXIS_WIDTH = 56;     // room on the right for the scale

    qint32 plotWidth() const { return qMax(1, width() - AXIS_WIDTH); }
    qint32 frameAt(qint32 x) const;

    QVector<SnrCurve> curves;
    qint32 totalFrames = 0;
    qint32 markerFrame = 0;
    qint32 hoverFrame = 0;   // under the cursor, 0 if it is not on the plot
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
    // The box that says the SNR of the source is measured from its fields instead of read from its metadata: whether it is ticked, whether it can be
    // changed (the SNR of a stack is always measured), and what its tooltip says (empty: the usual one)
    void setMeasure(bool on, bool enabled, const QString &tip = QString());
    void setRange(bool fixed, double low = 0, double high = 0);

    static QColor whiteColor() { return QColor("#378ADD"); }
    static QColor blackColor() { return QColor("#E8833A"); }

signals:
    void frameRequested(qint32 frame);
    void measureToggled(bool on);       // the person ticked or cleared it

private:
    void redraw();

    SnrPlot *plot = nullptr;
    QLabel *titleLabel = nullptr;
    QCheckBox *whiteCheck = nullptr;
    QCheckBox *blackCheck = nullptr;
    QCheckBox *stackCheck = nullptr;
    QCheckBox *measureCheck = nullptr;
    QVector<float> white, black, stackWhite, stackBlack;
};

#endif // SNRPLOT_H
