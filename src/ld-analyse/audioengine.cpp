/******************************************************************************
 * audioengine.cpp
 * ld-analyse - the audio that plays with the video
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "audioengine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <algorithm>
#include <cmath>

// What the sound card is given: the audio from a place on, mixed, and silence after its end
class AudioFeeder : public QIODevice
{
public:
    AudioFeeder(AudioReader *reader, qint64 firstSample, const AudioMix &mix) : reader(reader), position(firstSample), mix(mix)
    {
        open(QIODevice::ReadOnly);
    }

    void setMix(const AudioMix &newMix)
    {
        QMutexLocker locker(&lock);
        mix = newMix;
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 pairs = maxSize / 4;
        if (pairs < 1) return 0;
        AudioMix current;
        {
            QMutexLocker locker(&lock);
            current = mix;
        }
        qint16 *out = reinterpret_cast<qint16 *>(data);
        std::memset(data, 0, static_cast<size_t>(pairs) * 4);
        qint64 advance = pairs;
        if (reader && position + pairs > 0) {
            // (a start before the audio begins gives silence up to its first sample)
            const qint64 skip = position < 0 ? -position : 0;
            const QVector<qint16> samples = reader->read(position + skip, pairs - skip);
            const qint64 got = samples.size() / 2;
            if (got > 0) mixStereo(samples.constData(), out + 2 * skip, got, current);
            // Audio that is still coming (a live input, audio being decoded): when it has not come yet, the sound waits for it, with
            // silence, rather than going on past it and never meeting it
            if (reader->isGrowing() && position >= 0 && got < pairs - skip) advance = skip + got;
        }
        // (before the start of the audio nothing is played, and the position goes on, so the sound keeps its place)
        position += advance;
        return pairs * 4;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    AudioReader *reader;
    qint64 position;                // the next pair to give, which may be before the start of the audio
    QMutex lock;
    AudioMix mix;
};

AudioEngine::AudioEngine(QObject *parent) : QObject(parent), audioPeaks(1024) {}

AudioEngine::~AudioEngine()
{
    stop();
    stopPeaks();
    if (audioThread) {
        host->deleteLater();        // (it is deleted in its thread, when the thread ends)
        host = nullptr;
        audioThread->quit();
        audioThread->wait();
    }
}

void AudioEngine::stopPeaks()
{
    if (peaksThread) {
        cancelPeaks = true;
        peaksThread->wait();
        delete peaksThread;
        peaksThread = nullptr;
    }
    peaksReader.reset();
}

QVector<AudioTrackInfo> AudioEngine::setVideo(const QString &videoName)
{
    fieldSamples.clear();
    stop();
    stopPeaks();
    reader.reset();
    chosen = -1;
    audioPeaks.clear();
    peaksFinished = false;
    importedSamples.clear();
    available = AudioReader::tracksOf(videoName);
    return available;
}

bool AudioEngine::selectTrack(qint32 index)
{
    if (index < 0 || index >= available.size()) return false;
    stop();
    stopPeaks();
    audioPeaks.clear();
    peaksFinished = false;

    std::unique_ptr<AudioReader> opened(new AudioReader);
    QString error;
    bool ok = true;
    const bool decoded = available[index].trackId <= -2;
    if (decoded) ok = opened->openGrowing(available[index].where, available[index].sampleRate, error);
    else if (available[index].source == "import" && available[index].trackId == -1) opened->openMemory(importedSamples, importedRate);
    else ok = opened->open(available[index], error);
    if (!ok) {
        reader.reset();
        chosen = -1;
        emit problem(error);
        return false;
    }
    reader = std::move(opened);
    chosen = index;
    if (decoded) {
        extend();
        return true;
    }

    // The shape of the sound, for the waveform, made on a thread of its own: a capture's takes a while. It reads with a reader of
    // its own, so that it does not hold up the one the sound is read from (they share a lock, and a block that was cached).
    cancelPeaks = false;
    peaksReader.reset(new AudioReader);
    QString peaksError;
    if (available[index].source == "import" && available[index].trackId == -1) peaksReader->openMemory(importedSamples, importedRate);
    else if (!peaksReader->open(available[index], peaksError)) peaksReader.reset();
    AudioReader *source = peaksReader ? peaksReader.get() : reader.get();
    peaksThread = QThread::create([this, source]() {
        const bool done = audioPeaks.addAll(*source, &cancelPeaks, [this](qint64 done, qint64 total) {
            if (total > 0) QMetaObject::invokeMethod(this, [this, done, total]() { emit peaksProgress(static_cast<qint32>(done * 100 / total)); }, Qt::QueuedConnection);
        });
        if (done) {
            peaksFinished = true;
            QMetaObject::invokeMethod(this, [this]() { emit peaksFinishedSignal(); }, Qt::QueuedConnection);
        }
    });
    peaksThread->start(QThread::LowPriority);
    return true;
}

void AudioEngine::setMix(bool left, bool right, double volume)
{
    mixLeft = left;
    mixRight = right;
    mixVolume = qBound(0.0, volume, 1.0);
    applyMix();
}

void AudioEngine::setMuted(bool muted)
{
    mixMuted = muted;
    applyMix();
}

void AudioEngine::applyMix()
{
    if (feeder) {
        AudioMix mix;
        mix.left = mixLeft;
        mix.right = mixRight;
        mix.volume = mixMuted ? 0.0 : mixVolume;
        feeder->setMix(mix);
    }
}

void AudioEngine::setOffsetMs(qint32 milliseconds)
{
    offsetMs = milliseconds;
}

double AudioEngine::sampleRate() const { return reader ? reader->sampleRate() : 44100.0; }
qint64 AudioEngine::samples() const { return reader ? reader->samples() : 0; }
double AudioEngine::durationSeconds() const { return reader ? static_cast<double>(reader->samples()) / reader->sampleRate() : 0.0; }

qint64 AudioEngine::sampleAt(double videoSeconds) const
{
    if (usesFieldSamples()) {
        const double position = (videoSeconds - offsetMs / 1000.0) * fieldRate;
        const qint64 last = fieldSamples.size() - 1;
        const qint64 field = qBound<qint64>(0, static_cast<qint64>(std::floor(position)), last - 1);
        const double fraction = qBound(0.0, position - static_cast<double>(field), 1.0);
        return fieldSamples[field] + static_cast<qint64>(fraction * static_cast<double>(fieldSamples[field + 1] - fieldSamples[field]));
    }
    return static_cast<qint64>((videoSeconds - offsetMs / 1000.0) * sampleRate());
}

double AudioEngine::videoSecondsOf(qint64 sample) const
{
    if (usesFieldSamples()) {
        const auto above = std::upper_bound(fieldSamples.constBegin(), fieldSamples.constEnd(), sample);
        const qint64 field = qBound<qint64>(0, static_cast<qint64>(above - fieldSamples.constBegin()) - 1, fieldSamples.size() - 2);
        const qint64 span = fieldSamples[field + 1] - fieldSamples[field];
        const double fraction = span > 0 ? static_cast<double>(sample - fieldSamples[field]) / static_cast<double>(span) : 0.0;
        return (static_cast<double>(field) + fraction) / fieldRate + offsetMs / 1000.0;
    }
    return static_cast<double>(sample) / sampleRate() + offsetMs / 1000.0;
}

bool AudioEngine::start(double videoSeconds)
{
    stop();
    if (!reader || chosen < 0) return false;

    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!deviceId.isEmpty()) {
        for (const QAudioDevice &candidate: QMediaDevices::audioOutputs()) {
            if (candidate.id() == deviceId) device = candidate;
        }
    }
    if (device.isNull()) {
        emit problem(tr("There is no sound card to play the audio on"));
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(static_cast<qint32>(reader->sampleRate()));
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        emit problem(tr("The sound card does not play %1 Hz stereo audio").arg(format.sampleRate()));
        return false;
    }

    AudioMix mix;
    mix.left = mixLeft;
    mix.right = mixRight;
    mix.volume = mixMuted ? 0.0 : mixVolume;
    feeder.reset(new AudioFeeder(reader.get(), sampleAt(videoSeconds), mix));
    // The sound card is fed from a thread of its own: the window is busy at times (a deinterlaced picture takes long), and the sound
    // must not stop for it
    if (!audioThread) {
        audioThread = new QThread(this);
        host = new QObject;
        host->moveToThread(audioThread);
        audioThread->start(QThread::HighPriority);
    }
    processedUs = 0;
    lastClockNs = 0;
    monotonic.start();
    stampNs = 0;
    AudioFeeder *source = feeder.get();
    QMetaObject::invokeMethod(host, [this, device, format, source]() {
        sink = new QAudioSink(device, format);
        sink->setBufferSize(static_cast<qsizetype>(format.sampleRate() * 4 * 0.3));       // (about 300 ms)
        sink->start(source);
        clock = new QTimer;
        QObject::connect(clock, &QTimer::timeout, [this]() {
            if (!sink) return;
            processedUs = sink->processedUSecs();
            stampNs = monotonic.nsecsElapsed();
        });
        clock->start(10);
    }, Qt::BlockingQueuedConnection);
    startedAt = videoSeconds;
    playing = true;
    return true;
}

void AudioEngine::stop()
{
    if (sink && host) {
        QMetaObject::invokeMethod(host, [this]() {
            delete clock;
            clock = nullptr;
            sink->stop();
            delete sink;
            sink = nullptr;
        }, Qt::BlockingQueuedConnection);
    }
    feeder.reset();
    playing = false;
}

double AudioEngine::playedSeconds() const
{
    if (!sink || stampNs.load() == 0) return 0.0;      // (nothing read yet)
    // What the card has played, read every 10 ms, and the time since: so that the clock moves smoothly, as the pictures are due
    const qint64 age = qBound<qint64>(0, monotonic.nsecsElapsed() - stampNs.load(), Q_INT64_C(40000000));
    const qint64 now = qMax(lastClockNs.load(), processedUs.load() * 1000 + age);
    lastClockNs = now;
    return static_cast<double>(now) / 1.0e9;
}

double AudioEngine::videoSecondsNow() const
{
    return startedAt + playedSeconds();
}

qint32 AudioEngine::setImported(const QVector<qint16> &samples, double sampleRate, const QString &label)
{
    importedSamples = samples;
    importedRate = sampleRate;
    for (qint32 i = 0; i < available.size(); i++) {
        if (available[i].source == "import" && available[i].trackId < 0) available.remove(i--);
    }
    AudioTrackInfo info;
    info.label = label;
    info.source = "import";
    info.trackId = -1;
    info.sampleRate = sampleRate;
    available.append(info);
    return static_cast<qint32>(available.size()) - 1;
}

void AudioEngine::clearImported()
{
    importedSamples.clear();
    const bool wasChosen = chosen >= 0 && available[chosen].source == "import" && available[chosen].trackId < 0;
    for (qint32 i = 0; i < available.size(); i++) {
        if (available[i].source == "import" && available[i].trackId < 0) available.remove(i--);
    }
    if (wasChosen) {
        stop();
        stopPeaks();
        reader.reset();
        chosen = -1;
        audioPeaks.clear();
    }
}

qint32 AudioEngine::addGrowing(qint32 slot, const QString &source, const QString &cacheFile, const QString &label, double sampleRate)
{
    removeDecoded(slot);
    AudioTrackInfo info;
    info.label = label;
    info.source = source;
    info.where = cacheFile;
    info.trackId = -2 - slot;
    info.sampleRate = sampleRate;
    available.append(info);
    return static_cast<qint32>(available.size()) - 1;
}

void AudioEngine::removeDecoded(qint32 slot)
{
    for (qint32 i = 0; i < available.size(); i++) {
        if (available[i].trackId != -2 - slot) continue;
        if (i == chosen) {
            stop();
            stopPeaks();
            reader.reset();
            chosen = -1;
            audioPeaks.clear();
        } else if (i < chosen) {
            chosen--;
        }
        available.remove(i--);
    }
}

void AudioEngine::extend()
{
    if (!reader || !isDecoded(chosen)) return;
    reader->refresh();
    const qint64 total = reader->samples();
    qint64 done = audioPeaks.samples();
    while (done < total) {
        const QVector<qint16> block = reader->read(done, qMin<qint64>(65536, total - done));
        if (block.isEmpty()) break;
        audioPeaks.add(block.constData(), block.size() / 2);
        done += block.size() / 2;
    }
    emit peaksProgress(0);
}

void AudioEngine::setFieldSamples(const QVector<qint64> &cumulative, double fieldsPerSecond)
{
    fieldSamples = cumulative.size() >= 2 && fieldsPerSecond > 0.0 ? cumulative : QVector<qint64>();
    fieldRate = fieldsPerSecond;
}

// (only the analogue audio of the capture is cut to the fields: audio decoded from EFM or imported is not)
bool AudioEngine::usesFieldSamples() const
{
    return !fieldSamples.isEmpty() && chosen >= 0 && chosen < available.size() && available[chosen].source == "analogue"
           && available[chosen].trackId > -2;
}
