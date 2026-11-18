/******************************************************************************
 * stackaudio.cpp
 * ld-analyse - the audio of a stack: the audio of its sources, one after the other
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "stackaudio.h"

#include <QDir>
#include <QMetaObject>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kChunkPairs = 44100;      // (a second)
constexpr qint64 kFadePairs = 441;         // (10 ms: where the audio goes from a source to another, or to silence)

// The tracks a source has: those of the capture the stack reads (its mapping, when it follows one, may have none: then the capture it was made from, which the
// mapping has not put in order, so its audio comes as it was)
QVector<AudioTrackInfo> tracksOfSpan(const StackAudioSpan &span)
{
    QVector<AudioTrackInfo> tracks;
    if (!span.reference.isEmpty()) tracks = AudioReader::tracksOf(span.reference);
    if (tracks.isEmpty() && !span.original.isEmpty()) tracks = AudioReader::tracksOf(span.original);
    return tracks;
}

bool find(const QVector<AudioTrackInfo> &tracks, const char *source, AudioTrackInfo &found)
{
    for (const AudioTrackInfo &track: tracks) {
        if (track.source == QLatin1String(source)) {
            found = track;
            return true;
        }
    }
    return false;
}

QString clock(double seconds)
{
    const qint64 total = static_cast<qint64>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

}

StackAudio::StackAudio(QObject *parent) : QObject(parent)
{
}

StackAudio::~StackAudio()
{
    stop();
}

QString StackAudio::cacheFile() const
{
    return cache ? cache->fileName() : QString();
}

QVector<StackAudio::Segment> StackAudio::choose(const QVector<qint32> &start, const QVector<qint32> &end, const QVector<bool> &hasAudio, qint32 timelineFrames)
{
    QVector<Segment> segments;
    const qint32 count = static_cast<qint32>(start.size());
    qint32 position = 0;
    while (position < timelineFrames) {
        // Of the sources that have audio here, the one that goes on the longest
        qint32 best = -1;
        qint32 bestEnd = position;
        for (qint32 i = 0; i < count; i++) {
            if (!hasAudio.value(i) || start[i] > position || end[i] <= position) continue;
            if (end[i] > bestEnd) {
                best = i;
                bestEnd = end[i];
            }
        }
        if (best >= 0) {
            const qint32 to = std::min(bestEnd, timelineFrames);
            segments.append({ position, to, best });
            position = to;
            continue;
        }
        // None here: silence, until the next source that has audio begins
        qint32 next = timelineFrames;
        for (qint32 i = 0; i < count; i++) {
            if (hasAudio.value(i) && start[i] > position) next = std::min(next, start[i]);
        }
        if (next <= position) break;
        segments.append({ position, next, -1 });
        position = next;
    }
    return segments;
}

StackAudio::Pick StackAudio::pick(const StackAudioSpan &span, Kind kind, const EfmOf &efmOf)
{
    Pick result;
    const QVector<AudioTrackInfo> tracks = tracksOfSpan(span);
    AudioTrackInfo found;
    const auto analogue = [&]() {
        if (!find(tracks, "analogue", found)) return false;
        result.has = true;
        result.track = found;
        return true;
    };
    const auto digital = [&]() {
        if (find(tracks, "efm", found)) {
            result.has = true;
            result.track = found;
            return true;
        }
        // (no audio decoded from it in the file: the EFM, if it has some, is decoded when it is asked for)
        const QString efm = efmOf ? efmOf(span) : QString();
        if (efm.isEmpty()) return false;
        result.has = true;
        result.efm = efm;
        return true;
    };
    const auto imported = [&]() {
        if (!find(tracks, "import", found)) return false;
        result.has = true;
        result.track = found;
        return true;
    };
    switch (kind) {
    case Kind::Analogue:
        analogue();
        break;
    case Kind::Digital:
        digital();
        break;
    case Kind::Imported:
        imported();
        break;
    case Kind::Mixed:
        // (what the source has: the analogue audio, else the digital audio, else what was imported)
        if (!analogue() && !digital()) imported();
        break;
    }
    return result;
}

bool StackAudio::available(const QVector<StackAudioSpan> &spans, Kind kind, const EfmOf &efmOf)
{
    for (const StackAudioSpan &span: spans) {
        if (pick(span, kind, efmOf).has) return true;
    }
    return false;
}

void StackAudio::stop()
{
    cancel = true;
    {
        QMutexLocker locker(&decodedMutex);
        decodedWake.wakeAll();
    }
    if (worker) {
        worker->wait();
        delete worker;
        worker = nullptr;
    }
    running = false;
}

void StackAudio::efmDecoded(qint32 source, const QString &pcmFile)
{
    QMutexLocker locker(&decodedMutex);
    decoded[source] = pcmFile;
    decodedWake.wakeAll();
}

bool StackAudio::prepare(const QVector<StackAudioSpan> &given, qint32 timelineFrames, double framesPerSecond, Kind kind, const EfmOf &efmOf, QString &error)
{
    stop();
    cancel = false;
    started = false;
    plan.clear();
    text.clear();
    picks.clear();
    decoded.clear();
    cache.reset();
    if (framesPerSecond <= 0.0 || timelineFrames <= 0) {
        error = tr("The stack has no frames");
        return false;
    }
    audioKind = kind;
    spans = given;
    fps = framesPerSecond;

    QVector<bool> hasAudio(spans.size(), false);
    QVector<qint32> starts, ends;
    for (qint32 i = 0; i < spans.size(); i++) {
        picks.append(pick(spans[i], kind, efmOf));
        hasAudio[i] = picks.last().has;
        starts.append(spans[i].start);
        ends.append(spans[i].start + spans[i].frames);
    }
    plan = choose(starts, ends, hasAudio, timelineFrames);
    bool any = false;
    for (const Segment &segment: plan) any |= segment.source >= 0;
    if (!any) {
        plan.clear();
        error = tr("No source of the stack has audio of that kind");
        return false;
    }

    QStringList lines;
    for (const Segment &segment: plan) {
        if (segment.source < 0) continue;
        lines << tr("source %1, %2 to %3").arg(segment.source + 1).arg(clock(segment.from / fps)).arg(clock(segment.to / fps));
    }
    text = lines.join(QStringLiteral("; "));

    cache = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/ld-analyse-stack-audio-XXXXXX.raw"));
    if (!cache->open()) {
        error = tr("The audio of the stack cannot be written: %1").arg(cache->errorString());
        cache.reset();
        plan.clear();
        return false;
    }
    return true;
}

bool StackAudio::start(const QVector<StackAudioSpan> &given, qint32 timelineFrames, double framesPerSecond, Kind kind, const EfmOf &efmOf, QString &error)
{
    if (!prepare(given, timelineFrames, framesPerSecond, kind, efmOf, error)) return false;
    begin();
    return true;
}

void StackAudio::begin()
{
    if (started || !cache || plan.isEmpty()) return;
    started = true;

    // The EFM to be decoded, in the order it is heard
    QVector<qint32> asked;
    for (const Segment &segment: plan) {
        if (segment.source < 0 || picks[segment.source].efm.isEmpty() || asked.contains(segment.source)) continue;
        asked.append(segment.source);
        emit efmNeeded(segment.source, picks[segment.source].efm);
    }

    // What is done in the thread (it owns the file while it runs)
    const QVector<Segment> segments = plan;
    QTemporaryFile *file = cache.get();
    running = true;
    worker = QThread::create([this, segments, file]() {
        const double outRate = sampleRate();
        bool ok = true;
        QString message;
        for (const Segment &segment: segments) {
            if (cancel) break;
            const qint64 outFrom = std::llround(segment.from * outRate / fps);
            const qint64 outTo = std::llround(segment.to * outRate / fps);
            const qint64 length = outTo - outFrom;
            AudioReader reader;
            double rate = outRate;
            qint64 sourceFrom = 0;
            bool readable = false;
            if (segment.source >= 0) {
                const Pick &chosen = picks[segment.source];
                QString readError;
                AudioTrackInfo track = chosen.track;
                bool opened = false;
                double startSeconds = 0.0;
                if (!chosen.efm.isEmpty()) {
                    // The EFM is decoded by the window: this waits for it (the audio is silent where it could not be)
                    QString pcm;
                    {
                        QMutexLocker locker(&decodedMutex);
                        while (!cancel && decoded.find(segment.source) == decoded.end()) decodedWake.wait(&decodedMutex, 200);
                        const auto found = decoded.find(segment.source);
                        if (found != decoded.end()) pcm = found->second;
                    }
                    if (cancel) break;
                    if (!pcm.isEmpty() && reader.openGrowing(pcm, outRate, readError)) {
                        reader.refresh();
                        opened = true;
                    }
                } else {
                    opened = reader.open(track, readError);
                    startSeconds = track.startSeconds;
                }
                if (opened) {
                    readable = true;
                    rate = reader.sampleRate() > 0 ? reader.sampleRate() : outRate;
                    // (where the source begins on the timeline is where its audio begins)
                    const double seconds = startSeconds + (segment.from - spans[segment.source].start) / fps;
                    sourceFrom = std::llround(seconds * rate);
                } else if (!readError.isEmpty()) {
                    message = readError;
                }
            }
            for (qint64 done = 0; done < length && !cancel; done += kChunkPairs) {
                const qint64 count = std::min(kChunkPairs, length - done);
                QVector<qint16> pairs(static_cast<qsizetype>(count * 2), 0);
                if (readable) {
                    if (rate == outRate) {
                        const QVector<qint16> got = reader.read(sourceFrom + done, count);
                        std::copy(got.begin(), got.end(), pairs.begin());
                    } else {
                        // (another rate: each pair is the nearest of the source)
                        const qint64 first = sourceFrom + static_cast<qint64>(std::floor(done * rate / outRate));
                        const qint64 need = static_cast<qint64>(std::ceil(count * rate / outRate)) + 2;
                        const QVector<qint16> got = reader.read(first, need);
                        for (qint64 k = 0; k < count; k++) {
                            const qint64 at = static_cast<qint64>(std::floor((done + k) * rate / outRate)) - static_cast<qint64>(std::floor(done * rate / outRate));
                            if (at * 2 + 1 < got.size()) {
                                pairs[static_cast<qsizetype>(k * 2)] = got[static_cast<qsizetype>(at * 2)];
                                pairs[static_cast<qsizetype>(k * 2 + 1)] = got[static_cast<qsizetype>(at * 2 + 1)];
                            }
                        }
                    }
                    // A short fade where the audio begins and where it ends (a source to another, or to silence), so that it does not click
                    if (done < kFadePairs || length - done <= kFadePairs + kChunkPairs) {
                        for (qint64 k = 0; k < count; k++) {
                            const qint64 index = done + k;
                            const double gain = std::min({ 1.0, static_cast<double>(index) / kFadePairs, static_cast<double>(length - index) / kFadePairs });
                            if (gain >= 1.0) continue;
                            pairs[static_cast<qsizetype>(k * 2)] = static_cast<qint16>(pairs[static_cast<qsizetype>(k * 2)] * gain);
                            pairs[static_cast<qsizetype>(k * 2 + 1)] = static_cast<qint16>(pairs[static_cast<qsizetype>(k * 2 + 1)] * gain);
                        }
                    }
                }
                if (file->write(reinterpret_cast<const char *>(pairs.constData()), pairs.size() * static_cast<qint64>(sizeof(qint16))) < 0) {
                    ok = false;
                    message = file->errorString();
                    cancel = true;
                    break;
                }
                file->flush();
                QMetaObject::invokeMethod(this, [this]() { emit grew(); }, Qt::QueuedConnection);
            }
        }
        running = false;
        const bool success = ok && !cancel;
        QMetaObject::invokeMethod(this, [this, success, message]() { emit finished(success, message); }, Qt::QueuedConnection);
    });
    worker->start(QThread::LowPriority);
}

bool StackAudio::needsDecoding() const
{
    for (const Segment &segment: plan) {
        if (segment.source >= 0 && !picks[segment.source].efm.isEmpty()) return true;
    }
    return false;
}
