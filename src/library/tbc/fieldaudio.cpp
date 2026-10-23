/******************************************************************************
 * fieldaudio.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "fieldaudio.h"

#include <QFileInfo>

QVector<FieldAudio::Range> FieldAudio::rangesOf(LdDecodeMetaData &metadata)
{
    QVector<Range> ranges;
    if (!metadata.hasPcmAudioParameters()) return ranges;
    const qint32 fields = metadata.getNumberOfFields();
    ranges.resize(fields);
    for (qint32 n = 1; n <= fields; n++) {
        ranges[n - 1].start = metadata.getFieldPcmAudioStart(n);
        ranges[n - 1].length = metadata.getFieldPcmAudioLength(n);
    }
    return ranges;
}

QVector<FieldAudio::Range> FieldAudio::rangesOfView(LdDecodeMetaData &capture, const QVector<qint32> &viewTable, qint32 paddedSamples)
{
    QVector<Range> ranges;
    if (!capture.hasPcmAudioParameters()) return ranges;
    ranges.resize(viewTable.size());
    for (qint32 i = 0; i < viewTable.size(); i++) {
        if (viewTable[i] == 0) {
            ranges[i].start = -1;
            ranges[i].length = paddedSamples;
        } else {
            ranges[i].start = capture.getFieldPcmAudioStart(viewTable[i]);
            ranges[i].length = capture.getFieldPcmAudioLength(viewTable[i]);
        }
    }
    return ranges;
}

bool FieldAudio::open(const QString &videoFile, const QVector<Range> &ranges)
{
    opened = false;
    if (ranges.isEmpty()) return false;
    if (!audio.open(QFileInfo(videoFile))) return false;
    fieldRanges = ranges;
    opened = true;
    return true;
}

QByteArray FieldAudio::audioOf(qint32 fieldNumber)
{
    if (!opened || fieldNumber < 1 || fieldNumber > fieldRanges.size()) return QByteArray();
    const Range &range = fieldRanges[fieldNumber - 1];
    if (range.length <= 0) return QByteArray();
    if (range.start < 0) return QByteArray(range.length * 4, '\0');
    const SourceAudio::Data samples = audio.getAudioDataPadded(range.start, range.length);
    return QByteArray(reinterpret_cast<const char *>(samples.constData()), static_cast<qsizetype>(samples.size()) * 2);
}
