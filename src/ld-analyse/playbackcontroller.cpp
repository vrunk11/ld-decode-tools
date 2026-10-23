/******************************************************************************
 * playbackcontroller.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "playbackcontroller.h"

#include <cmath>

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
    // Wake often enough to catch a position soon after it falls due; the position is
    // worked out from the clock, so the wake-ups need not be exact
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(4);
    connect(&timer, &QTimer::timeout, this, &PlaybackController::tick);
}

void PlaybackController::setRate(double positionsPerSecond)
{
    if (positionsPerSecond <= 0.0 || positionsPerSecond == rate) return;
    if (isPlaying()) rebase(shownPosition);
    rate = positionsPerSecond;
}

void PlaybackController::setSpeed(double newSpeed)
{
    if (newSpeed <= 0.0 || newSpeed == speed) return;
    // Keep the position the video is at, then go on at the new speed from there
    if (isPlaying()) rebase(shownPosition);
    speed = newSpeed;
}

void PlaybackController::setExternalClock(std::function<double()> seconds)
{
    externalClock = std::move(seconds);
    // Go on from the position that is shown, by the new clock
    if (isPlaying()) rebase(shownPosition);
}

void PlaybackController::setLoop(bool newLoop)
{
    loop = newLoop;
}

void PlaybackController::setRange(qint32 first, qint32 last)
{
    firstPosition = first;
    const qint32 previousLast = lastPosition;
    lastPosition = qMax(first, last);

    // Caught up with a live stream, and the next frame has just arrived: show it at once,
    // and go on from there at the pace of the video
    if (live && atEdge && isPlaying() && lastPosition > previousLast && lastPosition > shownPosition) {
        atEdge = false;
        rebase(shownPosition + 1);
        emit positionDue(shownPosition);
    }
}

void PlaybackController::setLive(bool newLive, bool ended)
{
    live = newLive;
    streamEnded = ended;
}

void PlaybackController::rebase(qint32 fromPosition)
{
    startPosition = fromPosition;
    shownPosition = fromPosition;
    clock.start();
    baseNs = 0;
    externalBase = externalClock ? externalClock() : 0.0;
}

void PlaybackController::play(qint32 fromPosition)
{
    if (isPlaying()) return;
    atEdge = false;

    // At the end already: play again from the start
    if (fromPosition >= lastPosition && !(live && !streamEnded)) fromPosition = firstPosition;

    skipped = 0;
    rebase(qBound(firstPosition, fromPosition, lastPosition));
    timer.start();
    emit stateChanged(true);
}

void PlaybackController::pause()
{
    if (!isPlaying()) return;
    timer.stop();
    emit stateChanged(false);
}

void PlaybackController::tick()
{
    const double elapsedSeconds = externalClock ? externalClock() - externalBase : static_cast<double>(clock.nsecsElapsed() - baseNs) / 1e9;
    qint64 due = static_cast<qint64>(startPosition) + static_cast<qint64>(std::floor(elapsedSeconds * rate * speed));

    if (due > lastPosition) {
        if (live && !streamEnded) {
            // Caught up with the video that has arrived: show the last of it if it is not
            // already shown, then wait for more
            if (shownPosition < lastPosition) {
                skipped += lastPosition - shownPosition - 1;
                shownPosition = lastPosition;
                emit positionDue(shownPosition);
            }
            atEdge = true;
            return;
        }
        if (loop) {
            // Wrap round, keeping the time that went past the end
            rebase(firstPosition);
            due = firstPosition;
            emit looped();
        } else {
            timer.stop();
            if (shownPosition != lastPosition) {
                shownPosition = lastPosition;
                emit positionDue(lastPosition);
            }
            emit stateChanged(false);
            return;
        }
    }

    if (due <= shownPosition) return;

    // Positions between the last one shown and this one were not shown: showing the last
    // one took longer than they lasted
    if (due > shownPosition + 1) skipped += static_cast<qint32>(due - shownPosition - 1);

    shownPosition = static_cast<qint32>(due);
    emit positionDue(shownPosition);
}
