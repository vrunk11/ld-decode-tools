/******************************************************************************
 * audioring.cpp
 * ld-decode-tools TBC library - the last seconds of audio that is coming, as a ring
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "audioring.h"

#include <QMutexLocker>
#include <cstring>

AudioRing::AudioRing(double seconds, double rate)
    : capacityPairs(qMax<qint64>(1024, static_cast<qint64>(seconds * rate))), sampleRate(rate)
{
    data.resize(static_cast<qsizetype>(capacityPairs * 4));
}

void AudioRing::clear()
{
    QMutexLocker locker(&lock);
    first = end = head = 0;
    carry.clear();
}

qint64 AudioRing::firstSample() const
{
    QMutexLocker locker(&lock);
    return first;
}

qint64 AudioRing::endSample() const
{
    QMutexLocker locker(&lock);
    return end;
}

void AudioRing::append(const QByteArray &bytes)
{
    QMutexLocker locker(&lock);
    QByteArray all = carry + bytes;
    const qint64 pairs = all.size() / 4;
    carry = all.mid(static_cast<qsizetype>(pairs * 4));
    if (pairs == 0) return;

    // A piece longer than the ring: only its end can be kept
    qint64 skip = 0;
    if (pairs > capacityPairs) skip = pairs - capacityPairs;
    const char *source = all.constData() + skip * 4;
    qint64 count = pairs - skip;
    end += skip;
    if (skip > 0) {
        first = end;
        head = 0;
    }

    // Where the next pair goes in the ring, and what is let go to make room for it
    qint64 held = end - first;
    if (held + count > capacityPairs) {
        const qint64 drop = held + count - capacityPairs;
        first += drop;
        head = (head + drop) % capacityPairs;
        held -= drop;
    }
    qint64 write = (head + held) % capacityPairs;
    while (count > 0) {
        const qint64 room = qMin(count, capacityPairs - write);
        std::memcpy(data.data() + write * 4, source, static_cast<size_t>(room) * 4);
        source += room * 4;
        count -= room;
        write = (write + room) % capacityPairs;
        end += room;
    }
}

QVector<qint16> AudioRing::read(qint64 start, qint64 count, qint64 *firstOut) const
{
    QMutexLocker locker(&lock);
    QVector<qint16> out;
    if (start < first) {
        count -= first - start;
        start = first;
    }
    if (firstOut) *firstOut = start;
    if (count < 1 || start >= end) return out;
    count = qMin(count, end - start);
    out.resize(static_cast<qint32>(count * 2));
    qint64 read = (head + (start - first)) % capacityPairs;
    qint64 done = 0;
    while (done < count) {
        const qint64 room = qMin(count - done, capacityPairs - read);
        std::memcpy(out.data() + done * 2, data.constData() + read * 4, static_cast<size_t>(room) * 4);
        done += room;
        read = (read + room) % capacityPairs;
    }
    return out;
}
