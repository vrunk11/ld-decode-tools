/******************************************************************************
 * fieldpool.h
 * ld-decode-tools - hands the fields of a capture out to worker threads and writes the metadata back
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FIELDPOOL_H
#define FIELDPOOL_H

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QThread>
#include <functional>

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// What ld-process-vbi and ld-process-vits do: every field of a capture is read once, a thread works on it, and what it found goes into the metadata of that
// field, which is written when the last field is done. The programs say what a worker is, which lines of the field it needs, and which part of the
// metadata of a field it changes.
class FieldPool
{
public:
    struct Settings {
        QString name;                   // "VBI", "VITS": in the messages
        qint32 firstLine = -1;          // the lines of the field that the workers read (all of the field if firstLine is -1)
        qint32 lastLine = -1;
        // A worker: it runs until the pool has no field left, or until abort is set
        std::function<QThread *(QAtomicInt &abort, FieldPool &pool)> createWorker;
        // Store what a worker found for a field in the metadata
        std::function<void(LdDecodeMetaData &metadata, qint32 fieldNumber, const LdDecodeMetaData::Field &field)> storeField;
    };

    explicit FieldPool(const Settings &settings, QString inputFilename, QString outputMetadataFilename,
                       qint32 maxThreads, LdDecodeMetaData &ldDecodeMetaData);

    // Process the whole capture and write the metadata; false if it failed
    bool process();

    // Member functions used by worker threads
    bool getInputField(qint32 &fieldNumber, SourceVideo::Data &fieldVideoData, LdDecodeMetaData::Field &fieldMetadata,
                       LdDecodeMetaData::VideoParameters &videoParameters);
    bool setOutputField(qint32 fieldNumber, const LdDecodeMetaData::Field &fieldMetadata);

private:
    Settings settings;
    QString inputFilename;
    QString outputMetadataFilename;
    qint32 maxThreads;
    QElapsedTimer totalTimer;

    // Atomic abort flag shared by worker threads; workers watch this, and shut
    // down as soon as possible if it becomes true
    QAtomicInt abort;

    // Input stream information (all guarded by inputMutex while threads are running)
    QMutex inputMutex;
    qint32 inputFieldNumber = 1;
    qint32 lastFieldNumber = 0;
    LdDecodeMetaData &ldDecodeMetaData;
    SourceVideo sourceVideo;

    // Output stream information (all guarded by outputMutex while threads are running)
    QMutex outputMutex;
};

#endif // FIELDPOOL_H
