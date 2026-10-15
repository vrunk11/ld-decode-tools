/******************************************************************************
 * playbackcontroller.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

// The clock of the video playback. It knows nothing of where frames come from: it
// says which position (frame, or field in field view) is due at this moment, and the
// owner shows it. The clock is the wall clock, not a count of frames shown, so if
// showing a frame takes longer than the frame lasts, the following positions are
// skipped and the video stays at the right speed instead of running slow.
class PlaybackController : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackController(QObject *parent = nullptr);

    // Positions per second at normal speed (frames per second, or twice that for fields)
    void setRate(double positionsPerSecond);
    void setSpeed(double speed);
    void setLoop(bool loop);
    // Take the time from somewhere else than the wall clock: the seconds of sound played since play() (the video then keeps
    // to the sound). Null goes back to the wall clock. At normal speed only.
    void setExternalClock(std::function<double()> seconds);
    void setRange(qint32 first, qint32 last);
    // A live stream: the range grows as video arrives, and playback waits at its end for more
    // instead of stopping, until the stream has ended
    void setLive(bool live, bool streamEnded = false);

    double getSpeed() const { return speed; }
    bool getLoop() const { return loop; }
    bool isPlaying() const { return timer.isActive(); }
    qint32 getSkippedPositions() const { return skipped; }
    void clearSkipped() { skipped = 0; }

    void play(qint32 fromPosition);
    void pause();

signals:
    // The position now due. Not emitted for a position that is already showing.
    void positionDue(qint32 position);
    // Playback reached the end of the range (and stopped, unless looping)
    void reachedEnd();
    void stateChanged(bool playing);
    // Playback went from the end to the start again (looping)
    void looped();

private slots:
    void tick();

private:
    QTimer timer;
    QElapsedTimer clock;
    double rate = 25.0;
    double speed = 1.0;
    bool loop = false;
    qint32 firstPosition = 1, lastPosition = 1;
    qint32 startPosition = 1;
    qint32 shownPosition = 1;
    qint32 skipped = 0;
    bool live = false;
    bool streamEnded = false;
    bool atEdge = false;     // live and caught up with the video that has arrived
    qint64 baseNs = 0;       // clock time at which startPosition was due
    std::function<double()> externalClock;
    double externalBase = 0.0;
    void rebase(qint32 fromPosition);
};

#endif // PLAYBACKCONTROLLER_H
