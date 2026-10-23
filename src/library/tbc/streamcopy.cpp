/******************************************************************************
 * streamcopy.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "streamcopy.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <limits>

#include "ldxcapture.h"
#include "targetvideo.h"
#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

QString StreamCopy::Candidate::description() const
{
    QString text = kind;
    if (kind == QLatin1String("pcm")) text = depth > 16 ? QStringLiteral("audio, %1 bits").arg(depth) : QStringLiteral("analogue audio");
    text += QStringLiteral(" - %1 blocks").arg(blocks);
    if (!name.isEmpty()) text += QStringLiteral(" - ") + name;
    if (derived) text += QStringLiteral(" (written from the fields)");
    return text;
}

QString StreamCopy::Candidate::selector() const
{
    return QStringLiteral("%1:#%2").arg(source + 1).arg(track);
}

#ifdef LDDECODE_WITH_LDX

QVector<StreamCopy::Candidate> StreamCopy::candidates(const QStringList &sources)
{
    QVector<Candidate> result;
    for (qint32 s = 0; s < sources.size(); s++) {
        const QString &name = sources[s];
        if (!LdxCapture::isLdxName(name)) continue;
        QString path;
        qint32 videoTrack = 0;
        LdxCapture::split(name, path, videoTrack);
        Ldx::Reader reader;
        QString error;
        if (!reader.open(path, error) || !reader.buildIndex(error)) continue;
        for (const Ldx::Track &track : reader.tracks()) {
            const Ldx::Kind kind = track.kind;
            if (kind != Ldx::Kind::Efm && kind != Ldx::Kind::Ac3 && kind != Ldx::Kind::Rf && kind != Ldx::Kind::Pcm) continue;
            // Only the streams of the capture that is the source: those that name its video track, or all of them if the file has one capture
            const qint32 of = track.parameters.value("track").toInt(0);
            if (videoTrack > 0 && of > 0 && of != videoTrack) continue;
            Candidate candidate;
            candidate.source = s;
            candidate.file = path;
            candidate.track = track.number;
            candidate.kind = Ldx::kindName(kind);
            candidate.codec = Ldx::codecName(track.codec);
            candidate.depth = track.depth;
            candidate.name = track.name;
            candidate.parameters = track.parameters;
            candidate.blocks = reader.blockCount(track.number);
            candidate.derived = kind == Ldx::Kind::Pcm && track.depth <= 16;
            result.append(candidate);
        }
    }
    return result;
}

#else

QVector<StreamCopy::Candidate> StreamCopy::candidates(const QStringList &) { return QVector<Candidate>(); }

#endif

QVector<StreamCopy::Candidate> StreamCopy::defaultChoice(const QVector<Candidate> &all)
{
    QVector<Candidate> chosen;
    for (const Candidate &candidate : all) {
        if (candidate.source == 0 && !candidate.derived && candidate.kind != QLatin1String("rf")) chosen.append(candidate);
    }
    return chosen;
}

bool StreamCopy::parseChoice(const QString &text, const QVector<Candidate> &all, QVector<Candidate> &chosen, QString &error)
{
    chosen.clear();
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String("default")) {
        chosen = defaultChoice(all);
        return true;
    }
    if (trimmed == QLatin1String("none")) return true;

    for (const QString &item : trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList parts = item.trimmed().split(QLatin1Char(':'));
        bool ok = false;
        const qint32 number = parts.size() == 2 ? parts[0].toInt(&ok) : 0;
        if (!ok || number < 1) {
            error = QObject::tr("\"%1\" is not source:stream (for example 1:efm, 2:#4)").arg(item);
            return false;
        }
        const QString selector = parts[1].trimmed();
        bool found = false;
        for (const Candidate &candidate : all) {
            if (candidate.source != number - 1 || candidate.derived) continue;
            const bool matches = selector == QLatin1String("all")
                    || selector == candidate.kind
                    || (selector == QLatin1String("audio") && candidate.kind == QLatin1String("pcm"))
                    || selector == QStringLiteral("#%1").arg(candidate.track);
            if (!matches) continue;
            found = true;
            bool already = false;
            for (const Candidate &have : chosen) already = already || (have.source == candidate.source && have.track == candidate.track);
            if (!already) chosen.append(candidate);
        }
        if (!found) {
            error = QObject::tr("The source %1 has no stream \"%2\" to copy").arg(number).arg(selector);
            return false;
        }
    }
    return true;
}

QString StreamCopy::historyDetail(const QVector<Candidate> &chosen, const QStringList &sources, const QMap<qint32, QJsonObject> &alignments)
{
    if (chosen.isEmpty()) return QString();
    QStringList parts;
    for (const Candidate &candidate : chosen) {
        const QString file = candidate.source < sources.size() ? QFileInfo(sources[candidate.source]).fileName() : QString();
        QString part = QStringLiteral("%1 (track %2, %3 blocks) of source %4 %5").arg(candidate.kind).arg(candidate.track).arg(candidate.blocks).arg(candidate.source + 1).arg(file);
        if (alignments.contains(candidate.source)) part += QStringLiteral(" [") + alignmentSummary(alignments.value(candidate.source)) + QStringLiteral("]");
        parts << part;
    }
    return QStringLiteral("streams copied as they are, with their own times (not realigned to the frames of the output; the alignment is in the track): ") + parts.join(QStringLiteral("; "));
}

QJsonObject StreamCopy::alignmentOf(const QVector<qint32> &sourceFrames)
{
    QJsonArray segments;
    qint32 outputStart = 0, sourceStart = 0, count = 0;
    const auto flush = [&]() {
        if (count > 0) segments.append(QJsonArray{ outputStart, sourceStart, count });
    };
    for (qint32 i = 0; i < sourceFrames.size(); i++) {
        const qint32 frame = sourceFrames[i];
        const bool follows = count > 0 && ((sourceStart == 0 && frame == 0) || (sourceStart != 0 && frame == sourceStart + count));
        if (follows) {
            count++;
        } else {
            flush();
            outputStart = i + 1;
            sourceStart = frame;
            count = 1;
        }
    }
    flush();
    QJsonObject object;
    object["unit"] = "frame";
    object["segments"] = segments;
    return object;
}

QString StreamCopy::alignmentSummary(const QJsonObject &alignment)
{
    // (the pieces where the output has no frame of the source do not count: a source that begins later or ends sooner is shifted all the same)
    QVector<QJsonArray> pieces;
    for (const QJsonValue &value : alignment.value("segments").toArray()) {
        const QJsonArray segment = value.toArray();
        if (segment.at(1).toInt() > 0) pieces.append(segment);
    }
    if (pieces.isEmpty()) return QStringLiteral("no frame of the output has this source");
    if (pieces.size() == 1) {
        const qint32 shift = pieces[0].at(1).toInt() - pieces[0].at(0).toInt();
        return shift == 0 ? QStringLiteral("not shifted from the video")
                          : QStringLiteral("shifted from the video by %1 frames: the frame 1 of the output is the frame %2 of the source").arg(shift).arg(1 + shift);
    }
    return QStringLiteral("in %1 pieces, each shifted from the video by its own number of frames: see the alignment of the track").arg(pieces.size());
}

#ifdef LDDECODE_WITH_LDX

struct StreamCopy::Private {
    struct Cursor {
        Candidate candidate;
        std::shared_ptr<Ldx::Reader> reader;
        qint32 outputTrack = -1;
        qint64 index = 0;
        qint64 count = 0;
    };
    QVector<Cursor> cursors;
    QMap<QString, std::shared_ptr<Ldx::Reader>> readers;       // one for each file (the tracks of one file are read through the same reader)
};

StreamCopy::StreamCopy() : d(std::make_unique<Private>()) {}
StreamCopy::~StreamCopy() = default;

bool StreamCopy::isEmpty() const { return d->cursors.isEmpty(); }

bool StreamCopy::prepare(const QVector<Candidate> &chosen, TargetVideo &target, QString &error, const QMap<qint32, QJsonObject> &alignments)
{
    d->cursors.clear();
    for (const Candidate &candidate : chosen) {
        std::shared_ptr<Ldx::Reader> reader = d->readers.value(candidate.file);
        if (!reader) {
            reader = std::make_shared<Ldx::Reader>();
            if (!reader->open(candidate.file, error) || !reader->buildIndex(error)) return false;
            d->readers.insert(candidate.file, reader);
        }
        Private::Cursor cursor;
        cursor.candidate = candidate;
        cursor.reader = reader;
        cursor.count = reader->blockCount(candidate.track);
        QJsonObject parameters = candidate.parameters;
        parameters.remove("track");
        if (alignments.contains(candidate.source)) parameters["alignment"] = alignments.value(candidate.source);
        cursor.outputTrack = target.addLdxTrack(candidate.kind, candidate.codec, candidate.depth, candidate.name, parameters);
        if (cursor.outputTrack < 0) {
            error = QObject::tr("The output cannot take other streams (it is not an LDX file)");
            return false;
        }
        d->cursors.append(cursor);
    }
    return true;
}

bool StreamCopy::writeUpTo(TargetVideo &target, qint64 timeNs)
{
    bool progress = true;
    while (progress) {
        progress = false;
        // The block that comes first, of all the streams
        qint32 best = -1;
        qint64 bestTime = 0;
        for (qint32 i = 0; i < d->cursors.size(); i++) {
            Private::Cursor &cursor = d->cursors[i];
            if (cursor.index >= cursor.count) continue;
            const qint64 time = cursor.reader->blockTimeNs(cursor.candidate.track, cursor.index);
            if (best < 0 || time < bestTime) {
                best = i;
                bestTime = time;
            }
        }
        if (best < 0 || bestTime >= timeNs) break;
        Private::Cursor &cursor = d->cursors[best];
        Ldx::Block block;
        if (!cursor.reader->readBlock(cursor.candidate.track, cursor.index, block)) return false;
        if (!target.writeLdxTrack(cursor.outputTrack, block.timeNs, block.data)) return false;
        cursor.index++;
        progress = true;
    }
    return true;
}

bool StreamCopy::finish(TargetVideo &target)
{
    return writeUpTo(target, std::numeric_limits<qint64>::max());
}

#else

struct StreamCopy::Private {};
StreamCopy::StreamCopy() : d(std::make_unique<Private>()) {}
StreamCopy::~StreamCopy() = default;
bool StreamCopy::isEmpty() const { return true; }
bool StreamCopy::prepare(const QVector<Candidate> &, TargetVideo &, QString &error, const QMap<qint32, QJsonObject> &) { error = QObject::tr("This program was built without support for LDX files"); return false; }
bool StreamCopy::writeUpTo(TargetVideo &, qint64) { return true; }
bool StreamCopy::finish(TargetVideo &) { return true; }

#endif
