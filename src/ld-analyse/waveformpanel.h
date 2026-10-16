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
#include <QFrame>
#include <functional>
#include <QWidget>

#include "audioreader.h"

class AudioEngine;
class QBoxLayout;
class QHBoxLayout;
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
    // A colour of its own, or none (an invalid colour): then the theme's, for the analogue audio or for the audio from EFM
    void setColor(const QColor &colour);
    enum class Style { Filled = 0, Outline = 1, Bars = 2 };
    void setStyle(Style newStyle) { style = newStyle; update(); }
    void setMessage(const QString &text) { message = text; update(); }

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
    Style style = Style::Filled;
    QString message;
};

// The waveform with, on its left, what is played: which audio (analogue, decoded from EFM...), which track of the file, the
// channels that are on and the volume. The controls can be hidden: the same are in the audio configuration.
class WaveformPanel : public QFrame
{
    Q_OBJECT
public:
    explicit WaveformPanel(AudioEngine *engine, QWidget *parent = nullptr);

    // The audio the capture has: fills the choices
    void setTracks(const QVector<AudioTrackInfo> &tracks, qint32 chosen);
    void setPlayheadSeconds(double videoSeconds) { view->setPlayheadSeconds(videoSeconds); }
    void setColor(const QColor &colour) { view->setColor(colour); }
    void setStyle(WaveformView::Style style) { view->setStyle(style); }
    void setWell(bool on);
    // How much height the panel takes: Large, Compact (the default, a little more than the buttons of the playback bar) and Ultra
    // (one line of buttons: the controls beside the waveform in a single row)
    enum class Density { Large = 0, Compact = 1, Ultra = 2 };
    void setDensity(Density density);
    bool muted() const;
    void setMuted(bool muted);
    void setMessage(const QString &text) { view->setMessage(text); }
    // Whether the EFM may be decoded (asked when the source list is made): the source EFM digital is then there to be chosen
    void setEfmPossible(std::function<bool()> possible) { efmPossible = std::move(possible); }
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
    void mutedChanged(bool muted);
    void efmDecodeRequested();
    void seekRequested(double videoSeconds);

private:
    void sourceChosen();
    void emitMix();

    QWidget *controls;
    QFrame *separator = nullptr;
    bool controlsOn = true;
    bool wellOn = true;
    WaveformView *view;
    QFrame *well = nullptr;
    QComboBox *sourceCombo;
    QComboBox *trackCombo;
    QToolButton *leftButton;
    QToolButton *rightButton;
    QToolButton *muteButton;
    QBoxLayout *column = nullptr;
    QHBoxLayout *mixRow = nullptr;
    QSlider *volumeSlider;
    QVector<AudioTrackInfo> allTracks;
    bool filling = false;
    qint32 shownSource = -1;
    std::function<bool()> efmPossible;
};

#endif // WAVEFORMPANEL_H
