/******************************************************************************
 * waveformpanel.h
 * ld-analyse - the waveform of the audio, with the controls of what is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef WAVEFORMPANEL_H
#define WAVEFORMPANEL_H

#include <QColor>
#include <QWidget>

#include "audioreader.h"

class AudioEngine;
class QComboBox;
class QSlider;
class QToolButton;

// The shape of the audio, the left channel above the right, with a line where the video is; a click on it asks to go there.
class WaveformView : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformView(AudioEngine *engine, QWidget *parent = nullptr);
    void setPlayheadSeconds(double videoSeconds);
    void setColor(const QColor &colour);
    void setMessage(const QString &text) { message = text; update(); }
    QSize minimumSizeHint() const override { return QSize(120, 60); }

signals:
    void seekRequested(double videoSeconds);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    double secondsAtX(qint32 x) const;
    AudioEngine *engine;
    double playhead = 0.0;
    QColor colour;
    QString message;
};

// The waveform with, on its left, what is played: which audio (analogue, decoded from EFM...), which track of the file, the
// channels that are on and the volume. The controls can be hidden: the same are in the audio configuration.
class WaveformPanel : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformPanel(AudioEngine *engine, QWidget *parent = nullptr);

    // The audio the capture has: fills the choices
    void setTracks(const QVector<AudioTrackInfo> &tracks, qint32 chosen);
    void setPlayheadSeconds(double videoSeconds) { view->setPlayheadSeconds(videoSeconds); }
    void setColor(const QColor &colour) { view->setColor(colour); }
    void setMessage(const QString &text) { view->setMessage(text); }
    void setControlsVisible(bool visible);
    bool controlsVisible() const { return controls->isVisible(); }

    bool leftOn() const;
    bool rightOn() const;
    qint32 volume() const;                  // 0 to 100
    void setMix(bool left, bool right, qint32 volume);
    void refresh() { view->update(); }

signals:
    void trackSelected(qint32 index);
    void mixChanged(bool left, bool right, qint32 volume);
    void seekRequested(double videoSeconds);

private:
    void sourceChosen();
    void emitMix();

    QWidget *controls;
    WaveformView *view;
    QComboBox *sourceCombo;
    QComboBox *trackCombo;
    QToolButton *leftButton;
    QToolButton *rightButton;
    QSlider *volumeSlider;
    QVector<AudioTrackInfo> allTracks;
    bool filling = false;
};

#endif // WAVEFORMPANEL_H
