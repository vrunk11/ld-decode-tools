/******************************************************************************
 * cvbsaudio.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CVBSAUDIO_H
#define CVBSAUDIO_H

#include <QFile>
#include <QByteArray>
#include <QString>

// Audio of a TBC capture (<base>.pcm: 16-bit signed little-endian stereo, 44.1 kHz
// from ld-decode) and of a CVBS capture (<base>_audio_<pair>.wav: 24-bit stereo
// WAV, 48 kHz from decode-orc). The samples are copied across as they are, only
// changing between 16 and 24 bits; they are never resampled, so the sample rate
// is just the one the audio is declared to have.
namespace CvbsAudio {

// The sample rate ld-decode's TBC audio has, and decode-orc's CVBS audio
constexpr quint32 TBC_SAMPLE_RATE = 44100;
constexpr quint32 CVBS_SAMPLE_RATE = 48000;

// The audio file of a CVBS channel pair: <base>_audio_<pair>.wav
QString wavFileName(const QString &baseName, qint32 pair);

// Copy numberOfSamples stereo samples of 16-bit PCM, from sample firstSample on,
// to a 24-bit WAV declared to be at sampleRate. Fewer are copied if the PCM
// ends sooner; the number copied is returned, or -1, having logged why, on failure.
qint64 pcmToWav(const QString &pcmFileName, qint64 firstSample, qint64 numberOfSamples,
                const QString &wavFileName, quint32 sampleRate);

// What a WAV file holds
struct WavInfo {
    quint32 sampleRate = 0;
    qint32 bits = 0;
    qint64 numberOfSamples = 0;  // stereo samples
};

// Read a WAV header (16 or 24-bit stereo PCM). Returns false, having logged why,
// if the file cannot be read or is another kind of WAV.
bool readWavInfo(const QString &wavFileName, WavInfo &info);

// Copy a WAV to 16-bit PCM, returning the number of stereo samples written, or
// -1, having logged why, on failure.
qint64 wavToPcm(const QString &wavFileName, const QString &pcmFileName);

// Reads ranges of stereo samples from a WAV as 24-bit samples (6 bytes each), for
// copying audio around without losing any of its 24 bits. A 16-bit WAV is widened.
class WavReader
{
public:
    // Returns false, having logged why, if the file cannot be read
    bool open(const QString &wavFileName);
    const WavInfo &info() const { return wavInfo; }

    // count samples from sample start; any beyond the end of the file are silence
    QByteArray read(qint64 start, qint64 count);

private:
    QFile file;
    WavInfo wavInfo;
    qint64 dataOffset = 0;
};

// Writes a 24-bit stereo WAV a piece at a time, filling in the sizes at the end
class WavWriter
{
public:
    ~WavWriter();

    // Returns false, having logged why, if the file cannot be created
    bool open(const QString &wavFileName, quint32 sampleRate);
    // Samples as 24-bit stereo, 6 bytes each
    bool write(const QByteArray &samples);
    // Fill in the sizes and close; returns false if that failed
    bool close();

    qint64 numberOfSamples() const { return samplesWritten; }

private:
    QFile file;
    quint32 sampleRate = 0;
    qint64 samplesWritten = 0;
};

} // namespace CvbsAudio

#endif // CVBSAUDIO_H
