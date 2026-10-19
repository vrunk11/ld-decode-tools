/******************************************************************************
 * audiopeaks.cpp
 * ld-decode-tools TBC library - the shape of audio, for drawing it, and the mix that is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "audiopeaks.h"

#include <QMutexLocker>
#include <algorithm>

#include "audioreader.h"

AudioPeaks::AudioPeaks(qint32 samplesPerBin) : binSamples(qMax(1, samplesPerBin)) {}

void AudioPeaks::clear()
{
    QMutexLocker locker(&lock);
    peaks.clear();
    added = 0;
    filled = 0;
}

void AudioPeaks::add(const qint16 *stereo, qint64 pairs)
{
    QMutexLocker locker(&lock);
    for (qint64 i = 0; i < pairs; i++) {
        const qint16 left = stereo[2 * i];
        const qint16 right = stereo[2 * i + 1];
        if (peaks.isEmpty() || filled >= binSamples) {
            Peak first;
            first.minLeft = first.maxLeft = left;
            first.minRight = first.maxRight = right;
            peaks.append(first);
            filled = 1;
        } else {
            Peak &peak = peaks.last();
            peak.minLeft = std::min(peak.minLeft, left);
            peak.maxLeft = std::max(peak.maxLeft, left);
            peak.minRight = std::min(peak.minRight, right);
            peak.maxRight = std::max(peak.maxRight, right);
            filled++;
        }
    }
    added += pairs;
}

qint64 AudioPeaks::samples() const
{
    QMutexLocker locker(&lock);
    return added;
}

qint64 AudioPeaks::bins() const
{
    QMutexLocker locker(&lock);
    return peaks.size();
}

AudioPeaks::Peak AudioPeaks::bin(qint64 index) const
{
    QMutexLocker locker(&lock);
    return (index >= 0 && index < peaks.size()) ? peaks[static_cast<qint32>(index)] : Peak();
}

AudioPeaks::Peak AudioPeaks::range(qint64 first, qint64 last) const
{
    QMutexLocker locker(&lock);
    Peak result;
    if (peaks.isEmpty() || last <= first) return result;
    qint64 from = qBound<qint64>(0, first / binSamples, peaks.size() - 1);
    qint64 to = qBound<qint64>(from, (last - 1) / binSamples, peaks.size() - 1);
    result = peaks[static_cast<qint32>(from)];
    for (qint64 i = from + 1; i <= to; i++) {
        const Peak &peak = peaks[static_cast<qint32>(i)];
        result.minLeft = std::min(result.minLeft, peak.minLeft);
        result.maxLeft = std::max(result.maxLeft, peak.maxLeft);
        result.minRight = std::min(result.minRight, peak.minRight);
        result.maxRight = std::max(result.maxRight, peak.maxRight);
    }
    return result;
}

bool AudioPeaks::addAll(AudioReader &reader, const std::atomic_bool *cancel, const std::function<void(qint64, qint64)> &progress)
{
    constexpr qint64 Block = 1 << 16;
    const qint64 total = reader.samples();
    for (qint64 position = 0; position < total; position += Block) {
        if (cancel && cancel->load()) return false;
        const QVector<qint16> block = reader.read(position, Block);
        if (block.isEmpty()) break;
        add(block.constData(), block.size() / 2);
        if (progress) progress(position + block.size() / 2, total);
    }
    return true;
}

void mixStereo(const qint16 *in, qint16 *out, qint64 pairs, const AudioMix &mix)
{
    const double volume = qBound(0.0, mix.volume, 1.0);
    auto scaled = [volume](qint32 value) {
        return static_cast<qint16>(qBound<qint32>(-32768, static_cast<qint32>(value * volume), 32767));
    };
    for (qint64 i = 0; i < pairs; i++) {
        const qint32 left = in[2 * i];
        const qint32 right = in[2 * i + 1];
        qint16 outLeft = 0, outRight = 0;
        if (mix.left && mix.right) {
            outLeft = scaled(left);
            outRight = scaled(right);
        } else if (mix.left) {
            outLeft = outRight = scaled(left);
        } else if (mix.right) {
            outLeft = outRight = scaled(right);
        }
        out[2 * i] = outLeft;
        out[2 * i + 1] = outRight;
    }
}

void AudioPeaks::addSilence(qint64 pairs)
{
    QMutexLocker locker(&lock);
    while (pairs > 0) {
        if (peaks.isEmpty() || filled >= binSamples) {
            peaks.append(Peak());
            filled = 0;
        }
        const qint64 take = std::min<qint64>(pairs, binSamples - filled);
        Peak &peak = peaks.last();
        peak.minLeft = std::min<qint16>(peak.minLeft, 0);
        peak.maxLeft = std::max<qint16>(peak.maxLeft, 0);
        peak.minRight = std::min<qint16>(peak.minRight, 0);
        peak.maxRight = std::max<qint16>(peak.maxRight, 0);
        filled += static_cast<qint32>(take);
        added += take;
        pairs -= take;
    }
}
