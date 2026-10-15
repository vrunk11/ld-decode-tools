/******************************************************************************
 * audiopeaks.h
 * ld-decode-tools TBC library - the shape of audio, for drawing it, and the mix that is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef AUDIOPEAKS_H
#define AUDIOPEAKS_H

#include <QMutex>
#include <QVector>
#include <atomic>
#include <functional>

class AudioReader;

// The lowest and highest value of each channel over each run of samples ("bins"), which is what a waveform draws. Audio is added
// to it as it comes (a live input accumulates, as the SNR graphs do) or all at once from a reader (a capture that is there).
// Adding and reading may be done from different threads.
class AudioPeaks
{
public:
    struct Peak {
        qint16 minLeft = 0, maxLeft = 0, minRight = 0, maxRight = 0;
    };

    explicit AudioPeaks(qint32 samplesPerBin = 1024);

    qint32 samplesPerBin() const { return binSamples; }
    void clear();

    // Stereo samples, interleaved, one pair after the other from where the last call ended
    void add(const qint16 *stereo, qint64 pairs);

    qint64 samples() const;                         // pairs added
    qint64 bins() const;                            // whole bins, and the one being filled
    Peak bin(qint64 index) const;
    // The extremes over the samples first to last (pairs, last excluded), for a column of a drawing
    Peak range(qint64 first, qint64 last) const;

    // Read all of a reader into the peaks, a block at a time; progress(done, total) is called now and then. False if cancelled.
    bool addAll(AudioReader &reader, const std::atomic_bool *cancel = nullptr, const std::function<void(qint64, qint64)> &progress = nullptr);

private:
    mutable QMutex lock;
    qint32 binSamples;
    QVector<Peak> peaks;
    qint64 added = 0;
    qint32 filled = 0;          // samples in the last bin
};

// What is played of the audio: the channels that are on, and the volume. With only one channel on it is played on both sides.
struct AudioMix {
    bool left = true;
    bool right = true;
    double volume = 1.0;        // 0 to 1
};

// in and out are interleaved stereo; they may be the same buffer
void mixStereo(const qint16 *in, qint16 *out, qint64 pairs, const AudioMix &mix);

#endif // AUDIOPEAKS_H
