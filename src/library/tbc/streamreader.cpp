/******************************************************************************
 * streamreader.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "streamreader.h"

#include <QDebug>
#include <algorithm>
#include <cstring>

#include "tbc/logging.h"

StreamReader::StreamReader(FILE *_stream, bool _ownsStream, qint32 _fieldLength, const Options &_options)
    : stream(_stream), ownsStream(_ownsStream), fieldLength(_fieldLength), options(_options)
{
    if (options.cvbs) {
        geometry = Cvbs::geometry(options.system);
        converter = std::make_unique<Cvbs::SampleConverter>(options.encoding, options.system);
        padValue = static_cast<quint16>(Cvbs::standardLevels(options.system).blanking * 64);
        fieldsPerUnit = 2;
        unitBytes = geometry.frameSamples * 2;
    } else {
        fieldsPerUnit = 1;
        unitBytes = static_cast<qint64>(fieldLength) * 2;
    }
    // bufferFrames frames, in units
    windowUnits = std::max(options.bufferFrames, 1) * 2 / fieldsPerUnit;
    aheadUnits = (options.aheadFrames > 0 ? options.aheadFrames : std::max(options.bufferFrames, 1)) * 2 / fieldsPerUnit;
}

StreamReader::~StreamReader()
{
    stop();
    if (ownsStream && !isRunning()) fclose(stream);
}

void StreamReader::stop()
{
    {
        QMutexLocker locker(&mutex);
        stopRequested = true;
        spaceFreed.wakeAll();
        unitsAdded.wakeAll();
    }

    // A reader waiting for the window to move has stopped by now; one blocked
    // reading a stream that is not producing anything is given up on
    if (isRunning() && !wait(500)) {
        terminate();
        wait();
    }
}

void StreamReader::run()
{
    while (true) {
        {
            // Wait until the users have got near enough to the end of what is read
            QMutexLocker locker(&mutex);
            while (!stopRequested && unitsRead - furthestRequested > aheadUnits) spaceFreed.wait(&mutex);
            if (stopRequested) break;
        }

        // Read a whole field (or frame), straight into the memory it will stay in. This
        // is all that is done between reads: see the class description.
        Unit unit;
        qint64 got = 0;
        char *destination;
        if (options.cvbs) {
            unit.raw.resize(static_cast<qint32>(unitBytes));
            destination = unit.raw.data();
        } else {
            unit.fields[0].resize(fieldLength);
            unit.converted = true;
            destination = reinterpret_cast<char *>(unit.fields[0].data());
        }
        while (got < unitBytes) {
            const size_t count = fread(destination + got, 1, static_cast<size_t>(unitBytes - got), stream);
            if (count == 0) break;
            got += count;
        }

        if (got != unitBytes && got > 0) {
            qWarning() << "The input stream ended in the middle of a" << (options.cvbs ? "frame" : "field")
                       << "- the last" << got << "bytes are ignored";
        }

        QMutexLocker locker(&mutex);
        if (got != unitBytes) {
            ended = true;
            unitsAdded.wakeAll();
            return;
        }
        window.insert(unitsRead++, std::move(unit));

        // Units too far behind the furthest one asked for are let go
        const qint32 oldest = furthestRequested - windowUnits;
        while (!window.isEmpty() && window.firstKey() < oldest) window.erase(window.begin());
        unitsAdded.wakeAll();
    }

    QMutexLocker locker(&mutex);
    ended = true;
    unitsAdded.wakeAll();
}

bool StreamReader::waitFor(qint32 unit)
{
    if (unit > furthestRequested) {
        furthestRequested = unit;
        spaceFreed.wakeAll();
    }
    while (unit >= unitsRead && !ended) unitsAdded.wait(&mutex);
    return unit < unitsRead;
}

bool StreamReader::hasField(qint32 index)
{
    QMutexLocker locker(&mutex);
    return waitFor(index / fieldsPerUnit);
}

bool StreamReader::getField(qint32 index, Data &data)
{
    const qint32 unitIndex = index / fieldsPerUnit;
    const qint32 which = index % fieldsPerUnit;

    QByteArray raw;
    {
        QMutexLocker locker(&mutex);
        if (!waitFor(unitIndex)) return false;

        const auto found = window.constFind(unitIndex);
        if (found == window.constEnd()) {
            qFatal("Field %d of the input stream was asked for after the reader had moved on to field %d: "
                   "it is gone, as a stream cannot be read again (the buffer holds %d frames - "
                   "a larger buffer, or reading in order, would avoid this)",
                   index + 1, furthestRequested * fieldsPerUnit + 1, windowUnits * fieldsPerUnit / 2);
        }
        if (found->converted) {
            data = found->fields[which];
            return true;
        }
        raw = found->raw;
    }

    // Convert the frame into its two fields, away from the reader and the other users
    Data fields[2] = {Data(fieldLength), Data(fieldLength)};
    Cvbs::frameToFields(reinterpret_cast<const qint16 *>(raw.constData()), geometry, *converter, padValue,
                        fields[0].data(), fields[1].data());
    data = fields[which];

    QMutexLocker locker(&mutex);
    const auto found = window.find(unitIndex);
    if (found != window.end() && !found->converted) {
        found->fields[0] = fields[0];
        found->fields[1] = fields[1];
        found->converted = true;
        found->raw.clear();
    }
    return true;
}

qint32 StreamReader::numberOfFields()
{
    QMutexLocker locker(&mutex);
    return ended ? unitsRead * fieldsPerUnit : -1;
}

qint32 StreamReader::fieldsRead()
{
    QMutexLocker locker(&mutex);
    return unitsRead * fieldsPerUnit;
}

qint32 StreamReader::firstFieldKept()
{
    QMutexLocker locker(&mutex);
    return window.isEmpty() ? unitsRead * fieldsPerUnit : window.firstKey() * fieldsPerUnit;
}

qint32 StreamReader::framesAhead()
{
    QMutexLocker locker(&mutex);
    return std::max(0, unitsRead - furthestRequested - 1) * fieldsPerUnit / 2;
}
