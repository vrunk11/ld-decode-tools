/******************************************************************************
 * ldx.h
 * ld-decode-tools LDX library - reading and writing the LDX container
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDX_H
#define LDX_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <memory>

// The LDX container (docs/Specifications/container-format.md): a Matroska file that follows a profile. Each
// stream is a track, and the data are blocks stamped with a time in nanoseconds. This is the first slice of it:
// raw tracks, and the history, written and read from a file.
namespace Ldx {

constexpr int ProfileVersion = 1;

enum class Kind { Tbc, Cvbs, Rf, Pcm, Efm, Ac3, Metadata, Annotations, Unknown };
enum class Codec { Raw, Flac, Ffv1, Unknown };

QString kindName(Kind kind);
Kind kindFromName(const QString &name);
QString codecName(Codec codec);
Codec codecFromName(const QString &name);

struct Track {
    Kind kind = Kind::Unknown;
    Codec codec = Codec::Raw;
    int depth = 16;                 // the useful bits of a sample (16, 10, 8) or 0 for text
    QString name;
    QJsonObject parameters;         // what the tools know of the stream: system, sample rate, field size...

    // Given when the track is added (writing) or read (reading)
    quint32 number = 0;             // the number of the track in the file, from 1
    quint64 uid = 0;
};

struct Block {
    quint32 track = 0;              // the number of its track
    qint64 timeNs = 0;
    QByteArray data;
};

class Writer
{
public:
    Writer();
    ~Writer();

    // The tracks and the history are given before the file is opened. The number of the track is set
    // in `track`, and is what writeBlock takes.
    void addTrack(Track &track);
    void setHistory(const QJsonArray &entries);

    // The id the next track added to a file gets. It only goes up: the id of a removed track is not given again.
    // By default it is one above the highest id of the tracks.
    void setNextTrackId(quint32 id);

    // A file kept in the LDX file (the session of a stack, for instance), written before the data
    void addAttachment(const QString &name, const QString &mediaType, const QByteArray &data);

    bool open(const QString &fileName, QString &error);

    // Blocks are written in the order of their time, all tracks together
    bool writeBlock(quint32 track, qint64 timeNs, const QByteArray &data);
    bool finish(QString &error);

    // Why the last writeBlock failed
    QString lastError() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class Reader
{
public:
    Reader();
    ~Reader();

    bool open(const QString &fileName, QString &error);

    int profile() const;                    // the LDX_PROFILE of the file, 0 if it has none
    QVector<Track> tracks() const;
    QJsonArray history() const;
    quint32 nextTrackId() const;

    struct Attachment {
        QString name;
        QString mediaType;
        QByteArray data;
    };
    QVector<Attachment> attachments() const;

    // The next block of the file, in the order it was written. False at the end.
    bool nextBlock(Block &block);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace Ldx

#endif // LDX_H
