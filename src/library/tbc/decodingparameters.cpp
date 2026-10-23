/******************************************************************************
 * decodingparameters.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "decodingparameters.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <sstream>

#include "jsonio.h"

namespace {
constexpr qint32 DECODING_VERSION = 1;
}

QByteArray DecodingParameters::toJsonBytes() const
{
    std::ostringstream out;
    JsonWriter writer(out);
    write(writer);
    const std::string text = out.str();
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

bool DecodingParameters::fromJsonBytes(const QByteArray &bytes, DecodingParameters &parameters, QString &error)
{
    std::istringstream in(std::string(bytes.constData(), static_cast<size_t>(bytes.size())));
    JsonReader reader(in);
    return parameters.read(reader, error);
}

void DecodingParameters::write(JsonWriter &writer) const
{
    writer.beginObject();
    writer.writeMember("decoder", decoder);
    writer.writeMember("ldxDecoding", DECODING_VERSION);
    if (!madeBy.isEmpty()) writer.writeMember("madeBy", madeBy);
    writer.writeMember("settings");
    writer.beginObject();
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        writer.writeMember(it.key().toUtf8().constData(), it.value());
    }
    writer.endObject();
    writer.endObject();
}

bool DecodingParameters::read(JsonReader &reader, QString &error)
{
    *this = DecodingParameters();
    qint32 version = 0;
    try {
        reader.beginObject();
        std::string member;
        while (reader.readMember(member)) {
            if (member == "ldxDecoding") reader.read(version);
            else if (member == "decoder") reader.read(decoder);
            else if (member == "madeBy") reader.read(madeBy);
            else if (member == "settings") {
                reader.beginObject();
                std::string name;
                while (reader.readMember(name)) {
                    double number = 0.0;
                    reader.read(number);
                    settings.insert(QString::fromStdString(name), number);
                }
                reader.endObject();
            } else {
                reader.discard();
            }
        }
        reader.endObject();
    } catch (JsonReader::Error &jsonError) {
        error = QString("not decoding parameters: ") + jsonError.what();
        return false;
    }
    if (version != DECODING_VERSION) {
        error = "decoding parameters of another version (" + QString::number(version) + ")";
        return false;
    }
    if (decoder.isEmpty()) {
        error = "the decoding parameters name no decoder";
        return false;
    }
    return true;
}

bool DecodingParameters::isTape(const QString &tapeFormat, const QString &decoder)
{
    return !tapeFormat.isEmpty() || decoder == QLatin1String("vhs-decode");
}

bool DecodingParameters::prefers2D(bool stackOfSources, bool tape)
{
    return stackOfSources && tape;
}

QString DecodingParameters::defaultDecoder(bool ntsc, bool twoD)
{
    if (ntsc) return twoD ? QStringLiteral("ntsc2d") : QStringLiteral("ntsc3d");
    return twoD ? QStringLiteral("transform2d") : QStringLiteral("transform3d");
}
