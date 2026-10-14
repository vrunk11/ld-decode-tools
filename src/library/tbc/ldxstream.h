/******************************************************************************
 * ldxstream.h
 * ld-decode-tools TBC library - an LDX file coming in on the standard input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXSTREAM_H
#define LDXSTREAM_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <cstdio>
#include <memory>

#include "lddecodemetadata.h"

// An LDX stream on the standard input (`something | tool -`): what a producer sends in one pipe, whatever is in it.
// The tools that read video from a pipe take the video of the stream as they take a TBC or CVBS stream, and the
// metadata from the stream instead of from a file or from the command line.
//
// openStdin reads what comes before the data (the tracks, the tags) and the metadata of the video, which a producer sends
// first, and then a thread of its own goes on reading the stream: the blocks of the first TBC or CVBS track are decoded
// (a FLAC or FFV1 track too) and given, as fields of a TBC or frames of CVBS, to a pipe of the process that the video
// reader reads. The other tracks are not used by the tools that read a stream (yet).
class LdxStream
{
public:
    ~LdxStream();

    // Take the standard input as an LDX stream. False, with the reason, if it is not one, or has no video.
    static bool openStdin(QString &error);
    // The stream that was opened, if one was
    static LdxStream *instance();

    // What the video is
    bool isCvbs() const;
    VideoSystem system() const;
    QString videoName() const;
    // The metadata the stream began with (ld-decode metadata as JSON): empty if it sent none before the video
    QByteArray metadataJson() const;
    // The metadata as the tools hold them. False if the stream sent none.
    bool readMetadata(LdDecodeMetaData &metadata, QString &error) const;

    // Metadata that comes with the video, field by field (a producer that does not have it all at the start, as when
    // it is itself fed by a pipe, sends the metadata of each field just before its video): the fields that metadata
    // makes (initialiseDefault and ensureFields) take theirs from the stream. Call it once the metadata is set up.
    void provideFields(LdDecodeMetaData &metadata);

    // The video as the raw stream the tools read (fields of 16-bit TBC, or frames of CVBS as 10-bit values in words), as
    // a FILE. The thread that fills it starts with the first call. The stream owns it.
    FILE *videoPipe();

private:
    LdxStream();
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // LDXSTREAM_H
