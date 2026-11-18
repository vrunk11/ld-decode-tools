/******************************************************************************
 * scopeupdater.h
 * ld-analyse - the data of the scopes, made away from the window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SCOPEUPDATER_H
#define SCOPEUPDATER_H

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <memory>

#include "tbcsource.h"
#include "vectorscopedialog.h"

// The line scope and the vector scope need the fields of the frame read and the frame decoded (the chroma decoder), and the vector scope draws a few hundred
// thousand points. Done in the thread of the window, that is what held up the playback (the frames are decoded in threads of their own for the same reason).
// This does it in a thread of its own, on a TbcSource opened on the same files: the window asks for what it needs, and takes what is made when it is told.
// Only the last thing asked for is made: when the thread is behind, what was asked for in between is dropped, as the playback skips frames.
class ScopeUpdater : public QObject
{
    Q_OBJECT
public:
    struct Request {
        TbcSource::RenderSettings settings;
        quint32 generation = 0;         // of the settings (a change of them makes the thread apply them again)
        qint32 frame = 0, field = 0;
        qint32 line = 1, dot = 0;       // of the line scope
        bool wantLine = false, wantVector = false;
        VectorscopeDialog::Options vectorOptions;
    };

    struct Result {
        bool failed = false;
        bool hasLine = false, hasVector = false;
        TbcSource::ScanLineData scanLine;
        qint32 line = 1, dot = 0, frameWidth = 0, frameHeight = 0;
        bool bothSources = false;
        QImage vectorImage;
    };

    explicit ScopeUpdater(QObject *parent = nullptr);
    ~ScopeUpdater() override;

    // Ask for the scopes of a frame (the last asking is the one that is made)
    void request(const Request &request);
    // What was made (once: false when there is nothing new)
    bool take(Result &result);
    // The source that is open changed: what is open here is closed
    void reset();

signals:
    void ready();

private:
    class Worker;
    void stop();

    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_hasRequest = false;
    bool m_stopping = false;
    bool m_reset = false;
    Request m_request;
    bool m_hasResult = false;
    Result m_result;
    Worker *m_worker = nullptr;
};

#endif // SCOPEUPDATER_H
