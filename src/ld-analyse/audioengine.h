/******************************************************************************
 * audioengine.h
 * ld-analyse - the audio that plays with the video
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <QObject>
#include <QVector>
#include <atomic>
#include <memory>

#include "audiopeaks.h"
#include "audioreader.h"

class QAudioSink;
class QThread;
class AudioFeeder;

// The audio of the capture being shown: which tracks it has, the one that is chosen with its peaks (for the waveform), and
// the playing of it. Audio plays only while the video does: the video starts it at a moment of the video and stops it, and,
// at normal speed, takes its clock from what the sound card has played (playedSeconds), so that picture and sound stay together.
class AudioEngine : public QObject
{
    Q_OBJECT
public:
    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    // The audio a capture has to choose from; none if it has none. Closes the one that was chosen.
    QVector<AudioTrackInfo> setVideo(const QString &videoName);
    const QVector<AudioTrackInfo> &tracks() const { return available; }
    bool hasAudio() const { return !available.isEmpty(); }

    // Choose one (an index of tracks()): it is opened, and its peaks are made on a thread of its own
    bool selectTrack(qint32 index);
    qint32 currentTrack() const { return chosen; }

    // The channels that are played and the volume (0 to 1)
    void setMix(bool left, bool right, double volume);
    // Audio brought in by the person (stereo, interleaved), kept in memory, as one more choice of tracks(): the index of it
    qint32 setImported(const QVector<qint16> &samples, double sampleRate, const QString &label);
    void clearImported();
    // The sound device to play on (the id of a QAudioDevice; empty: the default one)
    void setOutputDevice(const QByteArray &id) { deviceId = id; }

    // The audio is this many milliseconds later than the video (negative: earlier)
    void setOffsetMs(qint32 milliseconds);

    double sampleRate() const;
    qint64 samples() const;
    double durationSeconds() const;
    AudioPeaks &peaks() { return audioPeaks; }
    bool peaksComplete() const { return peaksFinished; }
    // The place in the audio of a moment of the video, in pairs of samples, and the other way
    qint64 sampleAt(double videoSeconds) const;
    double videoSecondsOf(qint64 sample) const;

    // Play from a moment of the video; true if sound is playing (false if there is none, or no sound card)
    bool start(double videoSeconds);
    void stop();
    bool isPlaying() const { return playing; }
    // Seconds of sound played since start(): the clock of the video
    double playedSeconds() const;
    // The video second the sound is at
    double videoSecondsNow() const;

signals:
    void peaksProgress(qint32 percent);
    void peaksFinishedSignal();
    void problem(const QString &message);

private:
    void stopPeaks();

    QVector<AudioTrackInfo> available;
    qint32 chosen = -1;
    std::unique_ptr<AudioReader> reader;
    AudioPeaks audioPeaks;
    std::atomic_bool cancelPeaks{false};
    std::atomic_bool peaksFinished{false};
    QThread *peaksThread = nullptr;

    QAudioSink *sink = nullptr;
    std::unique_ptr<AudioFeeder> feeder;
    bool playing = false;
    double startedAt = 0.0;        // the video second the sound started at

    bool mixLeft = true, mixRight = true;
    double mixVolume = 1.0;
    qint32 offsetMs = 0;
    QByteArray deviceId;
    QVector<qint16> importedSamples;
    double importedRate = 44100.0;
};

#endif // AUDIOENGINE_H
