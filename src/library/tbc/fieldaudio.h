/******************************************************************************
 * fieldaudio.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FIELDAUDIO_H
#define FIELDAUDIO_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>

#include "lddecodemetadata.h"
#include "sourceaudio.h"

// The analogue audio (16-bit stereo, 44.1 kHz) of the fields of a capture, field by field, for a tool that writes the audio with the video it writes (a stack, a
// corrected capture, in an LDX file): the audio of a field is a range of samples of the audio of the capture, and a field of a capture seen through a map is
// the audio of the field of the capture it is, or silence if it is padded.
class FieldAudio
{
public:
    // A range of the audio of the capture: start -1 is silence of that length
    struct Range {
        qint32 start = -1;
        qint32 length = 0;
    };

    // The ranges of the fields of a capture (index 0 is the field 1), from its metadata: none if they do not hold the audio parameters
    static QVector<Range> rangesOf(LdDecodeMetaData &metadata);
    // The ranges of the fields of a capture seen through a map (MappedMetadata::viewFields), from the metadata of the capture; a padded field is silence of
    // paddedSamples
    static QVector<Range> rangesOfView(LdDecodeMetaData &capture, const QVector<qint32> &viewTable, qint32 paddedSamples);

    // The audio of the capture beside its video file (or in its LDX file); false if it is not there
    bool open(const QString &videoFile, const QVector<Range> &ranges);
    bool isOpen() const { return opened; }
    // The audio of a field (numbered from 1): 16-bit stereo samples, left and right in turn, as bytes. What the file does not hold is silence.
    QByteArray audioOf(qint32 fieldNumber);

private:
    SourceAudio audio;
    QVector<Range> fieldRanges;
    bool opened = false;
};

#endif // FIELDAUDIO_H
