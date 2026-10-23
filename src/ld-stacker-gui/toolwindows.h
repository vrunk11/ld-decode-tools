/******************************************************************************
 * toolwindows.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef TOOLWINDOWS_H
#define TOOLWINDOWS_H

#include <QDialog>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMap>
#include <QVector>

#include "snrplot.h"

// The SNR of one source over the whole disc, with the stack's behind it to compare
class SnrWindow : public QDialog
{
    Q_OBJECT

public:
    explicit SnrWindow(QWidget *parent = nullptr);

    // The SNR of a source, a value per frame of the timeline (NaN where it has none), with the stack's behind
    void setContent(const QString &title, const QVector<float> &white, const QVector<float> &black,
                    const QVector<float> &stackWhite, const QVector<float> &stackBlack, qint32 marker);
    // The colour of the source, which tells its window from the others: a bar at the top, and the window's icon
    void setAccent(const QColor &color);
    void setMarker(qint32 frame);
    void setMeasure(bool on, bool enabled);
    void setBridgeGaps(bool bridge);
    void setRange(bool fixed, double low, double high);

signals:
    void frameRequested(qint32 frame);
    void measureToggled(bool on);

private:
    SnrGraph *graph = nullptr;
    QFrame *accentBar = nullptr;
};

// The final stack in a window of its own, so that it stays in view while the main window shows a source
class StackOutputWindow : public QDialog
{
    Q_OBJECT

public:
    explicit StackOutputWindow(QWidget *parent = nullptr);

    void setPicture(const QImage &image, const QString &caption);
    void setMessage(const QString &message);

signals:
    void closed();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void showScaled();

    QLabel *label = nullptr;
    QLabel *caption = nullptr;
    QImage image;
};

// The dropouts of one source in a window of its own, as the SNR of a source has: the total length of the dropouts of each frame along the timeline, on the
// scale that is the same for every graph of the dropouts, and, when it was measured, what the stack is left with
class DropoutWindow : public QDialog
{
    Q_OBJECT

public:
    explicit DropoutWindow(QWidget *parent = nullptr);

    void setContent(const QString &title, const QVector<SnrCurve> &curves, qint32 marker);
    // The colour of the source: a bar at the top, and the window's icon
    void setAccent(const QColor &color);
    void setMarker(qint32 frame);
    void setRange(bool fixed, double low, double high);

signals:
    void frameRequested(qint32 frame);

private:
    DropoutGraph *graph = nullptr;
    QFrame *accentBar = nullptr;
};

#endif // TOOLWINDOWS_H
