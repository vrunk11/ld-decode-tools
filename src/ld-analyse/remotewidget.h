/******************************************************************************
 * remotewidget.h
 * ld-analyse - the remote control of a laserdisc player, to drive the playback of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef REMOTEWIDGET_H
#define REMOTEWIDGET_H

#include <QVector>
#include <QWidget>

class JogWheel;
class QLabel;
class QPushButton;

// The keys of the remote and its display. It decides nothing: a key press is a signal, and the owner says what is lit and what the
// display shows. It is one widget that is in a window of its own, or over the picture when that fills the screen.
class RemoteWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RemoteWidget(QWidget *parent = nullptr);

    // What the display shows: the way of searching, the number being typed, and the state (speed, play...)
    void setDisplay(const QString &mode, const QString &entry, const QString &state);
    void setJogMode(bool jog);
    void setDisplayOn(bool on);
    void setMemoryStored(bool stored);
    void setStillWithSound(bool on);
    void setRepeatPoints(bool aSet, bool bSet);
    // The audio: whether each of the two (digital: decoded from the EFM, analogue) can be had, and which is heard
    void setAudioChoice(bool digitalAvailable, bool analogueAvailable, bool digital);
    // The transport that is going: "play", "pause", "scan" or "" (nothing yet)
    void setTransport(const QString &state);
    // The side of the disc that the VBI says (0: A, 1: B, -1: not known), and whether it says CX
    void setSide(qint32 side);
    void setCx(bool on);
    // Without a capture the keys that act on it are off
    void setSourceLoaded(bool loaded);
    // Whether the disc has the chapters, the picture numbers, the times that a search can use (the keys are lit for what it has)
    void setSearchAvailable(bool chapters, bool pictures, bool times);
    // The picture without the colour decoded (lit when it is raw), the sound (lit when it is muted) and the view of the picture (its name: FRAME, SPLIT, FIELD...)
    void setRaw(bool raw);
    void setMuted(bool muted);
    void setViewMode(const QString &name);
    // The ffmpeg filters: lit when they are on, off when the build has none
    void setFilter(bool on, bool available);

signals:
    void digitPressed(qint32 digit);
    void plusTenPressed();
    void clearPressed();
    void searchPressed();
    void frameSearchPressed();
    void programPressed();
    void filterPressed();
    void modePressed();
    void playPressed();
    void pausePressed();
    void stepPressed(qint32 direction);
    void skipPressed(qint32 direction);
    void multiSpeedPressed(qint32 direction);        // - and +
    void directionPressed(qint32 direction);         // reverse and forward
    void jogged(qint32 steps);
    void shuttled(double deflection);
    void jogModeChanged(bool jog);
    void displayPressed();
    void digitalAnalogPressed();
    void repeatAPressed();
    void repeatBPressed();
    void memoryPressed();
    void stillWithSoundPressed();
    void chromaPressed();
    void mutePressed();
    void viewModePressed();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    QPushButton *key(const QString &text, const QString &tip, const QString &role);
    static void setLit(QPushButton *button, bool lit);
    QLabel *entryLabel;
    QLabel *stateLabel;
    QLabel *audioLabel;
    JogWheel *wheel;
    QPushButton *modeButton;
    QPushButton *jogModeButton, *displayButton, *memoryButton, *stillButton, *repeatA, *repeatB, *digitalAnalogButton;
    QPushButton *playButton, *pauseButton, *sideA, *sideB, *cxButton;
    QPushButton *chromaButton, *muteButton, *viewButton;
    QPushButton *filterButton;
    QVector<QPushButton *> needsSource;
};

#endif // REMOTEWIDGET_H
