/******************************************************************************
 * playbackrenderer.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "playbackrenderer.h"

#include <algorithm>

class PlaybackRenderer::Worker : public QThread
{
public:
    Worker(PlaybackRenderer &_owner, TbcSource &_source) : owner(_owner), source(_source) {}

protected:
    void run() override
    {
        quint32 appliedGeneration = 0;
        bool opened = source.getIsSourceLoaded();
        bool applied = false;

        while (true) {
            TbcSource::RenderSettings settings;
            quint32 generation;
            qint32 position;
            bool isFieldView;
            {
                QMutexLocker locker(&owner.mutex);
                while (true) {
                    if (owner.stopping) return;
                    const qint32 candidate = std::max(owner.nextToRender, owner.dueHint);
                    if (candidate <= owner.lastPosition && candidate <= owner.dueHint + owner.lookAhead) {
                        position = candidate;
                        break;
                    }
                    owner.wake.wait(&owner.mutex);
                }
                owner.nextToRender = position + 1;
                settings = owner.currentSettings;
                generation = owner.currentGeneration;
            }

            // The metadata is read here, in the worker, the first time it is needed
            if (!opened) {
                opened = source.openForRendering(settings);
                if (!opened) {
                    QMutexLocker locker(&owner.mutex);
                    owner.failed = true;
                    owner.stopping = true;
                    owner.wake.wakeAll();
                    emit owner.imageReady();
                    return;
                }
                applied = true;
                appliedGeneration = generation;
            }
            if (!applied || appliedGeneration != generation) {
                source.applyRenderSettings(settings);
                applied = true;
                appliedGeneration = generation;
            }

            isFieldView = settings.viewMode == TbcSource::ViewMode::FIELD_VIEW;
            source.load(isFieldView ? (position + 1) / 2 : position, isFieldView ? position : position * 2 - 1);
            const QImage image = source.getImage();

            {
                QMutexLocker locker(&owner.mutex);
                owner.ready.insert(position, Rendered{image, generation});
            }
            emit owner.imageReady();
        }
    }

private:
    PlaybackRenderer &owner;
    TbcSource &source;
};

PlaybackRenderer::PlaybackRenderer(QObject *parent)
    : QObject(parent)
{
}

PlaybackRenderer::~PlaybackRenderer()
{
    stop();
}

void PlaybackRenderer::start(const TbcSource::RenderSettings &settings, quint32 generation,
                             qint32 firstPosition, qint32 _lastPosition)
{
    stop();

    // New files need new sources; the same ones can go on as they are
    if (openedFor != settings.sourceFilename + "|" + settings.metadataFilename) {
        sources.clear();
        openedFor = settings.sourceFilename + "|" + settings.metadataFilename;
    }

    // Leave a core for the window, and a few for the rest of the machine
    const qint32 count = std::clamp(QThread::idealThreadCount() - 2, 1, 4);
    while (static_cast<qint32>(sources.size()) < count) sources.push_back(std::make_unique<TbcSource>());

    {
        QMutexLocker locker(&mutex);
        currentSettings = settings;
        currentGeneration = generation;
        lastPosition = _lastPosition;
        nextToRender = firstPosition;
        dueHint = firstPosition;
        lastTaken = firstPosition - 1;
        stopping = false;
        failed = false;
        lookAhead = count * 2;
        ready.clear();
    }

    for (qint32 i = 0; i < count; i++) {
        workers.push_back(std::make_unique<Worker>(*this, *sources[i]));
        workers.back()->start();
    }
}

void PlaybackRenderer::setSettings(const TbcSource::RenderSettings &settings, quint32 generation)
{
    QMutexLocker locker(&mutex);
    currentSettings = settings;
    currentGeneration = generation;

    // What was rendered with the old settings is of no use; render from the present again
    ready.clear();
    nextToRender = dueHint;
    wake.wakeAll();
}

void PlaybackRenderer::stop()
{
    {
        QMutexLocker locker(&mutex);
        stopping = true;
        wake.wakeAll();
    }
    for (auto &worker: workers) worker->wait();
    workers.clear();
    QMutexLocker locker(&mutex);
    ready.clear();
}

void PlaybackRenderer::close()
{
    stop();
    sources.clear();
    openedFor.clear();
}

void PlaybackRenderer::setDue(qint32 position)
{
    QMutexLocker locker(&mutex);
    if (position > dueHint) {
        dueHint = position;
        wake.wakeAll();
    }
}

bool PlaybackRenderer::take(qint32 due, qint32 &position, QImage &image)
{
    QMutexLocker locker(&mutex);

    // The newest image ready that is not later than the one due
    auto it = ready.upperBound(due);
    if (it == ready.begin()) return false;
    --it;
    if (it.key() <= lastTaken || it.value().generation != currentGeneration) return false;

    position = it.key();
    image = it.value().image;
    lastTaken = position;

    // Anything older is of no use now
    ready.erase(ready.begin(), std::next(it));
    wake.wakeAll();
    return true;
}
