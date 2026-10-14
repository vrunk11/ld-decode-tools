/******************************************************************************
 * ldxfilter.h
 * ld-decode-tools TBC library - a tool that adds metadata to an LDX stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXFILTER_H
#define LDXFILTER_H

#include <QString>
#include <functional>

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// The part of a chain of pipes that works on the metadata of the fields: an LDX stream comes in on the standard input,
// and goes out on the standard output with the same tracks, the same blocks, and the metadata of each field changed by
// the tool. The metadata of a field go out in a block of their own just before its video (see the specification, the
// pipes), so a field is passed on as soon as it is done: the tool next in the chain does not wait for the whole stream.
//
// Metadata that came with the stream, as a whole (a stream made from a file) or field by field, are the ones the tool
// is given; a stream without any gets the defaults of the system of its video. The other tracks are passed on as
// they are, in their place among the fields.
class LdxFieldFilter
{
public:
    struct Options {
        QString tool;           // for the history of the stream
        QString detail;         // what it did
        QString version;
        qint32 threads = 1;     // fields worked on at the same time
    };

    // Called for each field, in threads of its own: field is the metadata to change, data the samples of the field
    // (its lines in order, from line 1)
    using Process = std::function<void(qint32 fieldNumber, const SourceVideo::Data &data,
                                       const LdDecodeMetaData::VideoParameters &videoParameters,
                                       LdDecodeMetaData::Field &field)>;

    // Do it, until the stream ends. False, with the reason, if the input is not a stream with TBC video or the output fails.
    static bool run(const Options &options, const Process &process, QString &error);
};

#endif // LDXFILTER_H
