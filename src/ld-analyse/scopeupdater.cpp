/******************************************************************************
 * scopeupdater.cpp
 * ld-analyse - the data of the scopes, made away from the window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "scopeupdater.h"

#include <QMutexLocker>

class ScopeUpdater::Worker : public QThread
{
public:
    explicit Worker(ScopeUpdater &owner) : m_owner(owner) {}

protected:
    void run() override
    {
        std::unique_ptr<TbcSource> source;
        QString openedKey;
        quint32 appliedGeneration = 0;

        while (true) {
            Request request;
            {
                QMutexLocker locker(&m_owner.m_mutex);
                while (!m_owner.m_stopping && !m_owner.m_hasRequest && !m_owner.m_reset) m_owner.m_wake.wait(&m_owner.m_mutex);
                if (m_owner.m_stopping) return;
                if (m_owner.m_reset) {
                    m_owner.m_reset = false;
                    source.reset();
                    openedKey.clear();
                }
                if (!m_owner.m_hasRequest) continue;
                request = m_owner.m_request;
                m_owner.m_hasRequest = false;
            }

            Result result;
            const TbcSource::RenderSettings &settings = request.settings;
            const QString key = settings.sourceFilename + QLatin1Char('\n') + settings.chromaFilename + QLatin1Char('\n') + settings.metadataFilename;
            if (!source || openedKey != key) {
                // (the metadata is read here, the first time it is needed, as the playback does)
                source = std::make_unique<TbcSource>();
                if (!source->openForRendering(settings)) {
                    source.reset();
                    openedKey.clear();
                    result.failed = true;
                } else {
                    openedKey = key;
                    appliedGeneration = request.generation;
                }
            } else if (appliedGeneration != request.generation) {
                source->applyRenderSettings(settings);
                appliedGeneration = request.generation;
            }

            if (source) {
                source->load(request.frame, request.field);
                result.frameWidth = source->getFrameWidth();
                result.frameHeight = source->getFrameHeight();
                result.bothSources = source->getSourceMode() == TbcSource::SourceMode::BOTH_SOURCES;
                result.line = request.line;
                result.dot = request.dot;
                if (request.wantLine) {
                    result.scanLine = source->getScanLineData(request.line);
                    result.hasLine = true;
                }
                if (request.wantVector) {
                    result.vectorImage = VectorscopeDialog::renderTrace(source->getComponentFrame(), source->getVideoParameters(), request.vectorOptions);
                    result.hasVector = true;
                }
            }

            {
                QMutexLocker locker(&m_owner.m_mutex);
                // (what was asked for meanwhile is more recent than this: it is not shown over it)
                if (!m_owner.m_hasRequest) {
                    m_owner.m_result = result;
                    m_owner.m_hasResult = true;
                }
            }
            emit m_owner.ready();
        }
    }

private:
    ScopeUpdater &m_owner;
};

ScopeUpdater::ScopeUpdater(QObject *parent) : QObject(parent)
{
    m_worker = new Worker(*this);
    m_worker->start(QThread::LowPriority);
}

ScopeUpdater::~ScopeUpdater()
{
    stop();
}

void ScopeUpdater::stop()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
}

void ScopeUpdater::request(const Request &request)
{
    QMutexLocker locker(&m_mutex);
    m_request = request;
    m_hasRequest = true;
    m_wake.wakeAll();
}

bool ScopeUpdater::take(Result &result)
{
    QMutexLocker locker(&m_mutex);
    if (!m_hasResult) return false;
    result = std::move(m_result);
    m_result = Result();
    m_hasResult = false;
    return true;
}

void ScopeUpdater::reset()
{
    QMutexLocker locker(&m_mutex);
    m_reset = true;
    m_hasRequest = false;
    m_hasResult = false;
    m_wake.wakeAll();
}
