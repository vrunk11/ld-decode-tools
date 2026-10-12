/******************************************************************************
 * playbackrenderer.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PLAYBACKRENDERER_H
#define PLAYBACKRENDERER_H

#include <QObject>
#include <QImage>
#include <QMap>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <memory>
#include <vector>

#include "tbcsource.h"

// Decodes the frames of the video a little ahead of the playback clock, in threads of
// their own, so that the window is not held up by the chroma decoder. Each worker has a
// TbcSource opened on the same files, which draws the images exactly as the window's does.
//
// The clock tells it which position is due; a worker takes the next position that is not
// already past, so when decoding cannot keep up, it skips to the present rather than
// falling further behind.
class PlaybackRenderer : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackRenderer(QObject *parent = nullptr);
    ~PlaybackRenderer() override;

    // Start rendering from a position, for the settings given. Settings may be changed later
    // with setSettings; the files (and so the metadata read for each worker) stay open
    // until close or a start for other files.
    void start(const TbcSource::RenderSettings &settings, quint32 generation, qint32 firstPosition, qint32 lastPosition);
    void setSettings(const TbcSource::RenderSettings &settings, quint32 generation);
    void stop();
    void close();

    // The position that is due now: workers work from here
    void setDue(qint32 position);

    // The newest image ready for a position no later than the due one, if it is newer than
    // the last one taken. Images for earlier positions are dropped.
    bool take(qint32 due, qint32 &position, QImage &image);

    bool isRunning() const { return !workers.empty(); }
    bool hasFailed() const { return failed; }

signals:
    // An image was finished (emitted from a worker thread)
    void imageReady();

private:
    class Worker;
    friend class Worker;

    QMutex mutex;
    QWaitCondition wake;
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::unique_ptr<TbcSource>> sources;   // kept from one play to the next
    QString openedFor;

    // Shared state, guarded by mutex
    TbcSource::RenderSettings currentSettings;
    quint32 currentGeneration = 0;
    qint32 lastPosition = 0;
    qint32 nextToRender = 0;
    qint32 dueHint = 0;
    qint32 lastTaken = 0;
    bool stopping = false;
    bool failed = false;
    qint32 lookAhead = 4;
    struct Rendered { QImage image; quint32 generation; };
    QMap<qint32, Rendered> ready;
};

#endif // PLAYBACKRENDERER_H
