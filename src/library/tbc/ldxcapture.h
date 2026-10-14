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
#include <QVector>
#include <memory>

// A capture kept in an LDX file (docs/Specifications/container-format.md), as the tools see it: a name that
// says which one, the fields of its TBC track by number, and its metadata. A name is the path of the file,
// and, for one of the tracks of a file that holds several, "#" and the id of the track:
//
//     capture.ldx        the stack of the session the file holds, or, if it has none, its first TBC track
//     capture.ldx#3      the track 3
//
// (When the library is built without LDX support, every call fails and says so.)
class LdxCapture
{
public:
    LdxCapture();
    ~LdxCapture();

    // Whether a name is that of an LDX file or of one of its tracks
    static bool isLdxName(const QString &name);
    // The path and the track (0 when none is named) of a name
    static void split(const QString &name, QString &path, qint32 &track);
    // The name of a track of a file
    static QString trackName(const QString &path, qint32 track);

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

private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // LDXCAPTURE_H
