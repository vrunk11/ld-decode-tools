/******************************************************************************
 * audioreader.h
 * ld-decode-tools TBC library - the audio of a capture, as stereo samples to play
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef AUDIOREADER_H
#define AUDIOREADER_H

#include <QString>
#include <QVector>
#include <memory>

// One audio source a capture has, to choose from
struct AudioTrackInfo {
    QString label;              // for a person: "3: capture (analogue, 44.1 kHz)"
    QString source;             // "analogue", "efm" or "import"
    QString where;              // the capture it goes with (a .tbc, .cvbs or .ldx name), or the file it is read from
    qint32 trackId = 0;         // the id of the pcm track of an LDX file, 0 for a file
    double sampleRate = 44100;
    qint32 bits = 16;
};

// Reads the audio of a capture as 16-bit stereo samples (a left and a right for each sample, in turn), whatever it is kept as:
// a pcm track of an LDX file (16 bits, or the 24 bits of a CVBS capture, which are brought to 16 for playing), the .pcm file
// beside a .tbc, the _audio_0.wav beside a .cvbs, or samples held in memory (audio that was imported).
class AudioReader
{
public:
    AudioReader();
    ~AudioReader();

    AudioReader(const AudioReader &) = delete;
    AudioReader &operator=(const AudioReader &) = delete;

    // The audio a capture has: the pcm tracks of its LDX file (analogue, decoded from EFM, imported), or the audio file beside it
    static QVector<AudioTrackInfo> tracksOf(const QString &videoName);

    bool open(const AudioTrackInfo &track, QString &error);
    // Samples held in memory (stereo, interleaved): imported audio
    void openMemory(const QVector<qint16> &samples, double sampleRate);

    qint64 samples() const;             // pairs of samples
    double sampleRate() const;
    // count pairs from sample start (fewer if the audio ends), 2 * count values
    QVector<qint16> read(qint64 start, qint64 count);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // AUDIOREADER_H
