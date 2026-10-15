/******************************************************************************
 * ldxfilter.cpp
 * ld-decode-tools TBC library - a tool that adds metadata to an LDX stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxfilter.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>

#include <cstring>
#include <deque>
#include <future>
#include <map>

#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#include "ldxcapture.h"
#endif

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

#ifdef LDDECODE_WITH_LDX

namespace {

bool metadataFromJson(const QByteArray &json, LdDecodeMetaData &metadata)
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    QFile file(directory.filePath("metadata.json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return false;
    file.close();
    return metadata.read(file.fileName());
}

// One item of the output, waiting its turn: a field being worked on, or a block that goes through as it is
struct Item {
    Ldx::Block block;
    bool isField = false;
    std::future<QByteArray> metadata;
};

} // namespace

bool LdxFieldFilter::run(const Options &options, const Process &process, QString &error)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    Ldx::Reader reader;
    if (!reader.openStream(stdin, error)) {
        error = QObject::tr("The standard input is not an LDX stream (%1)").arg(error);
        return false;
    }
    if (reader.profile() < 1) {
        error = QObject::tr("The standard input is Matroska, but not an LDX stream");
        return false;
    }

    // The video, and the track of its metadata (there is one made if the stream has none)
    Ldx::Track video;
    bool haveVideo = false;
    quint32 metadataTrack = 0;
    for (const Ldx::Track &track: reader.tracks()) {
        if (!haveVideo && track.kind == Ldx::Kind::Tbc) {
            video = track;
            haveVideo = true;
        }
    }
    if (!haveVideo) {
        error = QObject::tr("The LDX stream has no TBC video (this tool works on the fields of a TBC)");
        return false;
    }
    for (const Ldx::Track &track: reader.tracks()) {
        if (track.kind == Ldx::Kind::Metadata && (track.parameters.value("track").toInt(0) == static_cast<int>(video.number)
                                                  || (!track.parameters.contains("track") && metadataTrack == 0))) {
            metadataTrack = track.number;
        }
    }

    Ldx::Writer writer;
    for (Ldx::Track track: reader.tracks()) writer.addTrack(track);
    writer.setNextTrackId(reader.nextTrackId());
    if (metadataTrack == 0) {
        Ldx::Track track;
        track.kind = Ldx::Kind::Metadata;
        track.depth = 0;
        track.name = video.name + " metadata";
        track.parameters["namespace"] = "ldx.tbcmetadata";
        track.parameters["format"] = "ld-decode-json";
        track.parameters["track"] = static_cast<int>(video.number);
        writer.addTrack(track);
        metadataTrack = track.number;
    }
    for (const Ldx::Reader::Attachment &attachment: reader.attachments()) writer.addAttachment(attachment.name, attachment.mediaType, attachment.data);
    QJsonArray history = reader.history();
    QJsonObject entry = LdxCapture::historyEntry("process", options.detail);
    entry["tool"] = options.tool;
    history.append(entry);
    writer.setHistory(history);
    if (!writer.openStream(stdout, error)) return false;

    LdDecodeMetaData metadata;
    bool haveParameters = false;
    bool parametersSent = false;
    std::vector<LdDecodeMetaData::Field> wholeFields;             // metadata that came as a whole, from a file
    std::map<qint32, LdDecodeMetaData::Field> comingFields;       // and those that come field by field
    LdDecodeMetaData::VideoParameters videoParameters;
    qint32 fieldNumber = 0;
    const size_t window = static_cast<size_t>(qMax<qint32>(1, options.threads)) * 4;
    std::deque<Item> queue;

    auto flush = [&](size_t keep) {
        while (queue.size() > keep) {
            Item &item = queue.front();
            if (item.isField) {
                const QByteArray json = item.metadata.get();
                if (!writer.writeBlock(metadataTrack, item.block.timeNs, json)) return false;
            }
            if (!writer.writeBlock(item.block.track, item.block.timeNs, item.block.data)) return false;
            queue.pop_front();
        }
        return true;
    };

    Ldx::Block block;
    bool ok = true;
    while (ok && reader.nextBlock(block)) {
        if (block.track == metadataTrack) {
            if (block.data.contains("videoParameters")) {
                if (!metadataFromJson(block.data, metadata)) {
                    error = QObject::tr("The metadata of the stream could not be read");
                    return false;
                }
                haveParameters = true;
                wholeFields.clear();
                comingFields.clear();
                for (qint32 number = 1; number <= metadata.getNumberOfFields(); number++) wholeFields.push_back(metadata.getField(number));
            } else {
                LdDecodeMetaData::Field field;
                if (LdDecodeMetaData::fieldFromJson(block.data, field)) comingFields[field.seqNo] = field;
            }
            continue;
        }
        if (block.track != video.number) {
            Item item;
            item.block = std::move(block);
            queue.push_back(std::move(item));
            continue;
        }

        if (!parametersSent) {
            if (!haveParameters) {
                const QString name = video.parameters.value("system").toString();
                metadata.initialiseDefault(name == "PAL" ? PAL : (name == "NTSC" ? NTSC : PAL_M));
            }
            videoParameters = metadata.getVideoParameters();
            const QByteArray json = metadata.parametersJson();
            if (json.isEmpty()) {
                error = QObject::tr("The parameters of the video could not be written");
                return false;
            }
            if (!flush(0) || !writer.writeBlock(metadataTrack, 0, json)) { ok = false; break; }
            parametersSent = true;
        }

        fieldNumber++;
        LdDecodeMetaData::Field field;
        const auto coming = comingFields.find(fieldNumber);
        if (coming != comingFields.end()) {
            field = coming->second;
            comingFields.erase(coming);
        } else if (static_cast<size_t>(fieldNumber) <= wholeFields.size()) {
            field = wholeFields[static_cast<size_t>(fieldNumber) - 1];
        } else {
            field.isFirstField = (fieldNumber % 2) == 1;
        }
        field.seqNo = fieldNumber;

        Item item;
        item.isField = true;
        const qint32 number = fieldNumber;
        SourceVideo::Data samples(block.data.size() / 2);
        std::memcpy(samples.data(), block.data.constData(), static_cast<size_t>(samples.size()) * 2);
        item.metadata = std::async(std::launch::async, [&process, &videoParameters, number, field, samples = std::move(samples)]() mutable {
            process(number, samples, videoParameters, field);
            return LdDecodeMetaData::fieldToJson(field);
        });
        item.block = std::move(block);
        queue.push_back(std::move(item));
        if (!flush(window)) ok = false;
    }
    if (ok) ok = flush(0);
    if (!ok) {
        error = writer.lastError().isEmpty() ? QObject::tr("Could not write to the standard output") : writer.lastError();
        return false;
    }
    return writer.finish(error);
}

#else // no LDX

bool LdxFieldFilter::run(const Options &, const Process &, QString &error)
{
    error = QObject::tr("This program was built without support for LDX files");
    return false;
}

#endif
