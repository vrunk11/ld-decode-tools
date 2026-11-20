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

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "libvlcapi.h"

// VLC shows the picture, in a widget of the window of ld-analyse, through its libvlc, which is loaded from the VLC that is installed (so the modules that were added to it are
// there: an upscaler as a video output, a deinterlacer). The picture on show is given to it as I420 frames by its `imem` input, with the time that each is given at: VLC calls
// back for the next frame when it wants one, and waits for it. Nothing is sent when the picture does not change (VLC keeps the last one it drew); a picture that comes by
// itself (a seek, a step) is given a few times, because a deinterlacer needs the frames round the one it shows.
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

    // The picture that is on show, and the rate of the video
    void setImage(const QImage &image, double framesPerSecond);

signals:
    // libvlc could not be loaded or started: the renderer is off then
    void stopped(const QString &message);

private:
    bool ensureInstance();
    void start(const QImage &image);
    void stopPlayer();
    void destroyInstance();
    void push(const QByteArray &frame);
    void giveStillAgain();

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
    QMap<const void *, std::shared_ptr<QByteArray>> held;
    bool closing = false;
    QElapsedTimer clock;               // the time that the frames are given at: since the player was started

    QElapsedTimer sinceLast;           // since the last picture came
    QByteArray still;                  // the picture that came by itself, and is given again (a deinterlacer needs the frames round it)
    qint32 repeatsLeft = 0;
    QTimer repeatTimer;
};

#endif // VLCRENDERER_H
