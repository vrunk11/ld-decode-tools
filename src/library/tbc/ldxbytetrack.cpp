/******************************************************************************
 * ldxbytetrack.cpp
 * ld-decode-tools TBC library - the EFM or AC3 data of an LDX file as a stream of bytes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxbytetrack.h"
#include "ldxcapture.h"

#include <QObject>

#include <cstring>

#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

#ifdef LDDECODE_WITH_LDX

struct LdxByteTrack::Private {
    Ldx::Reader reader;
    quint32 track = 0;
    bool stream = false;
    qint64 total = 0;           // the size of the data (a file; unknown, -1, in a stream)
    qint64 position = 0;        // bytes given so far
    qint64 nextBlock = 0;       // the next block to read (a file)
    QByteArray current;         // what is left of the block being read
    bool finished = false;
};

namespace {

// The track of a kind that is about the capture a name gives (a track id after # chooses it)
bool chooseTrack(const QVector<Ldx::Track> &tracks, qint32 asked, Ldx::Kind kind, quint32 &chosen, QString &error, const QString &name)
{
    if (asked > 0) {
        for (const Ldx::Track &track: tracks) {
            if (track.number == static_cast<quint32>(asked)) {
                if (track.kind != kind) {
                    error = QObject::tr("The track %1 of %2 is not a %3 track").arg(asked).arg(name, Ldx::kindName(kind));
                    return false;
                }
                chosen = track.number;
                return true;
            }
        }
        error = QObject::tr("%1 has no track %2").arg(name).arg(asked);
        return false;
    }
    quint32 video = 0;
    for (const Ldx::Track &track: tracks) {
        if (video == 0 && (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs)) video = track.number;
    }
    quint32 first = 0;
    for (const Ldx::Track &track: tracks) {
        if (track.kind != kind) continue;
        if (track.parameters.value("track").toInt(0) == static_cast<int>(video)) { chosen = track.number; return true; }
        if (first == 0) first = track.number;
    }
    if (first == 0) {
        error = QObject::tr("%1 has no %2 track").arg(name, Ldx::kindName(kind));
        return false;
    }
    chosen = first;
    return true;
}

Ldx::Kind kindOf(LdxByteTrack::Kind kind) { return kind == LdxByteTrack::Kind::Efm ? Ldx::Kind::Efm : Ldx::Kind::Ac3; }

} // namespace

LdxByteTrack::LdxByteTrack(QObject *parent) : QIODevice(parent), d(new Private) {}
LdxByteTrack::~LdxByteTrack() = default;

bool LdxByteTrack::isLdxMagic(const QByteArray &bytes)
{
    static const QByteArray magic("\x1A\x45\xDF\xA3", 4);
    return bytes.size() >= 4 && bytes.left(4) == magic;
}

bool LdxByteTrack::openFile(const QString &name, Kind kind, QString &error)
{
    QString path;
    qint32 asked;
    LdxCapture::split(name, path, asked);
    if (!d->reader.open(path, error) || !d->reader.buildIndex(error)) return false;
    if (d->reader.profile() < 1) {
        error = QObject::tr("%1 is not an LDX file").arg(path);
        return false;
    }
    if (!chooseTrack(d->reader.tracks(), asked, kindOf(kind), d->track, error, name)) return false;
    d->stream = false;

    // The size is the sum of the blocks (only the blocks of the track are read)
    const qint64 blocks = d->reader.blockCount(d->track);
    d->total = 0;
    for (qint64 i = 0; i < blocks; i++) {
        Ldx::Block block;
        if (!d->reader.readBlock(d->track, i, block)) {
            error = QObject::tr("Could not read the data of the track %1 of %2").arg(d->track).arg(path);
            return false;
        }
        d->total += block.data.size();
    }
    return QIODevice::open(QIODevice::ReadOnly);
}

bool LdxByteTrack::openStdin(const QByteArray &alreadyRead, Kind kind, QString &error)
{
    if (!d->reader.openStream(stdin, error, alreadyRead)) return false;
    if (d->reader.profile() < 1) {
        error = QObject::tr("The standard input is Matroska, but not an LDX stream");
        return false;
    }
    if (!chooseTrack(d->reader.tracks(), 0, kindOf(kind), d->track, error, QObject::tr("the stream"))) return false;
    d->stream = true;
    d->total = -1;
    return QIODevice::open(QIODevice::ReadOnly);
}

bool LdxByteTrack::isSequential() const { return d->stream; }
qint64 LdxByteTrack::size() const { return d->total; }

qint64 LdxByteTrack::bytesAvailable() const
{
    // (in a stream nobody knows how much is to come: what is held, and 1 while the stream goes on, so that a loop that
    // reads while bytes are available reads until the end)
    if (d->stream) return d->current.size() + (d->finished ? 0 : 1) + QIODevice::bytesAvailable();
    return (d->total - d->position) + QIODevice::bytesAvailable();
}

void LdxByteTrack::close()
{
    QIODevice::close();
}

qint64 LdxByteTrack::readData(char *data, qint64 maxSize)
{
    qint64 done = 0;
    while (done < maxSize) {
        if (d->current.isEmpty()) {
            Ldx::Block block;
            if (d->stream) {
                bool got = false;
                while (d->reader.nextBlock(block)) {
                    if (block.track == d->track) { got = true; break; }
                }
                if (!got) { d->finished = true; break; }
            } else {
                if (d->nextBlock >= d->reader.blockCount(d->track)) break;
                if (!d->reader.readBlock(d->track, d->nextBlock++, block)) break;
            }
            d->current = block.data;
            continue;
        }
        const qint64 count = qMin<qint64>(maxSize - done, d->current.size());
        std::memcpy(data + done, d->current.constData(), static_cast<size_t>(count));
        d->current.remove(0, static_cast<qsizetype>(count));
        done += count;
        d->position += count;
    }
    return done;
}

#else // no LDX

struct LdxByteTrack::Private {};
LdxByteTrack::LdxByteTrack(QObject *parent) : QIODevice(parent), d(new Private) {}
LdxByteTrack::~LdxByteTrack() = default;
bool LdxByteTrack::isLdxMagic(const QByteArray &) { return false; }
bool LdxByteTrack::openFile(const QString &, Kind, QString &error) { error = QObject::tr("This program was built without support for LDX files"); return false; }
bool LdxByteTrack::openStdin(const QByteArray &, Kind, QString &error) { error = QObject::tr("This program was built without support for LDX files"); return false; }
bool LdxByteTrack::isSequential() const { return false; }
qint64 LdxByteTrack::size() const { return 0; }
qint64 LdxByteTrack::bytesAvailable() const { return 0; }
void LdxByteTrack::close() { QIODevice::close(); }
qint64 LdxByteTrack::readData(char *, qint64) { return -1; }

#endif
