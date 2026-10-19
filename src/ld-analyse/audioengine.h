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
#include <QElapsedTimer>
#include <atomic>
#include <memory>

#include <QMap>

#include "audiopeaks.h"
#include "audioring.h"
#include "audioreader.h"

class QAudioSink;
class QThread;
class QTimer;
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
    void setMuted(bool muted);
    // Audio brought in by the person (stereo, interleaved), kept in memory, as one more choice of tracks(): the index of it
    qint32 setImported(const QVector<qint16> &samples, double sampleRate, const QString &label);
    void clearImported();
    // Audio being decoded from EFM by a program, in a raw file that grows (slot 0 or 1: the two decoders): one more choice of tracks(),
    // the index of it. extend() is called when the file has grown: the waveform is made as the audio comes.
    qint32 addDecoded(qint32 slot, const QString &cacheFile, const QString &label, double startSeconds = 0.0) { return addGrowing(slot, "efm", cacheFile, label, 44100.0, startSeconds); }
    // The same for any raw 16-bit stereo file that grows (the analogue audio of a live input is slot 2)
    qint32 addGrowing(qint32 slot, const QString &source, const QString &cacheFile, const QString &label, double sampleRate, double startSeconds = 0.0);
    // Audio that arrives and of which only the last part is kept, as the video of a live input (a live input's audio, or audio being decoded
    // from its EFM as it comes): the same, with the ring that has it
    qint32 addRing(qint32 slot, const QString &source, std::shared_ptr<AudioRing> ring, const QString &label, double startSeconds = 0.0);
    // Where in the capture the first sample of a growing track is (known when its first audio comes)
    void setGrowingStart(qint32 slot, double startSeconds);
    void removeDecoded(qint32 slot);
    void extend();
    bool isDecoded(qint32 index) const { return index >= 0 && index < available.size() && available[index].trackId <= -2; }
    // The sound device to play on (the id of a QAudioDevice; empty: the default one)
    void setOutputDevice(const QByteArray &id) { deviceId = id; }

    // The audio is this many milliseconds later than the video (negative: earlier)
    void setOffsetMs(qint32 milliseconds);

    double sampleRate() const;
    qint64 samples() const;
    double durationSeconds() const;
    AudioPeaks &peaks() { return audioPeaks; }
    bool peaksComplete() const { return peaksFinished; }
    // The audio samples of each field, from the metadata (the analogue audio of a capture is cut to the fields): then the place in
    // the audio of a moment of the video is exact, not the time times the sample rate. cumulative[i] is the number of samples
    // before field i (from 0), and has one more entry than there are fields. Empty: no map.
    void setFieldSamples(const QVector<qint64> &cumulative, double fieldsPerSecond);
    bool hasFieldSamples() const { return !fieldSamples.isEmpty(); }
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
    // What the player gave the card since this was asked last: the loudest sample, and since start() how many pairs were real audio
    qint32 takeOutputLevel();
    qint64 pairsGiven() const;
    qint64 pairsWaited() const;       // pairs of silence given while the audio had not come yet (audio that grows: live, or being decoded)
    qint64 waitsForAudio() const;     // how many times it had to wait
    qint64 samplePlaying() const;     // the sample of the track the card is being given now

signals:
    void peaksProgress(qint32 percent);
    void peaksFinishedSignal();
    void problem(const QString &message);

private:
    void stopPeaks();
    void applyMix();

    QVector<AudioTrackInfo> available;
    qint32 chosen = -1;
    std::unique_ptr<AudioReader> reader;
    std::unique_ptr<AudioReader> peaksReader;      // (the thread of the waveform reads with this one)
    AudioPeaks audioPeaks;
    std::atomic_bool cancelPeaks{false};
    std::atomic_bool peaksFinished{false};
    QThread *peaksThread = nullptr;

    QAudioSink *sink = nullptr;
    QThread *audioThread = nullptr;     // the sink lives in it
    QObject *host = nullptr;            // (what runs in that thread)
    QTimer *clock = nullptr;            // (in that thread: reads how much the sink has played)
    std::atomic<qint64> processedUs{0};
    std::atomic<qint64> stampNs{0};                // (when, on monotonic, processedUs was read)
    mutable std::atomic<qint64> lastClockNs{0};    // (the clock never goes back)
    QElapsedTimer monotonic;
    std::unique_ptr<AudioFeeder> feeder;
    bool playing = false;
    double startedAt = 0.0;        // the video second the sound started at

    bool mixLeft = true, mixRight = true;
    double mixVolume = 1.0;
    bool mixMuted = false;
    qint32 offsetMs = 0;
    QVector<qint64> fieldSamples;
    double fieldRate = 0.0;
    bool usesFieldSamples() const;
    double trackStartSeconds() const;
    QByteArray deviceId;
    QMap<qint32, std::shared_ptr<AudioRing>> rings;        // (by slot)
    QVector<qint16> importedSamples;
    double importedRate = 44100.0;
};

#endif // AUDIOENGINE_H
