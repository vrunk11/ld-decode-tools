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

#include <QObject>
#include <cstring>

#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
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
        if (track > 0 ? candidate.number == static_cast<quint32>(track) : candidate.kind == Ldx::Kind::Tbc) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        error = track > 0 ? QObject::tr("%1 has no track %2").arg(path).arg(track) : QObject::tr("%1 has no TBC track").arg(path);
        return false;
    }
    if (chosen->kind != Ldx::Kind::Tbc) {
        error = QObject::tr("Track %1 of %2 is %3, and only TBC tracks are read for now")
                    .arg(chosen->number).arg(path, Ldx::kindName(chosen->kind));
        return false;
    }
    if (chosen->codec != Ldx::Codec::Raw) {
        error = QObject::tr("Track %1 of %2 is %3 coded, which this build does not read yet")
                    .arg(chosen->number).arg(path, Ldx::codecName(chosen->codec));
        return false;
    }
    if (!d->reader.buildIndex(error)) return false;

    d->trackId = static_cast<qint32>(chosen->number);
    d->trackParameters = chosen->parameters;
    d->fields = d->reader.blockCount(chosen->number);
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
    Ldx::Block block;
    if (fieldNumber < 1 || fieldNumber > d->fields || !d->reader.readBlock(static_cast<quint32>(d->trackId), fieldNumber - 1, block)) return field;
    field.resize(block.data.size() / 2);
    std::memcpy(field.data(), block.data.constData(), static_cast<size_t>(field.size()) * 2);
#else
    Q_UNUSED(fieldNumber);
#endif
    return field;
}

bool LdxCapture::readMetadata(QByteArray &json, QString &error)
{
#ifdef LDDECODE_WITH_LDX
    // The metadata track that is about this track or, failing that, the only one there is
    const Ldx::Track *chosen = nullptr;
    int count = 0;
    const QVector<Ldx::Track> tracks = d->reader.tracks();
    for (const Ldx::Track &candidate: tracks) {
        if (candidate.kind != Ldx::Kind::Metadata) continue;
        count++;
        if (candidate.parameters.value("track").toInt(0) == d->trackId) { chosen = &candidate; break; }
        if (!chosen && !candidate.parameters.contains("track")) chosen = &candidate;
    }
    if (!chosen || (count > 1 && chosen->parameters.value("track").toInt(0) != d->trackId && chosen->parameters.contains("track"))) {
        error = QObject::tr("Track %1 has no metadata").arg(d->trackId);
        return false;
    }
    Ldx::Block block;
    if (d->reader.blockCount(chosen->number) < 1 || !d->reader.readBlock(chosen->number, 0, block)) {
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
