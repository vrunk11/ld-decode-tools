/******************************************************************************
 * vlcrenderer.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "vlcrenderer.h"

#include "i420.h"

#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <thread>

#include <cstdio>
#include <cstring>

namespace {
// The times of the frames start there (a time below it, for a frame that is late on purpose, stays above zero: VLC takes a negative time for no time)
constexpr int64_t timeBase = 10000000;
// How late a frame that is given again is dated: far enough behind the clock of VLC (its 300 ms of buffering included) that its video output drops it as too late
constexpr int64_t repeatLateness = 2000000;
// How far in the past a new frame is dated: VLC shows a frame at its time plus the 300 ms it buffers (its input cannot be told less), so a frame dated that much earlier is
// shown soon after it comes, with the time its filters and its output need (80 ms) left for them
constexpr int64_t newFrameLead = 220000;
// The delay of VLC's input for imem: it answers DEFAULT_PTS_DELAY, 300 ms in VLC 3 (modules/access/imem.c), which no option changes
constexpr int64_t vlcInputDelay = 300000;
}

VlcRenderer::VlcRenderer(QObject *parent)
    : QObject(parent)
{
    warmTimer.setSingleShot(true);
    connect(&warmTimer, &QTimer::timeout, this, [this]() {
        if (!player || !warming) return;
        warming = false;
        // (a deinterlacer that was just made has seen nothing but the black: given the picture after it, it does not deinterlace it. It is given the frame before
        // the picture first, as it would have had it playing)
        if (!deinterlaceMode.isEmpty() && lastPosition > 1) {
            previousExpected = lastPosition - 1;
            emit previousWanted(previousExpected);
            previousExpected = -1;
        }
        push(lastPushed);
        // (a deinterlacer is given the next frame one period later, as it is when playing)
        if (!deinterlaceMode.isEmpty() && lastPosition >= 0) nextTimer.start(qMax(1, qRound(1000.0 / qMax(1.0, rate))));
    });
    // How late VLC shows a picture after ld-analyse made it is known, not measured on the screen: VLC shows a frame at its time plus the delay of its input (for imem,
    // DEFAULT_PTS_DELAY: a constant of VLC), and the time it is given is ld-analyse's; what is not known in advance is how long the frames waited for VLC to take them,
    // which is counted as they are taken (provide). It is told when it has changed.
    latencyTimer.setInterval(500);
    connect(&latencyTimer, &QTimer::timeout, this, [this]() {
        const int64_t waited = averageWait.load();
        if (waited < 0) return;
        const qint32 milliseconds = static_cast<qint32>((vlcInputDelay - newFrameLead + waited + 500) / 1000);
        if (qAbs(milliseconds - toldLatency) < 3) return;
        toldLatency = milliseconds;
        if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) fprintf(stderr, "VlcRenderer: the picture is shown %d ms after it is made (frames waited %lld ms)\n", milliseconds, static_cast<long long>(waited / 1000));
        emit latencyMeasured(milliseconds);
    });
    nextTimer.setSingleShot(true);
    connect(&nextTimer, &QTimer::timeout, this, [this]() {
        if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) fprintf(stderr, "VlcRenderer: next frame wanted after %d (mode \"%s\", ahead %d)\n", lastPosition, qPrintable(deinterlaceMode), aheadPosition);
        if (warming) return;
        if (player && !deinterlaceMode.isEmpty() && lastPosition >= 0 && aheadPosition < 0) emit nextWanted(lastPosition + 1);
    });
}

VlcRenderer::~VlcRenderer()
{
    stopPlayer();
    destroyInstance();
}

void VlcRenderer::setConfiguration(const QString &vlcExecutable, const QStringList &arguments)
{
    if (vlcExecutable == vlcPath && arguments == vlcArguments) return;
    vlcPath = vlcExecutable;
    vlcArguments = arguments;
    // (what runs was made with the old ones: the next picture starts it again)
    stopPlayer();
    destroyInstance();
}

void VlcRenderer::setEnabled(bool enabled)
{
    if (enabled == enabledNow) return;
    enabledNow = enabled;
    if (!enabled) {
        stopPlayer();
        destroyInstance();
    }
}

void VlcRenderer::setDeinterlace(const QString &mode)
{
    if (mode == deinterlaceMode) return;
    deinterlaceMode = mode;
    if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) fprintf(stderr, "VlcRenderer: deinterlacing \"%s\"%s\n", qPrintable(mode), player ? "" : " (for the next player)");
    if (!player) return;
    applyDeinterlace();
    // (the picture on show was made by the old filters: it is given again as a new frame, which the new filters, that have seen nothing yet, make again; and a
    // deinterlacer is given the one that follows it)
    aheadPosition = -1;
    aheadFrame.clear();
    if (warming) return;
    if (!lastPushed.isEmpty()) push(lastPushed);
    askForNext(false);
}

void VlcRenderer::setAspect(const QString &newAspect)
{
    if (newAspect == aspect) return;
    aspect = newAspect;
    if (player) api->setAspectRatio(player, aspect.isEmpty() ? nullptr : aspect.toUtf8().constData());
}

bool VlcRenderer::ensureInstance()
{
    if (instance) return true;
    QString error;
    api = LibVlc::load(vlcPath, error);
    if (!api) {
        enabledNow = false;
        emit stopped(error);
        return false;
    }
    // (what VLC says goes to the standard error: its errors by default, more with LD_ANALYSE_VLC_VERBOSE=2)
    // --drop-late-frames: the frames that are given again are dropped by the video output as too late, before its filters (a deinterlacer) see them
    // --clock-synchro=0: the times of the frames are the time of the clock of the computer, there is no drift for VLC to follow
    QStringList arguments = { QString("--verbose=%1").arg(qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE") ? qEnvironmentVariable("LD_ANALYSE_VLC_VERBOSE") : QStringLiteral("0")), "--no-one-instance", "--no-audio", "--no-video-title-show", "--no-snapshot-preview", "--no-osd", "--no-spu", "--deinterlace=0", "--drop-late-frames", "--clock-synchro=0" };
    arguments << vlcArguments;
    QList<QByteArray> encoded;
    for (const QString &argument: arguments) encoded.push_back(argument.toUtf8());
    QVector<const char *> pointers;
    for (const QByteArray &argument: encoded) pointers.push_back(argument.constData());
    instance = api->newInstance(static_cast<int>(pointers.size()), pointers.constData());
    if (!instance) {
        enabledNow = false;
        emit stopped(tr("libvlc could not be started (%1)").arg(vlcPath));
        return false;
    }
    return true;
}

void VlcRenderer::destroyInstance()
{
    if (instance && api) api->releaseInstance(instance);
    instance = nullptr;
}

void VlcRenderer::start(const QImage &image, const QByteArray &firstFrame)
{
    stopPlayer();
    if (!ensureInstance()) return;
    pictureWidth = image.width();
    pictureHeight = image.height();
    width = (pictureWidth + 1) & ~1;
    height = (pictureHeight + 1) & ~1;

    LibVlc::Media *media = api->mediaNewLocation(instance, "imem://");
    if (!media) {
        enabledNow = false;
        emit stopped(tr("libvlc could not make its input"));
        return;
    }
    const auto option = [&](const QString &text) { api->mediaAddOption(media, text.toUtf8().constData()); };
    // (the addresses of the functions, as the input of VLC takes them)
    option(QString(":imem-get=%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(&VlcRenderer::provide))));
    option(QString(":imem-release=%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(&VlcRenderer::finished))));
    option(QString(":imem-data=%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(this))));
    option(":imem-cat=2");
    option(":imem-codec=I420");
    option(QString(":imem-width=%1").arg(width));
    option(QString(":imem-height=%1").arg(height));
    option(QString(":imem-fps=%1/1000").arg(qRound(rate * 1000.0)));
    player = api->playerNewFromMedia(media);
    api->mediaRelease(media);
    if (!player) {
        enabledNow = false;
        emit stopped(tr("libvlc could not make its player"));
        return;
    }
    if (host) {
#if defined(Q_OS_WIN)
        if (api->playerSetHwnd) api->playerSetHwnd(player, reinterpret_cast<void *>(host->winId()));
#elif defined(Q_OS_MACOS)
        if (api->playerSetNsobject) api->playerSetNsobject(player, reinterpret_cast<void *>(host->winId()));
#else
        if (api->playerSetXwindow) api->playerSetXwindow(player, static_cast<uint32_t>(host->winId()));
#endif
        // (that set the video output of the player back to "any": the one that was chosen is set again, or VLC would take its own)
        QString vout;
        for (const QString &argument: vlcArguments)
            if (argument.startsWith("--vout=")) vout = argument.mid(7);
        if (!vout.isEmpty()) {
            if (api->varSetChecked) {
                const QByteArray name = vout.toUtf8();
                LibVlc::Value value;
                value.string = name.constData();
                api->varSetChecked(player, "vout", LibVlc::varString, value);
            } else if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) {
                fprintf(stderr, "VlcRenderer: libvlccore has no var_SetChecked, the video output %s cannot be set\n", qPrintable(vout));
            }
        }
    }
    applyDeinterlace();
    if (!aspect.isEmpty()) api->setAspectRatio(player, aspect.toUtf8().constData());
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = false;
        waiting.clear();
        waitingSince.clear();
        lastGiven.clear();
        waiting.push_back(firstFrame);
        waitingSince.push_back(std::chrono::steady_clock::now());
        lastGivenAt = std::chrono::steady_clock::time_point();
        clock.start();
    }
    averageWait.store(-1);
    toldLatency = -1000;
    latencyTimer.start();
    api->playerPlay(player);
}

void VlcRenderer::stopPlayer()
{
    lastPushed.clear();
    latencyTimer.stop();
    warmTimer.stop();
    warming = false;
    nextTimer.stop();
    lastPosition = aheadPosition = -1;
    aheadFrame.clear();
    if (!player) return;
    // (libvlc may be waiting for a frame that will not come: it is told so, or stopping it would wait for it)
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
    }
    wake.notify_all();
    // The player is stopped on a thread of its own, while this one goes on taking the messages of its windows: a video output that has a window of its own in the one
    // of ld-analyse sends messages to it when it closes, and waits for them to be taken. Stopped from here, that would wait for a thread that waits for it, until the
    // output gives up, is unloaded, and its window is then sent the messages that were waiting (ld-analyse crashes).
    LibVlc::Player *closingPlayer = player;
    player = nullptr;
    stopping = true;
    QEventLoop loop;
    std::thread stopper([this, closingPlayer, &loop]() {
        api->playerStop(closingPlayer);
        api->playerRelease(closingPlayer);
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    stopper.join();
    stopping = false;
    std::lock_guard<std::mutex> lock(mutex);
    waiting.clear();
    waitingSince.clear();
    held.clear();
    lastGiven.clear();
}

void VlcRenderer::push(const QByteArray &frame)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        waiting.push_back(frame);
        waitingSince.push_back(std::chrono::steady_clock::now());
        // (two at most: a frame that waits behind another is late; the older is dropped)
        while (waiting.size() > 2) {
            waiting.pop_front();
            waitingSince.pop_front();
        }
    }
    wake.notify_one();
}

void VlcRenderer::setImage(const QImage &image, double framesPerSecond, qint32 position, bool playing)
{
    if (!enabledNow || image.isNull()) return;
    // (a picture that comes while a player is being stopped: the window took its messages meanwhile; the next picture is shown)
    if (stopping) return;
    if (framesPerSecond > 0.0) rate = framesPerSecond;
    // The player is made for a size of picture: for another one (a view that is not the same size) it is made again
    if (player && (image.width() != pictureWidth || image.height() != pictureHeight)) {
        if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) fprintf(stderr, "VlcRenderer: the picture is %dx%d, the player was made for %dx%d\n", image.width(), image.height(), pictureWidth, pictureHeight);
        stopPlayer();
    }
    if (!player) {
        // (the frames are made for the size that the player is made for: even)
        const qint32 evenWidth = (image.width() + 1) & ~1;
        const qint32 evenHeight = (image.height() + 1) & ~1;
        // VLC shows the first frame it is given out of its course (as soon as it has it, before its clock runs), and its video output does not draw that one again by
        // itself: the stream starts with a black frame, and the picture comes next as any other. (A deinterlacer sees black, then the picture: a cut, not the same frame twice.)
        QByteArray black(static_cast<qsizetype>(evenWidth) * evenHeight * 3 / 2, char(128));
        memset(black.data(), 16, static_cast<size_t>(evenWidth) * evenHeight);
        start(image, black);
        if (!player) return;
        // The filters and the output of VLC are made when its first frame comes, and some take their time then (a deinterlacer or an output on the GPU): what comes
        // meanwhile is late for them, and they drop it. So the picture is given when they have had the black for a moment (the latest one that came by then).
        lastPushed = toI420(image, evenWidth, evenHeight);
        lastPosition = position;
        warming = true;
        warmTimer.start(400);
        return;
    }
    QByteArray frame = toI420(image, width, height);
    if (warming) {
        lastPushed = frame;
        lastPosition = position;
        return;
    }
    if (deinterlaceMode.isEmpty()) {
        // The same picture again (the window was drawn again, nothing moved) is not a new frame: a deinterlacer that is given the same frame twice takes the scene for still
        // and weaves it
        aheadPosition = -1;
        aheadFrame.clear();
        lastPosition = position;
        if (frame == lastPushed) return;
        lastPushed = frame;
        push(frame);
        return;
    }
    if (position >= 0 && position == lastPosition && frame != lastPushed) {
        // The same frame made again (another decoder, other settings): given after the one it replaces, a deinterlacer would see the same frame twice, take the scene for
        // still and weave it. Its filter is made again (off, then on: it has seen nothing), and is given the frame before, the frame, then the one that follows.
        applyDeinterlaceMode(QString());
        applyDeinterlace();
        // (and, as a filter that was just made, it is given the frame before first)
        lastPushed = frame;
        lastPosition = position;
        if (position > 1) {
            previousExpected = position - 1;
            emit previousWanted(previousExpected);
            previousExpected = -1;
        }
        push(frame);
    } else if (position >= 0 && position == aheadPosition && frame == aheadFrame) {
        // (it was given as the one that follows: the deinterlacer has it already, and gives it out with the next)
        lastPushed = frame;
    } else {
        if (frame == lastPushed && position == lastPosition) return;
        lastPushed = frame;
        push(frame);
    }
    lastPosition = position;
    aheadPosition = -1;
    aheadFrame.clear();
    askForNext(playing);
}

// A deinterlacer gives out a frame when it has the one that follows (it looks at both): the picture on show is only drawn when the next frame comes. Playing, it comes by
// itself; a picture that stays (a step, a seek, a pause) has it asked for, and it is given to VLC (setNextImage) without being shown: what is shown is the picture.
void VlcRenderer::askForNext(bool playing)
{
    if (deinterlaceMode.isEmpty() || lastPosition < 0) return;
    nextTimer.start(playing ? 150 : 0);
}

void VlcRenderer::setPreviousImage(const QImage &image, qint32 position)
{
    if (!player || image.isNull() || position < 0 || position != previousExpected) return;
    if (image.width() != pictureWidth || image.height() != pictureHeight) return;
    push(toI420(image, width, height));
}

void VlcRenderer::setNextImage(const QImage &image, qint32 position)
{
    if (qEnvironmentVariableIsSet("LD_ANALYSE_VLC_VERBOSE")) fprintf(stderr, "VlcRenderer: next frame %d given (%dx%d), on show %d, ahead %d, player %s\n", position, image.width(), image.height(), lastPosition, aheadPosition, player ? "yes" : "no");
    if (warming) return;
    if (!player || stopping || deinterlaceMode.isEmpty() || image.isNull() || position != lastPosition + 1 || aheadPosition >= 0) return;
    if (image.width() != pictureWidth || image.height() != pictureHeight) return;
    aheadFrame = toI420(image, width, height);
    aheadPosition = position;
    push(aheadFrame);
}

// The deinterlacing is set on the player and on its video outputs, as libvlc_video_set_deinterlace does; but that takes the modes of VLC's own deinterlacer only (not "auto",
// on which a deinterlacer that was added to VLC, as wkdif, takes the place of VLC's), so it is done here, when libvlccore has what it takes
void VlcRenderer::applyDeinterlace()
{
    applyDeinterlaceMode(deinterlaceMode);
}

void VlcRenderer::applyDeinterlaceMode(const QString &wanted)
{
    if (!player) return;
    if (!api->varSetChecked || !api->varType || !api->listChildren || !api->listRelease) {
        api->setDeinterlace(player, wanted.isEmpty() ? nullptr : wanted.toUtf8().constData());
        return;
    }
    const QByteArray mode = wanted.toUtf8();
    const auto set = [&](void *object) {
        if (!mode.isEmpty()) {
            LibVlc::Value value;
            value.string = mode.constData();
            api->varSetChecked(object, "deinterlace-mode", LibVlc::varString, value);
        }
        LibVlc::Value on;
        on.integer = mode.isEmpty() ? 0 : 1;
        api->varSetChecked(object, "deinterlace", LibVlc::varInteger, on);
    };
    set(player);
    if (LibVlc::List *children = api->listChildren(player)) {
        for (int i = 0; i < children->count; ++i) {
            void *child = children->values[i].address;
            if (child && api->varType(child, "deinterlace-mode")) set(child);
        }
        api->listRelease(children);
    }
}

int VlcRenderer::provide(void *data, const char *, int64_t *dts, int64_t *pts, unsigned *flags, size_t *size, void **buffer)
{
    auto *self = static_cast<VlcRenderer *>(data);
    std::unique_lock<std::mutex> lock(self->mutex);
    // The input of VLC follows the clock of the computer (it is a live source): it is given something each period, or it stops to buffer again, with 300 ms of delay
    // each time, and drops what comes after. So when there is no new picture, the last one is given again, but dated in the past: the clock of VLC goes on, and its video
    // output drops that frame as too late before its filters see it. What it shows (the last picture that its filters made) is drawn again by itself, as when VLC is paused.
    const auto period = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / qMax(1.0, self->rate)));
    // (one frame each period, as a video is: frames that come together are given one period apart, so that what follows them -- a deinterlacer that doubles the rate, an
    // output that times its pictures by theirs -- sees the times of a video, and not frames a few milliseconds apart that it takes for late)
    const auto due = self->lastGivenAt + period;
    self->wake.wait_until(lock, due, [self] { return self->closing; });
    if (self->closing) return -1;
    self->wake.wait_for(lock, period, [self] { return self->closing || !self->waiting.empty(); });
    if (self->closing) return -1;
    // (the next one is due one period after this one was due, so that the frames are taken at the rate they come and none waits behind another; after a wait, a frame
    // that comes is given at once)
    const auto given = std::chrono::steady_clock::now();
    self->lastGivenAt = (given - due < period) ? due : given;
    const int64_t now = timeBase + self->clock.nsecsElapsed() / 1000;
    std::shared_ptr<QByteArray> frame;
    bool repeat = false;
    if (!self->waiting.empty()) {
        frame = std::make_shared<QByteArray>(std::move(self->waiting.front()));
        self->waiting.pop_front();
        self->lastGiven = *frame;
        // (how long it waited to be taken: the mean of the last ones, for the delay of the picture)
        const int64_t waited = std::chrono::duration_cast<std::chrono::microseconds>(given - self->waitingSince.front()).count();
        self->waitingSince.pop_front();
        const int64_t mean = self->averageWait.load();
        self->averageWait.store(mean < 0 ? waited : (mean * 7 + waited) / 8);
    } else {
        frame = std::make_shared<QByteArray>(self->lastGiven);
        repeat = true;
    }
    self->held.insert(frame->constData(), frame);
    *buffer = const_cast<char *>(frame->constData());
    *size = static_cast<size_t>(frame->size());
    // (the time of decoding is the clock of VLC for its input; the time of showing is the one of the picture: now for a new one, in the past for a frame given again)
    *dts = now;
    *pts = repeat ? now - repeatLateness : now - newFrameLead;
    *flags = 0;
    return 0;
}

void VlcRenderer::finished(void *data, const char *, size_t, void *buffer)
{
    auto *self = static_cast<VlcRenderer *>(data);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->held.remove(buffer);
}
