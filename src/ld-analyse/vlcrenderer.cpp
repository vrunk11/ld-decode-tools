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

VlcRenderer::VlcRenderer(QObject *parent)
    : QObject(parent)
{
    repeatTimer.setTimerType(Qt::PreciseTimer);
    connect(&repeatTimer, &QTimer::timeout, this, &VlcRenderer::giveStillAgain);
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
    if (player) api->setDeinterlace(player, mode.isEmpty() ? nullptr : mode.toUtf8().constData());
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
    QStringList arguments = { "--quiet", "--no-one-instance", "--no-audio", "--no-video-title-show", "--no-snapshot-preview", "--no-osd", "--no-spu", "--deinterlace=0" };
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

void VlcRenderer::start(const QImage &image)
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
    option(":imem-caching=0");
    option(":live-caching=0");
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
    }
    if (!deinterlaceMode.isEmpty()) api->setDeinterlace(player, deinterlaceMode.toUtf8().constData());
    if (!aspect.isEmpty()) api->setAspectRatio(player, aspect.toUtf8().constData());
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = false;
        waiting.clear();
    }
    clock.start();
    api->playerPlay(player);
}

void VlcRenderer::stopPlayer()
{
    repeatTimer.stop();
    still.clear();
    if (!player) return;
    // (libvlc may be waiting for a frame that will not come: it is told so, or stopping it would wait for it)
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
    }
    wake.notify_all();
    api->playerStop(player);
    api->playerRelease(player);
    player = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    waiting.clear();
    held.clear();
}

void VlcRenderer::push(const QByteArray &frame)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        waiting.push_back(frame);
        while (waiting.size() > 3) waiting.pop_front();
    }
    wake.notify_one();
}

void VlcRenderer::setImage(const QImage &image, double framesPerSecond)
{
    if (!enabledNow || image.isNull()) return;
    if (framesPerSecond > 0.0) rate = framesPerSecond;
    // The player is made for a size of picture: for another one (a view that is not the same size) it is made again
    if (player && (image.width() != pictureWidth || image.height() != pictureHeight)) stopPlayer();
    if (!player) start(image);
    if (!player) return;

    const QByteArray frame = toI420(image, width, height);
    // A picture that comes by itself (a seek, a step) is given a few times, those that come one after the other (a video that plays) once each
    const bool alone = !sinceLast.isValid() || sinceLast.elapsed() > 120;
    sinceLast.restart();
    push(frame);
    if (alone) {
        still = frame;
        repeatsLeft = 2;
        repeatTimer.start(qMax(1, qRound(1000.0 / rate)));
    } else {
        repeatTimer.stop();
    }
}

void VlcRenderer::giveStillAgain()
{
    if (!player || still.isEmpty() || repeatsLeft <= 0) {
        repeatTimer.stop();
        return;
    }
    push(still);
    if (--repeatsLeft <= 0) repeatTimer.stop();
}

int VlcRenderer::provide(void *data, const char *, int64_t *dts, int64_t *pts, unsigned *flags, size_t *size, void **buffer)
{
    auto *self = static_cast<VlcRenderer *>(data);
    std::unique_lock<std::mutex> lock(self->mutex);
    self->wake.wait(lock, [self] { return self->closing || !self->waiting.empty(); });
    if (self->closing) return -1;
    auto frame = std::make_shared<QByteArray>(std::move(self->waiting.front()));
    self->waiting.pop_front();
    self->held.insert(frame->constData(), frame);
    *buffer = const_cast<char *>(frame->constData());
    *size = static_cast<size_t>(frame->size());
    // (the time that it is given at: VLC shows it then)
    *dts = *pts = static_cast<int64_t>(self->clock.nsecsElapsed() / 1000);
    *flags = 0;
    return 0;
}

void VlcRenderer::finished(void *data, const char *, size_t, void *buffer)
{
    auto *self = static_cast<VlcRenderer *>(data);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->held.remove(buffer);
}
