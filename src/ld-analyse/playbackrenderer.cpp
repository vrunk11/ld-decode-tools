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
#include <cstdio>
#include <cstdlib>

#include <QElapsedTimer>

class PlaybackRenderer::Worker : public QThread
{
public:
    // The frames that a worker decodes together for the HVD 3D decoder (one window for the run)
    static constexpr qint32 kRunLength = 6;

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
            qint32 count = 1;
            quint32 claimedEpoch;
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
                settings = owner.currentSettings;
                generation = owner.currentGeneration;
                claimedEpoch = owner.epoch;
                // The HVD 3D decoder decodes a window of fields: a run of frames in one window costs a lot less than the same frames each in its own
                // (that decodes the frames around each of them again), and the playback must beat the clock
                const bool run = settings.hvdSettings.enabled && settings.hvdSettings.temporal && settings.chromaOn && !settings.isLive
                                 && settings.viewMode == TbcSource::ViewMode::FRAME_VIEW && settings.sourceMode == TbcSource::ONE_SOURCE;
                // (the positions of a weave are fields, and the frames of the run are decoded once for the weaves that they are in: twice the positions)
                if (run) count = std::max<qint32>(1, std::min(settings.weave ? 2 * kRunLength : kRunLength, owner.lastPosition - position + 1));
                owner.nextToRender = position + count;
            }

            // The metadata is read here, in the worker, the first time it is needed
            if (!opened) {
                opened = source.openForRendering(settings, owner.liveParent);
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
            QElapsedTimer runTimer;
            runTimer.start();
            if (count > 1) {
                if (settings.weave) {
                    // the frames that hold the fields of the weaves of the run
                    const qint32 firstFrame = (position + 1) / 2;
                    const qint32 lastFrame = std::min((position + count) / 2 + 1, source.getNumberOfFrames());
                    source.predecodeRun(firstFrame, lastFrame - firstFrame + 1);
                } else {
                    source.predecodeRun(position, count);
                }
            }
            const qint64 predecodeMs = runTimer.elapsed();
            for (qint32 k = 0; k < count; k++) {
                const qint32 at = position + k;
                if (settings.weave) {
                    // (the position is a field: the image is the weave of it and the next)
                    source.loadWeave(at);
                } else {
                    source.load(isFieldView ? (at + 1) / 2 : at, isFieldView ? at : at * 2 - 1);
                }
                const QImage image = source.getImage();

                {
                    QMutexLocker locker(&owner.mutex);
                    // (the settings changed while this run was being decoded: what is left of it is of no use)
                    if (owner.stopping) return;
                    // (the playback looped while this run was being decoded)
                    if (owner.epoch != claimedEpoch) break;
                    owner.ready.insert(at, Rendered{image, generation});
                }
                emit owner.imageReady();
            }
            if (std::getenv("LD_ANALYSE_TIMING")) {
                std::fprintf(stderr, "worker: positions %d..%d, %lld ms (the decoding of the run %lld ms)\n", position, position + count - 1,
                             static_cast<long long>(runTimer.elapsed()), static_cast<long long>(predecodeMs));
            }
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
                             qint32 firstPosition, qint32 _lastPosition, const TbcSource *_liveParent)
{
    stop();

    // New files need new sources; the same ones can go on as they are
    // (a stream is a new one each time it is loaded)
    const QString key = settings.sourceFilename + "|" + settings.metadataFilename + "|" + QString::number(reinterpret_cast<quintptr>(_liveParent));
    if (openedFor != key) {
        sources.clear();
        openedFor = key;
    }
    liveParent = _liveParent;

    // Leave a core for the window, and a few for the rest of the machine
    qint32 count = settings.weave ? std::clamp(QThread::idealThreadCount() - 1, 2, 8) : std::clamp(QThread::idealThreadCount() - 2, 1, 4);
    // The HVD decoder takes a core for each frame for a good while (it is the decoder that most needs the frames to be decoded ahead, and the one that scales with the cores)
    if (settings.hvdSettings.enabled) count = std::max(count, std::clamp(QThread::idealThreadCount() - 2, 2, 12));
    while (static_cast<qint32>(sources.size()) < count) sources.push_back(std::make_unique<TbcSource>());
    // The sources decode at the same time: the HVD decoder of each takes its share of the cores, not all of them (a team of threads in each would be many)
    const qint32 shareOfCores = std::max(1, QThread::idealThreadCount() / (count + 1));
    for (const auto &source : sources) source->setHvdThreads(shareOfCores);

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

void PlaybackRenderer::rewind(qint32 firstPosition)
{
    QMutexLocker locker(&mutex);
    epoch++;
    ready.clear();
    nextToRender = firstPosition;
    dueHint = firstPosition;
    lastTaken = firstPosition - 1;
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

void PlaybackRenderer::setLastPosition(qint32 position)
{
    QMutexLocker locker(&mutex);
    if (position <= lastPosition) return;
    lastPosition = position;
    wake.wakeAll();
}
