/******************************************************************************
 * ldxcapture.h
 * ld-decode-tools TBC library - a capture kept in an LDX file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXCAPTURE_H
#define LDXCAPTURE_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <map>
#include <memory>

// A capture kept in an LDX file (docs/Specifications/container-format.md), as the tools see it: a name that
// says which one, the fields of its TBC track by number, and its metadata. A name is the path of the file,
// and, for one of the tracks of a file that holds several, "#" and the id of the track:
//
//     capture.ldx        the stack of the session the file holds, or, if it has none, its first TBC track
//     capture.ldx#3      the track 3
//
// (When the library is built without LDX support, every call fails and says so.)
// The current metadata of a capture, from the blocks of its metadata track: a block with the parameters of the video
// is the metadata (the last one is current), and the blocks of single fields (a stream whose metadata came field by
// field) are put in it, the last block of a field being its current metadata.
class LdxMetadataMerger
{
public:
    void add(const QByteArray &data);
    QByteArray current() const;

private:
    QByteArray base;
    std::map<int, QByteArray> fieldJson;
};

class LdxCapture
{
public:
    LdxCapture();
    ~LdxCapture();

    // A line of the history of an LDX file, which says who did what, when and with what, to be used later (to know how
    // a capture was made, to do it again, to tell a tool what it has to do): the tool and its version, the command, the
    // time (UTC), the arguments of the program, and whatever the tool adds (the files it read and wrote, the options it
    // took, what it counted) in extra, as members of the entry.
    static QJsonObject historyEntry(const QString &command, const QString &detail = QString(), const QJsonObject &extra = QJsonObject());

    // Whether a name is that of an LDX file or of one of its tracks
    static bool isLdxName(const QString &name);
    // The path and the track (0 when none is named) of a name
    static void split(const QString &name, QString &path, qint32 &track);
    // The name of a track of a file
    static QString trackName(const QString &path, qint32 track);

    // Whether the capture a name gives is CVBS (a cvbs track) and not a TBC
    static bool isCvbs(const QString &name);

    // The TBC tracks of an LDX file, with their names: the captures it holds
    static bool listCaptures(const QString &path, QVector<qint32> &tracks, QStringList &names, QString &error);

    // Whether the file keeps a stacking session, and the session
    static bool hasSession(const QString &path);
    static bool readSession(const QString &path, QByteArray &session, QString &error);

    // Open the capture a name gives (a track; the session of the file is not a capture)
    bool open(const QString &name, QString &error);
    qint32 track() const;
    qint32 numberOfFields() const;
    QJsonObject parameters() const;                        // of the track
    // The whole field, from 1. Empty if it is not there.
    QVector<quint16> getField(qint32 fieldNumber);
    // The ld-decode metadata of the capture, as JSON
    bool readMetadata(QByteArray &json, QString &error);

    // Keep new metadata for a capture in its LDX file: a block added at the end of the file, which is the current one from
    // then on (what was there stays, and the history says what was done)
    static bool writeMetadata(const QString &name, const QByteArray &json, QString &error);

    // The analogue audio of the capture: the PCM track that is about it (16-bit stereo), by samples (a sample is a pair)
    bool openAudio(const QString &name, QString &error);
    qint64 audioSamples() const;
    double audioSampleRate() const;
    // Left and right in turn, or less if the audio ends
    QVector<qint16> getAudio(qint64 startSample, qint64 numberOfSamples);

private:
    // The metadata track that is about this capture (0 if none)
    quint32 metadataTrackNumber() const;

    struct Private;
    std::unique_ptr<Private> d;
};

#endif // LDXCAPTURE_H
