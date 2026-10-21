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
#include <QComboBox>
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
    bool bridge = false;     // a sparse curve (a rough measure): the points that there are are joined across the frames that have none, and marked
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
    // What the scale is in: the unit written after the values (none: the number only) and how many decimals they have (dB: 1; a count: 0)
    void setUnit(const QString &unit, qint32 decimals);

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
    QString unit = QStringLiteral("dB");
    qint32 decimals = 1;
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
    // How the measure goes, shown beside the box Measure when it is ticked (the main graph): 0 rough (one frame in five seconds of video, and more as the person
    // seeks), 1 full, 2 seek (only the frames that the person goes to)
    void setMeasureMode(qint32 mode);
    // The curves of the SNR are joined across the frames that have none (a measure that is not of every frame)
    void setBridgeGaps(bool bridge);
    void setRange(bool fixed, double low = 0, double high = 0);
    // The box that shows the dropout graph (the same as View > Dropout graph), when this graph has it: set without sending dropoutsToggled
    void setDropoutsChecked(bool on);
    // The height of the plot, apart from the row of boxes (the plot goes away when neither the white SNR nor the black PSNR is ticked)
    void setPlotFixedHeight(qint32 height);

    static QColor whiteColor() { return QColor("#378ADD"); }
    static QColor blackColor() { return QColor("#E8833A"); }

signals:
    void frameRequested(qint32 frame);
    void measureToggled(bool on);       // the person ticked or cleared it
    void dropoutsToggled(bool on);
    void measureModeChanged(qint32 mode);
    void plotHeightChanged(qint32 delta);     // the plot came (+) or went (-): by how many pixels the graph grew or shrank

private:
    void redraw();
    void updatePlotVisibility();

    SnrPlot *plot = nullptr;
    QLabel *titleLabel = nullptr;
    QCheckBox *whiteCheck = nullptr;
    QCheckBox *blackCheck = nullptr;
    QCheckBox *stackCheck = nullptr;
    QCheckBox *measureCheck = nullptr;
    QComboBox *stackModeCombo = nullptr;
    bool bridgeGaps = false;
    QCheckBox *dropoutCheck = nullptr;
    QVector<float> white, black, stackWhite, stackBlack;
};

// The graph of the dropouts of the captures: the total length of the dropouts of each frame (the metadata of the captures), one curve for each source in
// its colour, along the same frames as the SNR graph and on a scale that is the same for all the sources
class DropoutGraph : public QWidget
{
    Q_OBJECT

public:
    explicit DropoutGraph(QWidget *parent = nullptr);

    void setData(const QString &title, const QVector<SnrCurve> &curves);
    void setMarker(qint32 frame);
    void setRange(bool fixed, double low = 0, double high = 0);

signals:
    void frameRequested(qint32 frame);

private:
    SnrPlot *plot = nullptr;
    QLabel *titleLabel = nullptr;
};

#endif // SNRPLOT_H
