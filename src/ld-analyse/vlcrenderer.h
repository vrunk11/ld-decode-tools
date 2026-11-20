/******************************************************************************
 * vlcrenderer.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VLCRENDERER_H
#define VLCRENDERER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "libvlcapi.h"

// VLC shows the picture, in a widget of the window of ld-analyse, through its libvlc, which is loaded from the VLC that is installed (so the modules that were added to it are
// there: an upscaler as a video output, a deinterlacer). The picture on show is given to it as I420 frames by its `imem` input, with the time that each is given at: VLC calls
// back for the next frame when it wants one, and waits for it a period at most. Each picture is given once (a deinterlacer that saw the same frame twice would weave it);
// between two, VLC is given the last again, dated in the past, which its video output drops before its filters: it keeps drawing the last picture they made, as in pause.
//
// What can change while it runs is changed in place: the deinterlacing (a mode, or off) and the aspect ratio of the picture. What is given to libvlc when it is started (the video
// output, the filters of the picture and their options) is made again by starting it again; and so is a picture of another size.
class VlcRenderer : public QObject
{
    Q_OBJECT
public:
    explicit VlcRenderer(QObject *parent = nullptr);
    ~VlcRenderer() override;

    // The widget of the window that VLC draws in
    void setHost(QWidget *widget) { host = widget; }
    // The VLC to use (its executable: libvlc is next to it), and the arguments of libvlc (--vout=..., --video-filter=...). A change closes what runs; the next picture starts it.
    void setConfiguration(const QString &vlcExecutable, const QStringList &arguments);

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabledNow; }
    bool isRunning() const { return player != nullptr; }

    // The deinterlacing of the picture: a mode of VLC (yadif, blend...), "auto" for its own, or empty for none
    void setDeinterlace(const QString &mode);
    // The shape the picture is shown in, as "939:625"
    void setAspect(const QString &aspect);

    // The picture that is on show, the rate of the video, the number of the frame it is (-1 if it is none), and whether it is playing
    void setImage(const QImage &image, double framesPerSecond, qint32 position = -1, bool playing = false);
    // The frame that follows the one on show (asked for by nextWanted), for the deinterlacer: given to VLC, not shown
    void setNextImage(const QImage &image, qint32 position);
    // The frame before the one on show (asked for by previousWanted, when a deinterlacer was just made): given to VLC before it
    void setPreviousImage(const QImage &image, qint32 position);

signals:
    // A deinterlacer needs the frame that follows the picture on show to give the picture out: the frame of that number is wanted (setNextImage)
    void nextWanted(qint32 position);
    // A deinterlacer that was just made is to see the frame before the picture first: the frame of that number is wanted, now (setPreviousImage)
    void previousWanted(qint32 position);
    // How late VLC shows the picture after it is made: the delay of its input, less what the frames are dated ahead, and the time they waited for it. The sound is to be
    // that much later
    void latencyMeasured(qint32 milliseconds);
    // libvlc could not be loaded or started: the renderer is off then
    void stopped(const QString &message);

private:
    bool ensureInstance();
    void start(const QImage &image, const QByteArray &firstFrame);
    void stopPlayer();
    void destroyInstance();
    void push(const QByteArray &frame);
    void applyDeinterlace();
    void applyDeinterlaceMode(const QString &wanted);
    void askForNext(bool playing);

    // The calls of libvlc for the next frame (from its thread) and for the end of one
    static int provide(void *data, const char *cookie, int64_t *dts, int64_t *pts, unsigned *flags, size_t *size, void **buffer);
    static void finished(void *data, const char *cookie, size_t size, void *buffer);

    QWidget *host = nullptr;
    QString vlcPath;
    QStringList vlcArguments;
    bool enabledNow = false;
    const LibVlc *api = nullptr;
    LibVlc::Instance *instance = nullptr;
    LibVlc::Player *player = nullptr;
    QString deinterlaceMode, aspect;

    qint32 pictureWidth = 0, pictureHeight = 0;    // the size of the picture that the player was made for
    qint32 width = 0, height = 0;                  // the size of the frames: those, made even
    double rate = 25.0;

    // The frames that are waiting for libvlc (a few at most: what it has not asked for is dropped), and the ones it holds
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<QByteArray> waiting;
    std::deque<std::chrono::steady_clock::time_point> waitingSince;   // (when each of them came)
    QMap<const void *, std::shared_ptr<QByteArray>> held;
    bool closing = false;
    bool stopping = false;             // (a player is being stopped: see stopPlayer)
    std::chrono::steady_clock::time_point lastGivenAt;   // when libvlc was last given a frame (they are given one period apart)
    bool warming = false;              // (a player that was just started is given black for a moment: see setImage)
    QTimer warmTimer;
    QTimer latencyTimer;               // (how late the picture is shown is told when it changes)
    std::atomic<int64_t> averageWait{ -1 };   // how long the frames waited for libvlc to take them, in microseconds (the mean of the last ones; -1: none yet)
    qint32 toldLatency = -1000;
    QElapsedTimer clock;               // the times of the frames: the clock of the computer, since the player started
    QByteArray lastGiven;              // the last frame that VLC was given (by its thread)
    QByteArray lastPushed;             // the last picture that was sent to it (the same one again is not sent)
    qint32 lastPosition = -1;          // the number of the frame of that picture
    qint32 aheadPosition = -1;         // the frame that follows it, when it was given to the deinterlacer ahead of time (-1: none)
    qint32 previousExpected = -1;      // (the frame before the picture, while it is asked for)
    QByteArray aheadFrame;
    QTimer nextTimer;                  // (the picture stayed: the next frame is asked for)
};

#endif // VLCRENDERER_H
