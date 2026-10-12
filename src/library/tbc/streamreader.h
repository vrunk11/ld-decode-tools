/******************************************************************************
 * streamreader.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STREAMREADER_H
#define STREAMREADER_H

#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QThread>
#include <QVector>
#include <QWaitCondition>
#include <cstdio>
#include <memory>

#include "cvbs.h"

// Reads video fields from a stream that cannot be seeked (standard input, a named
// pipe) on a thread of its own, into a sliding window. The users, which may be
// several threads asking for fields in a roughly increasing order, never wait on the
// I/O itself, and a producer that is slower, or bursty, is smoothed out.
//
// The thread only reads: it does nothing else between one read and the next, so
// that it is always ready for more. A pipe that is not emptied at once fills up,
// and a producer blocked on a full pipe can take many milliseconds to be woken
// again, which would hold the whole stream up at a few MB/s. Converting what is
// read to fields is left to the ones asking for them.
//
// Window: the reader runs at most bufferFrames frames ahead of the furthest field
// asked for, and the fields up to bufferFrames frames behind it are kept. A field
// asked for from before the window is an error (it is gone and cannot be read
// again); one asked for beyond the end of the stream does not exist.
class StreamReader : public QThread
{
public:
    using Data = QVector<quint16>;

    struct Options {
        // Frames of CVBS (a first field and a second field, as in a .cvbs file)
        // instead of fields of TBC
        bool cvbs = false;
        VideoSystem system = NTSC;
        Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
        // Frames read ahead of the furthest one asked for, and kept behind it
        qint32 bufferFrames = 32;
        // A live ring instead: the producer is never held up. The last 2 * bufferFrames frames are
        // kept, the older ones overwritten, apart from a range that is protected (see protectFields).
        // For video that arrives as it is made, not for a file that is read as fast as it can be.
        bool ring = false;
    };

    // The stream is read from (blocking) until it ends or the reader is stopped, and must
    // stay open for that long; it is closed with the reader if ownsStream. fieldLength is
    // the samples in a TBC field.
    StreamReader(FILE *stream, bool ownsStream, qint32 fieldLength, const Options &options);
    ~StreamReader() override;

    // Stop reading. A reader blocked in a read that never returns is given up on.
    void stop();

    // Wait for the 0-based field to be read: false if the stream ended before it
    bool hasField(qint32 index);
    // Get the field, waiting for it if need be: false if the stream ended before it.
    // Aborts, saying why, if the field has already left the window.
    bool getField(qint32 index, Data &data);

    // The number of fields in the stream: -1 until it has ended
    qint32 numberOfFields();
    // Fields read so far
    qint32 fieldsRead();
    // CVBS only: whether the first frame holds samples beyond 10 bits, which says the encoding is
    // not the default (u10) that was asked for; with no metadata on a pipe that is easy to get wrong
    bool looksWiderThan10Bit();
    // The first field (0-based) still in the window: earlier ones are gone
    qint32 firstFieldKept();

    // Ring mode. Hold on to the fields first to last (0-based, and those between) against being
    // overwritten, as a frozen picture to go through while the input goes on filling the ring (two
    // buffers: the frozen one and the live one). One range at a time; clear it to let it go.
    void protectFields(qint32 firstField, qint32 lastField);
    void clearProtection();
    // Whether the field is still held, and the first held one at or after it (-1 if none)
    bool isFieldHeld(qint32 index);
    qint32 firstFieldHeldAtOrAfter(qint32 index);

protected:
    void run() override;

private:
    // What is read at a time: a TBC field, or a CVBS frame (two fields)
    struct Unit {
        QByteArray raw;
        Data fields[2];
        bool converted = false;
    };

    FILE *stream;
    bool ownsStream;
    qint32 fieldLength;
    Options options;
    qint32 fieldsPerUnit;      // 1 for TBC, 2 for CVBS
    qint64 unitBytes;
    qint32 windowUnits;
    qint32 ringUnits = 0;
    qint32 protectedFirst = -1, protectedLast = -1;     // in units

    Cvbs::Geometry geometry;
    std::unique_ptr<Cvbs::SampleConverter> converter;
    quint16 padValue = 0;

    QMutex mutex;
    QWaitCondition unitsAdded;
    QWaitCondition spaceFreed;
    QMap<qint32, Unit> window;
    qint32 unitsRead = 0;
    qint32 furthestRequested = -1;     // in units
    bool ended = false;
    bool stopRequested = false;
    bool wideSamples = false;

    // Note that unit has been asked for, and wait until it is read or the stream ends.
    // mutex must be held. Returns true if the unit was read.
    bool waitFor(qint32 unit);
};

#endif // STREAMREADER_H
