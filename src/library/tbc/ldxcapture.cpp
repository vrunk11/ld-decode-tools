/******************************************************************************
 * ldxcapture.cpp
 * ld-decode-tools TBC library - a capture kept in an LDX file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxcapture.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QObject>
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#include "ldxcodec.h"
#include "cvbs.h"
#endif

namespace {

const char *SessionName = "session.stack";

#ifndef LDDECODE_WITH_LDX
QString unsupported()
{
    return QObject::tr("This program was built without support for LDX files");
}
#endif

} // namespace

struct LdxCapture::Private
{
#ifdef LDDECODE_WITH_LDX
    Ldx::Reader reader;
    qint32 trackId = 0;
    qint64 fields = 0;
    QJsonObject trackParameters;

    // A cvbs track holds frames, which are read as the two fields of the TBC they map to
    bool cvbs = false;
    Cvbs::Geometry geometry;
    std::unique_ptr<Cvbs::SampleConverter> converter;
    quint16 padValue = 0;
    qint64 cachedFrame = -1;
    QVector<quint16> cachedFields[2];

    // The audio: a PCM track, the first sample of each of its blocks, and the block read last
    quint32 audioTrack = 0;
    double audioRate = 0;
    std::vector<qint64> audioStarts;
    qint64 audioTotal = 0;
    qint64 cachedBlock = -1;
    QByteArray cachedData;
#endif
};

LdxCapture::LdxCapture() : d(new Private) {}
LdxCapture::~LdxCapture() = default;

bool LdxCapture::isLdxName(const QString &name)
{
    QString path;
    qint32 track;
    split(name, path, track);
    return path.endsWith(".ldx", Qt::CaseInsensitive);
}

void LdxCapture::split(const QString &name, QString &path, qint32 &track)
{
    path = name;
    track = 0;
    const qint32 hash = name.lastIndexOf('#');
    if (hash > 0) {
        bool ok = false;
        const qint32 id = name.mid(hash + 1).toInt(&ok);
        if (ok && id > 0 && name.left(hash).endsWith(".ldx", Qt::CaseInsensitive)) {
            path = name.left(hash);
            track = id;
        }
    }
}

QString LdxCapture::trackName(const QString &path, qint32 track)
{
    return track > 0 ? QString("%1#%2").arg(path).arg(track) : path;
}

bool LdxCapture::isCvbs(const QString &name)
{
#ifdef LDDECODE_WITH_LDX
    QString path;
    qint32 track;
    split(name, path, track);
    Ldx::Reader reader;
    QString error;
    if (!reader.open(path, error) || reader.profile() < 1) return false;
    const QVector<Ldx::Track> tracks = reader.tracks();
    for (const Ldx::Track &candidate: tracks) {
        if (track > 0 ? candidate.number == static_cast<quint32>(track) : (candidate.kind == Ldx::Kind::Tbc || candidate.kind == Ldx::Kind::Cvbs)) {
            return candidate.kind == Ldx::Kind::Cvbs;
        }
    }
    return false;
#else
    Q_UNUSED(name);
    return false;
#endif
}

bool LdxCapture::listCaptures(const QString &path, QVector<qint32> &tracks, QStringList &names, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    Ldx::Reader reader;
    if (!reader.open(path, error)) return false;
    if (reader.profile() < 1) {
        error = QObject::tr("%1 is not an LDX file").arg(path);
        return false;
    }
    for (const Ldx::Track &track: reader.tracks()) {
        if (track.kind != Ldx::Kind::Tbc && track.kind != Ldx::Kind::Cvbs) continue;
        tracks.append(static_cast<qint32>(track.number));
        names.append(track.name);
    }
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(tracks);
    Q_UNUSED(names);
    error = unsupported();
    return false;
#endif
}

bool LdxCapture::hasSession(const QString &path)
{
#ifdef LDDECODE_WITH_LDX
    Ldx::Reader reader;
    QString error;
    QByteArray session;
    return reader.open(path, error) && reader.profile() > 0 && reader.attachment(SessionName, session);
#else
    Q_UNUSED(path);
    return false;
#endif
}

bool LdxCapture::readSession(const QString &path, QByteArray &session, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    Ldx::Reader reader;
    if (!reader.open(path, error)) return false;
    if (reader.profile() < 1) {
        error = QObject::tr("%1 is not an LDX file").arg(path);
        return false;
    }
    if (!reader.attachment(SessionName, session)) {
        error = QObject::tr("%1 does not keep a stacking session").arg(path);
        return false;
    }
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(session);
    error = unsupported();
    return false;
#endif
}

bool LdxCapture::open(const QString &name, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    QString path;
    qint32 track;
    split(name, path, track);
    if (!d->reader.open(path, error)) return false;
    if (d->reader.profile() < 1) {
        error = QObject::tr("%1 is not an LDX file").arg(path);
        return false;
    }

    // The track named, or the first TBC track
    const Ldx::Track *chosen = nullptr;
    const QVector<Ldx::Track> tracks = d->reader.tracks();
    for (const Ldx::Track &candidate: tracks) {
        if (track > 0 ? candidate.number == static_cast<quint32>(track) : (candidate.kind == Ldx::Kind::Tbc || candidate.kind == Ldx::Kind::Cvbs)) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        error = track > 0 ? QObject::tr("%1 has no track %2").arg(path).arg(track) : QObject::tr("%1 has no TBC track").arg(path);
        return false;
    }
    if (chosen->kind != Ldx::Kind::Tbc && chosen->kind != Ldx::Kind::Cvbs) {
        error = QObject::tr("Track %1 of %2 is %3, and only TBC and CVBS tracks are read as video")
                    .arg(chosen->number).arg(path, Ldx::kindName(chosen->kind));
        return false;
    }
    if (chosen->codec == Ldx::Codec::Unknown) {
        error = QObject::tr("Track %1 of %2 is coded in a way this program does not know").arg(chosen->number).arg(path);
        return false;
    }
    if (chosen->codec == Ldx::Codec::Flac && !Ldx::flacAvailable()) {
        error = QObject::tr("Track %1 of %2 is FLAC, and this program was built without ffmpeg, which reads it").arg(chosen->number).arg(path);
        return false;
    }
    if (chosen->codec == Ldx::Codec::Ffv1 && !Ldx::ffv1Available()) {
        error = QObject::tr("Track %1 of %2 is FFV1, and this program was built without ffmpeg, which reads it").arg(chosen->number).arg(path);
        return false;
    }
    if (!d->reader.buildIndex(error)) return false;

    d->trackId = static_cast<qint32>(chosen->number);
    d->trackParameters = chosen->parameters;
    d->fields = d->reader.blockCount(chosen->number);

    if (chosen->kind == Ldx::Kind::Cvbs) {
        // Frames: two fields each, of the length of a TBC field
        const QString system = chosen->parameters.value("system").toString();
        const VideoSystem videoSystem = system == "PAL" ? PAL : (system == "NTSC" ? NTSC : PAL_M);
        d->geometry = Cvbs::geometry(videoSystem);
        if (static_cast<qint64>(chosen->parameters.value("frameSamples").toDouble()) != d->geometry.frameSamples) {
            error = QObject::tr("The frames of track %1 of %2 are not those of %3 CVBS").arg(chosen->number).arg(path, system);
            return false;
        }
        d->converter = std::make_unique<Cvbs::SampleConverter>(Cvbs::SampleEncoding::U10_4FSC, videoSystem);
        d->padValue = static_cast<quint16>(Cvbs::standardLevels(videoSystem).blanking * 64);
        d->cvbs = true;
        d->fields *= 2;
    }
    return true;
#else
    Q_UNUSED(name);
    error = unsupported();
    return false;
#endif
}

qint32 LdxCapture::track() const
{
#ifdef LDDECODE_WITH_LDX
    return d->trackId;
#else
    return 0;
#endif
}

qint32 LdxCapture::numberOfFields() const
{
#ifdef LDDECODE_WITH_LDX
    return static_cast<qint32>(d->fields);
#else
    return 0;
#endif
}

QJsonObject LdxCapture::parameters() const
{
#ifdef LDDECODE_WITH_LDX
    return d->trackParameters;
#else
    return QJsonObject();
#endif
}

QVector<quint16> LdxCapture::getField(qint32 fieldNumber)
{
    QVector<quint16> field;
#ifdef LDDECODE_WITH_LDX
    if (d->cvbs) {
        // The frame the field is of, split into its two fields (which are kept for the next one asked for)
        if (fieldNumber < 1 || fieldNumber > d->fields) return field;
        const qint64 frame = (fieldNumber - 1) / 2;
        if (frame != d->cachedFrame) {
            Ldx::Block block;
            if (!d->reader.readBlock(static_cast<quint32>(d->trackId), frame, block) || block.data.size() != d->geometry.frameSamples * 2) return field;
            d->cachedFields[0].resize(d->geometry.fieldLength());
            d->cachedFields[1].resize(d->geometry.fieldLength());
            // (the samples are 10-bit values in words, which the converter of the default encoding takes as they are)
            Cvbs::frameToFields(reinterpret_cast<const qint16 *>(block.data.constData()), d->geometry, *d->converter, d->padValue,
                                d->cachedFields[0].data(), d->cachedFields[1].data());
            d->cachedFrame = frame;
        }
        return d->cachedFields[(fieldNumber - 1) % 2];
    }
    Ldx::Block block;
    if (fieldNumber < 1 || fieldNumber > d->fields || !d->reader.readBlock(static_cast<quint32>(d->trackId), fieldNumber - 1, block)) return field;
    field.resize(block.data.size() / 2);
    std::memcpy(field.data(), block.data.constData(), static_cast<size_t>(field.size()) * 2);
#else
    Q_UNUSED(fieldNumber);
#endif
    return field;
}

#ifdef LDDECODE_WITH_LDX
// The metadata track that is about this capture or, failing that, the only one there is (0 if none)
quint32 LdxCapture::metadataTrackNumber() const
{
    const Ldx::Track *chosen = nullptr;
    int count = 0;
    const QVector<Ldx::Track> tracks = d->reader.tracks();
    for (const Ldx::Track &candidate: tracks) {
        if (candidate.kind != Ldx::Kind::Metadata) continue;
        count++;
        if (candidate.parameters.value("track").toInt(0) == d->trackId) return candidate.number;
        if (!chosen && !candidate.parameters.contains("track")) chosen = &candidate;
    }
    return chosen && count >= 1 ? chosen->number : 0;
}
#endif

bool LdxCapture::readMetadata(QByteArray &json, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    const quint32 metadataTrack = metadataTrackNumber();
    if (metadataTrack == 0) {
        error = QObject::tr("Track %1 has no metadata").arg(d->trackId);
        return false;
    }
    // The last block is the current one: what is added to the file supersedes what was there
    Ldx::Block block;
    if (d->reader.blockCount(metadataTrack) < 1 || !d->reader.readBlock(metadataTrack, d->reader.blockCount(metadataTrack) - 1, block)) {
        error = QObject::tr("The metadata of track %1 could not be read").arg(d->trackId);
        return false;
    }
    json = block.data;
    return true;
#else
    Q_UNUSED(json);
    error = unsupported();
    return false;
#endif
}

bool LdxCapture::writeMetadata(const QString &name, const QByteArray &json, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    QString path;
    quint32 metadataTrack = 0;
    qint64 after = 0;
    qint32 track = 0;
    {
        LdxCapture capture;
        if (!capture.open(name, error)) return false;
        metadataTrack = capture.metadataTrackNumber();
        if (metadataTrack == 0) {
            error = QObject::tr("%1 has no metadata track to keep the metadata in").arg(name);
            return false;
        }
        after = capture.d->reader.lastTimeNs() + 1000;
        track = capture.track();
        qint32 ignored;
        split(name, path, ignored);
    }

    Ldx::Writer writer;
    if (!writer.openAppend(path, error)) return false;
    QJsonObject entry;
    entry["tool"] = QCoreApplication::applicationName().isEmpty() ? QStringLiteral("a tool of ld-decode-tools") : QCoreApplication::applicationName();
    entry["command"] = "metadata";
    entry["detail"] = QString("the metadata of track %1").arg(track);
    entry["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    writer.addHistory(entry);
    if (!writer.writeBlock(metadataTrack, after, json)) {
        error = writer.lastError().isEmpty() ? QObject::tr("Could not add the metadata to %1").arg(path) : writer.lastError();
        return false;
    }
    return writer.finish(error);
#else
    Q_UNUSED(name);
    Q_UNUSED(json);
    error = unsupported();
    return false;
#endif
}

bool LdxCapture::openAudio(const QString &name, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    if (!open(name, error)) return false;

    // The PCM track about the capture or, failing that, the first one
    const Ldx::Track *chosen = nullptr;
    const QVector<Ldx::Track> tracks = d->reader.tracks();
    for (const Ldx::Track &candidate: tracks) {
        if (candidate.kind != Ldx::Kind::Pcm) continue;
        if (candidate.parameters.value("track").toInt(0) == d->trackId) { chosen = &candidate; break; }
        if (!chosen) chosen = &candidate;
    }
    if (!chosen) {
        error = QObject::tr("%1 has no analogue audio").arg(name);
        return false;
    }
    if (chosen->parameters.value("channels").toInt(2) != 2) {
        error = QObject::tr("The audio of %1 is not stereo").arg(name);
        return false;
    }
    d->audioRate = chosen->parameters.value("sampleRate").toDouble(44100);
    d->audioTrack = chosen->number;

    // Where each block starts, from its time (the blocks are of whole samples, one after the other)
    const qint64 blocks = d->reader.blockCount(d->audioTrack);
    if (blocks < 1) {
        error = QObject::tr("The audio of %1 is empty").arg(name);
        return false;
    }
    d->audioStarts.assign(static_cast<size_t>(blocks), 0);
    for (qint64 i = 0; i < blocks; i++) {
        d->audioStarts[static_cast<size_t>(i)] = static_cast<qint64>(d->reader.blockTimeNs(d->audioTrack, i) * d->audioRate / 1.0e9 + 0.5);
    }
    Ldx::Block last;
    if (!d->reader.readBlock(d->audioTrack, blocks - 1, last)) {
        error = QObject::tr("Could not read the audio of %1").arg(name);
        return false;
    }
    d->audioTotal = d->audioStarts.back() + last.data.size() / 4;
    d->cachedBlock = blocks - 1;
    d->cachedData = last.data;
    return true;
#else
    Q_UNUSED(name);
    error = unsupported();
    return false;
#endif
}

qint64 LdxCapture::audioSamples() const
{
#ifdef LDDECODE_WITH_LDX
    return d->audioTotal;
#else
    return 0;
#endif
}

double LdxCapture::audioSampleRate() const
{
#ifdef LDDECODE_WITH_LDX
    return d->audioRate;
#else
    return 0;
#endif
}

QVector<qint16> LdxCapture::getAudio(qint64 startSample, qint64 numberOfSamples)
{
    QVector<qint16> samples;
#ifdef LDDECODE_WITH_LDX
    if (d->audioStarts.empty() || startSample < 0 || numberOfSamples < 1) return samples;
    samples.reserve(static_cast<qint32>(numberOfSamples * 2));

    // The block that holds the first sample
    qint64 block = std::upper_bound(d->audioStarts.begin(), d->audioStarts.end(), startSample) - d->audioStarts.begin() - 1;
    qint64 position = startSample;
    const qint64 end = startSample + numberOfSamples;
    while (position < end && block < static_cast<qint64>(d->audioStarts.size())) {
        if (d->cachedBlock != block) {
            Ldx::Block data;
            if (!d->reader.readBlock(d->audioTrack, block, data)) break;
            d->cachedBlock = block;
            d->cachedData = data.data;
        }
        const qint64 first = d->audioStarts[static_cast<size_t>(block)];
        const qint64 inBlock = d->cachedData.size() / 4;
        const qint64 from = position - first;
        const qint64 count = qMin(end - position, inBlock - from);
        if (count <= 0) { block++; continue; }
        const qint32 old = samples.size();
        samples.resize(old + static_cast<qint32>(count * 2));
        std::memcpy(samples.data() + old, d->cachedData.constData() + from * 4, static_cast<size_t>(count * 4));
        position += count;
        block++;
    }
#else
    Q_UNUSED(startSample);
    Q_UNUSED(numberOfSamples);
#endif
    return samples;
}
