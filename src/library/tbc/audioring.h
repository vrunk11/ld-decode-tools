/******************************************************************************
 * audioring.h
 * ld-decode-tools TBC library - the last seconds of audio that is coming, as a ring
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef AUDIORING_H
#define AUDIORING_H

#include <QByteArray>
#include <QMutex>
#include <QVector>

// 16-bit stereo audio that is arriving (the audio of a live input, or audio being decoded from its EFM) of which only the last part is
// kept, as the video of a live input is: what is older is let go, so a capture that goes on for hours does not fill the memory or a disc.
// Samples are counted from the first that came, and go on counting: a sample has its number for good, kept or not, so the place of a
// moment of the video in it is the same however much has been let go. Adding and reading may be done from different threads.
class AudioRing
{
public:
    // seconds: how much is kept; rate: samples (pairs) a second
    explicit AudioRing(double seconds = 120.0, double rate = 44100.0);

    // Stereo samples as bytes (4 for a pair, little-endian); a piece of a pair is kept for the next call
    void append(const QByteArray &bytes);
    void clear();

    qint64 firstSample() const;         // the oldest sample still held (pairs)
    qint64 endSample() const;           // the number of the next one to come: every sample before it has come
    double rate() const { return sampleRate; }

    // count pairs from sample start (fewer if the ring ends or the oldest were let go: from the first held), 2 * count values; firstOut
    // is the number of the first pair given
    QVector<qint16> read(qint64 start, qint64 count, qint64 *firstOut = nullptr) const;

private:
    mutable QMutex lock;
    QByteArray data;                    // the pairs held, a pair after the other, as a ring
    qint64 capacityPairs;
    double sampleRate;
    qint64 first = 0;                   // the number of the oldest pair held
    qint64 end = 0;                     // the number of the next pair
    qint64 head = 0;                    // where in data (in pairs) the pair numbered `first` is
    QByteArray carry;                   // the beginning of a pair that was cut
};

#endif // AUDIORING_H
