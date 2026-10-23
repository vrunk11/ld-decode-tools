/******************************************************************************
 * framemap.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "framemap.h"

#include "jsonio.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace {
constexpr qint32 MAP_VERSION = 1;
constexpr const char *LETTERS = "musdgp";
}

QChar mapBasisLetter(MapBasis basis)
{
    return QLatin1Char(static_cast<char>(basis));
}

bool mapBasisFromLetter(QChar letter, MapBasis &basis)
{
    for (const char *p = LETTERS; *p; p++) {
        if (letter == QLatin1Char(*p)) {
            basis = static_cast<MapBasis>(*p);
            return true;
        }
    }
    return false;
}

void FrameMap::indexRuns()
{
    runStarts.clear();
    runStarts.reserve(runList.size());
    qint32 place = 1;
    for (const Run &run : runList) {
        runStarts.append(place);
        place += run.count;
    }
}

FrameMap FrameMap::fromFrames(const QVector<MapFrame> &frames)
{
    FrameMap map;
    for (qint32 i = 0; i < frames.size(); i++) {
        const MapFrame &frame = frames[i];
        if (frame.unusable) map.unusablePlaces.append(i + 1);

        if (!map.runList.isEmpty()) {
            Run &last = map.runList.last();
            const bool padded = frame.captureFrame == 0;
            const bool follows = frame.discFrame == last.discFrame + last.count
                    && frame.basis == last.basis
                    && (padded ? last.captureFrame == 0 : (last.captureFrame != 0 && frame.captureFrame == last.captureFrame + last.count));
            if (follows) {
                last.count++;
                continue;
            }
        }
        Run run;
        run.discFrame = frame.discFrame;
        run.captureFrame = frame.captureFrame;
        run.count = 1;
        run.basis = frame.basis;
        map.runList.append(run);
    }
    map.indexRuns();
    return map;
}

qint32 FrameMap::frameCount() const
{
    return runStarts.isEmpty() ? 0 : runStarts.last() + runList.last().count - 1;
}

MapFrame FrameMap::frameAt(qint32 place) const
{
    MapFrame frame;
    frame.basis = MapBasis::Padded;
    if (place < 1 || place > frameCount()) return frame;

    const auto it = std::upper_bound(runStarts.begin(), runStarts.end(), place);
    const qint32 index = static_cast<qint32>(it - runStarts.begin()) - 1;
    const Run &run = runList[index];
    const qint32 inRun = place - runStarts[index];
    frame.discFrame = run.discFrame + inRun;
    frame.captureFrame = run.captureFrame == 0 ? 0 : run.captureFrame + inRun;
    frame.basis = run.basis;
    frame.unusable = std::binary_search(unusablePlaces.begin(), unusablePlaces.end(), place);
    return frame;
}

QVector<MapFrame> FrameMap::frames() const
{
    QVector<MapFrame> result;
    result.reserve(frameCount());
    for (qint32 place = 1; place <= frameCount(); place++) result.append(frameAt(place));
    return result;
}

QVector<qint32> FrameMap::basisCounts() const
{
    QVector<qint32> counts(6, 0);
    for (const Run &run : runList) {
        for (qint32 i = 0; i < 6; i++) {
            if (static_cast<char>(run.basis) == LETTERS[i]) counts[i] += run.count;
        }
    }
    return counts;
}

double FrameMap::measuredShare() const
{
    const qint32 total = frameCount();
    return total == 0 ? 0.0 : static_cast<double>(basisCounts()[0]) / total;
}

QJsonObject FrameMap::toJson() const
{
    QJsonObject object;
    object["ldxMap"] = MAP_VERSION;

    QJsonObject capture;
    capture["fields"] = static_cast<double>(captureFields);
    capture["size"] = static_cast<double>(captureSize);
    object["capture"] = capture;
    if (!madeBy.isEmpty()) object["madeBy"] = madeBy;
    if (reverseFields) object["reverseFields"] = true;
    if (!discType.isEmpty()) object["discType"] = discType;

    QJsonArray runs;
    for (const Run &run : runList) {
        runs.append(QJsonArray{ run.discFrame, run.captureFrame, run.count, QString(mapBasisLetter(run.basis)) });
    }
    object["runs"] = runs;

    if (!unusablePlaces.isEmpty()) {
        QJsonArray unusable;
        for (qint32 place : unusablePlaces) unusable.append(place);
        object["unusable"] = unusable;
    }

    // The totals, so that a tool can say "98.6 % measured" without reading the runs
    const QVector<qint32> counts = basisCounts();
    QJsonObject totals;
    totals["frames"] = frameCount();
    for (qint32 i = 0; i < 6; i++) {
        if (counts[i] > 0) totals[QString(QLatin1Char(LETTERS[i]))] = counts[i];
    }
    object["counts"] = totals;
    return object;
}

QByteArray FrameMap::toJsonBytes() const
{
    return QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
}

bool FrameMap::fromJson(const QJsonObject &object, FrameMap &map, QString &error)
{
    map = FrameMap();
    if (!object.contains("ldxMap")) {
        error = "not a map (there is no \"ldxMap\")";
        return false;
    }
    if (object["ldxMap"].toInt(-1) != MAP_VERSION) {
        error = "a map of another version (" + QString::number(object["ldxMap"].toInt(-1)) + ")";
        return false;
    }

    const QJsonObject capture = object["capture"].toObject();
    map.captureFields = static_cast<qint64>(capture["fields"].toDouble(0));
    map.captureSize = static_cast<qint64>(capture["size"].toDouble(0));
    map.madeBy = object["madeBy"].toString();
    map.reverseFields = object["reverseFields"].toBool(false);
    map.discType = object["discType"].toString();

    const QJsonArray runs = object["runs"].toArray();
    for (const QJsonValue &value : runs) {
        const QJsonArray entry = value.toArray();
        MapBasis basis;
        if (entry.size() != 4 || entry[3].toString().size() != 1 || !mapBasisFromLetter(entry[3].toString()[0], basis)) {
            error = "a run of the map is not [disc frame, capture frame, count, basis]";
            return false;
        }
        Run run;
        run.discFrame = entry[0].toInt();
        run.captureFrame = entry[1].toInt();
        run.count = entry[2].toInt();
        run.basis = basis;
        if (run.count < 1 || run.captureFrame < 0 || (basis == MapBasis::Padded) != (run.captureFrame == 0)) {
            error = "a run of the map has a count under 1, or a capture frame that does not go with its basis";
            return false;
        }
        map.runList.append(run);
    }
    map.indexRuns();

    const QJsonArray unusable = object["unusable"].toArray();
    qint32 previous = 0;
    for (const QJsonValue &value : unusable) {
        const qint32 place = value.toInt();
        if (place <= previous || place > map.frameCount()) {
            error = "the unusable frames of the map are not places of it, in order";
            return false;
        }
        map.unusablePlaces.append(place);
        previous = place;
    }
    return true;
}

bool FrameMap::fromJsonBytes(const QByteArray &bytes, FrameMap &map, QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) {
        error = "not JSON: " + parseError.errorString();
        return false;
    }
    return fromJson(document.object(), map, error);
}

void FrameMap::write(JsonWriter &writer) const
{
    writer.beginObject();
    writer.writeMember("capture");
    writer.beginObject();
    writer.writeMember("fields", static_cast<qint64>(captureFields));
    writer.writeMember("size", static_cast<qint64>(captureSize));
    writer.endObject();
    if (!discType.isEmpty()) writer.writeMember("discType", discType);
    writer.writeMember("ldxMap", MAP_VERSION);
    if (!madeBy.isEmpty()) writer.writeMember("madeBy", madeBy);
    if (reverseFields) writer.writeMember("reverseFields", true);
    writer.writeMember("runs");
    writer.beginArray();
    for (const Run &run : runList) {
        writer.writeElement();
        writer.beginArray();
        writer.writeElement();
        writer.write(static_cast<int>(run.discFrame));
        writer.writeElement();
        writer.write(static_cast<int>(run.captureFrame));
        writer.writeElement();
        writer.write(static_cast<int>(run.count));
        writer.writeElement();
        writer.write(QString(mapBasisLetter(run.basis)));
        writer.endArray();
    }
    writer.endArray();
    if (!unusablePlaces.isEmpty()) {
        writer.writeMember("unusable");
        writer.beginArray();
        for (qint32 place : unusablePlaces) {
            writer.writeElement();
            writer.write(static_cast<int>(place));
        }
        writer.endArray();
    }
    writer.endObject();
}

bool FrameMap::read(JsonReader &reader, QString &error)
{
    *this = FrameMap();
    QVector<qint32> places;
    qint32 version = 0;
    try {
        reader.beginObject();
        std::string member;
        while (reader.readMember(member)) {
            if (member == "ldxMap") {
                reader.read(version);
            } else if (member == "capture") {
                reader.beginObject();
                std::string inner;
                while (reader.readMember(inner)) {
                    qint64 value = 0;
                    if (inner == "fields") { reader.read(value); captureFields = value; }
                    else if (inner == "size") { reader.read(value); captureSize = value; }
                    else reader.discard();
                }
                reader.endObject();
            } else if (member == "madeBy") {
                reader.read(madeBy);
            } else if (member == "discType") {
                reader.read(discType);
            } else if (member == "reverseFields") {
                reader.read(reverseFields);
            } else if (member == "runs") {
                reader.beginArray();
                while (reader.readElement()) {
                    Run run;
                    std::string letter;
                    reader.beginArray();
                    bool ok = reader.readElement();
                    if (ok) reader.read(run.discFrame);
                    ok = ok && reader.readElement();
                    if (ok) reader.read(run.captureFrame);
                    ok = ok && reader.readElement();
                    if (ok) reader.read(run.count);
                    ok = ok && reader.readElement();
                    if (ok) reader.read(letter);
                    if (ok) while (reader.readElement()) reader.discard();
                    reader.endArray();
                    MapBasis basis;
                    if (!ok || letter.size() != 1 || !mapBasisFromLetter(QLatin1Char(letter[0]), basis)) {
                        error = "a run of the map is not [disc frame, capture frame, count, basis]";
                        return false;
                    }
                    run.basis = basis;
                    if (run.count < 1 || run.captureFrame < 0 || (basis == MapBasis::Padded) != (run.captureFrame == 0)) {
                        error = "a run of the map has a count under 1, or a capture frame that does not go with its basis";
                        return false;
                    }
                    runList.append(run);
                }
                reader.endArray();
            } else if (member == "unusable") {
                reader.beginArray();
                while (reader.readElement()) {
                    qint32 place = 0;
                    reader.read(place);
                    places.append(place);
                }
                reader.endArray();
            } else {
                reader.discard();
            }
        }
        reader.endObject();
    } catch (JsonReader::Error &jsonError) {
        error = QString("not a map: ") + jsonError.what();
        return false;
    }
    if (version != MAP_VERSION) {
        error = "a map of another version (" + QString::number(version) + ")";
        return false;
    }
    indexRuns();
    qint32 previous = 0;
    for (qint32 place : places) {
        if (place <= previous || place > frameCount()) {
            error = "the unusable frames of the map are not places of it, in order";
            return false;
        }
        previous = place;
    }
    unusablePlaces = places;
    return true;
}

bool FrameMap::operator==(const FrameMap &other) const
{
    if (captureFields != other.captureFields || captureSize != other.captureSize || reverseFields != other.reverseFields || discType != other.discType || unusablePlaces != other.unusablePlaces) return false;
    if (runList.size() != other.runList.size()) return false;
    for (qint32 i = 0; i < runList.size(); i++) {
        const Run &a = runList[i];
        const Run &b = other.runList[i];
        if (a.discFrame != b.discFrame || a.captureFrame != b.captureFrame || a.count != b.count || a.basis != b.basis) return false;
    }
    return true;
}
