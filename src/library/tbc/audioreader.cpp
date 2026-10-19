/******************************************************************************
 * audioreader.cpp
 * ld-decode-tools TBC library - the audio of a capture, as stereo samples to play
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "audioreader.h"

#include "audioring.h"

#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <cstring>

#include "cvbs.h"
#include "cvbsaudio.h"
#include "ldxcapture.h"

struct AudioReader::Private {
    enum class Kind { None, Ldx, Pcm, Wav, Memory, Ring } kind = Kind::None;
    std::shared_ptr<AudioRing> ring;
    LdxCapture ldx;
    QFile pcm;
    CvbsAudio::WavReader wav;
    QVector<qint16> memory;
    qint64 total = 0;
    double rate = 44100;
    QMutex lock;            // (the audio is read by the player and by the thread that makes the waveform)
    bool growing = false;
};

AudioReader::AudioReader() : d(new Private) {}
AudioReader::~AudioReader() = default;

QVector<AudioTrackInfo> AudioReader::tracksOf(const QString &videoName)
{
    QVector<AudioTrackInfo> found;
    if (videoName.isEmpty()) return found;
    if (LdxCapture::isLdxName(videoName)) {
        for (const LdxCapture::AudioTrack &track: LdxCapture::audioTracks(videoName)) {
            AudioTrackInfo info;
            info.trackId = track.id;
            info.source = track.source;
            info.where = videoName;
            info.sampleRate = track.sampleRate;
            info.bits = track.bits;
            const QString what = track.source == "efm" ? QObject::tr("decoded from EFM") : (track.source == "analogue" ? QObject::tr("analogue") : track.source);
            info.label = QString("%1: %2 (%3, %4 kHz)").arg(track.id).arg(track.name, what).arg(track.sampleRate / 1000.0, 0, 'f', 1);
            found.append(info);
        }
        return found;
    }

    AudioTrackInfo info;
    info.where = videoName;
    info.source = "analogue";
    if (Cvbs::isDataFileName(videoName)) {
        const QString wav = CvbsAudio::wavFileName(Cvbs::baseName(videoName), 0);
        CvbsAudio::WavInfo wavInfo;
        if (QFileInfo::exists(wav) && CvbsAudio::readWavInfo(wav, wavInfo)) {
            info.where = wav;
            info.sampleRate = wavInfo.sampleRate;
            info.bits = wavInfo.bits;
            info.label = QObject::tr("%1 (analogue, %2 kHz)").arg(QFileInfo(wav).fileName()).arg(info.sampleRate / 1000.0, 0, 'f', 1);
            found.append(info);
        }
        return found;
    }
    const QFileInfo video(videoName);
    const QString pcm = video.absolutePath() + "/" + video.completeBaseName() + ".pcm";
    if (QFileInfo::exists(pcm)) {
        info.where = pcm;
        info.label = QObject::tr("%1 (analogue, 44.1 kHz)").arg(QFileInfo(pcm).fileName());
        found.append(info);
    }
    return found;
}

bool AudioReader::open(const AudioTrackInfo &track, QString &error)
{
    d->growing = false;
    d->kind = Private::Kind::None;
    d->total = 0;
    if (LdxCapture::isLdxName(track.where)) {
        if (!d->ldx.openAudio(track.where, error, track.bits == 24, track.trackId)) return false;
        d->kind = Private::Kind::Ldx;
        d->total = d->ldx.audioSamples();
        d->rate = d->ldx.audioSampleRate();
        return true;
    }
    if (track.where.endsWith(".wav", Qt::CaseInsensitive)) {
        if (!d->wav.open(track.where)) {
            error = QObject::tr("Could not read %1").arg(track.where);
            return false;
        }
        d->kind = Private::Kind::Wav;
        d->total = d->wav.info().numberOfSamples;
        d->rate = d->wav.info().sampleRate;
        return true;
    }
    d->pcm.setFileName(track.where);
    if (!d->pcm.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Could not open %1").arg(track.where);
        return false;
    }
    d->kind = Private::Kind::Pcm;
    d->total = d->pcm.size() / 4;
    d->rate = track.sampleRate;
    return true;
}

bool AudioReader::openGrowing(const QString &fileName, double sampleRate, QString &error)
{
    QMutexLocker locker(&d->lock);
    d->pcm.setFileName(fileName);
    if (!d->pcm.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Could not open %1").arg(fileName);
        return false;
    }
    d->kind = Private::Kind::Pcm;
    d->growing = true;
    d->total = d->pcm.size() / 4;
    d->rate = sampleRate;
    return true;
}

void AudioReader::refresh()
{
    QMutexLocker locker(&d->lock);
    if (d->growing) d->total = QFileInfo(d->pcm.fileName()).size() / 4;
}

void AudioReader::openMemory(const QVector<qint16> &samples, double sampleRate)
{
    d->growing = false;
    d->memory = samples;
    d->kind = Private::Kind::Memory;
    d->total = samples.size() / 2;
    d->rate = sampleRate;
}

bool AudioReader::isGrowing() const { return d->growing; }

void AudioReader::openRing(std::shared_ptr<AudioRing> audioRing)
{
    QMutexLocker locker(&d->lock);
    d->growing = true;
    d->kind = Private::Kind::Ring;
    d->rate = audioRing->rate();
    d->ring = std::move(audioRing);
}

qint64 AudioReader::firstSample() const
{
    return d->kind == Private::Kind::Ring && d->ring ? d->ring->firstSample() : 0;
}

qint64 AudioReader::samples() const { return d->kind == Private::Kind::Ring && d->ring ? d->ring->endSample() : d->total; }
double AudioReader::sampleRate() const { return d->rate; }

QVector<qint16> AudioReader::read(qint64 start, qint64 count)
{
    QMutexLocker locker(&d->lock);
    // (an arriving ring: from the oldest held, if what is asked for was let go)
    if (d->kind == Private::Kind::Ring && d->ring) return d->ring->read(start, count);
    if (d->growing && start + count > d->total) d->total = QFileInfo(d->pcm.fileName()).size() / 4;       // (it may have grown since)
    QVector<qint16> out;
    if (start < 0 || count < 1 || start >= d->total) return out;
    count = qMin(count, d->total - start);
    switch (d->kind) {
    case Private::Kind::Ldx:
        if (d->ldx.audioBytesPerSample() == 6) {
            // 24 bits: the top 16 of each
            const QByteArray bytes = d->ldx.getAudioBytes(start, count);
            out.resize(static_cast<qint32>(bytes.size() / 3));
            for (qint32 i = 0; i < out.size(); i++) {
                const char *p = bytes.constData() + i * 3;
                out[i] = static_cast<qint16>((static_cast<quint8>(p[1])) | (static_cast<quint8>(p[2]) << 8));
            }
        } else {
            out = d->ldx.getAudio(start, count);
        }
        break;
    case Private::Kind::Pcm: {
        d->pcm.seek(start * 4);
        const QByteArray bytes = d->pcm.read(count * 4);
        out.resize(static_cast<qint32>(bytes.size() / 2));
        std::memcpy(out.data(), bytes.constData(), static_cast<size_t>(out.size()) * 2);
        break;
    }
    case Private::Kind::Wav: {
        const QByteArray bytes = d->wav.read(start, count);       // 24-bit, 6 bytes a pair
        out.resize(static_cast<qint32>(bytes.size() / 3));
        for (qint32 i = 0; i < out.size(); i++) {
            const char *p = bytes.constData() + i * 3;
            out[i] = static_cast<qint16>((static_cast<quint8>(p[1])) | (static_cast<quint8>(p[2]) << 8));
        }
        break;
    }
    case Private::Kind::Memory:
        out = d->memory.mid(static_cast<qint32>(start * 2), static_cast<qint32>(count * 2));
        break;
    case Private::Kind::None:
        break;
    }
    return out;
}
