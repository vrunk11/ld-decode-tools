/******************************************************************************
 * captureinfo.cpp
 * ld-decode-tools - what a person says of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "captureinfo.h"

namespace CaptureInfo {

const QStringList &media()
{
    static const QStringList list = { "ld", "vhs", "vhd", "cvbs" };
    return list;
}

bool parseMedium(const QString &text, QString &value)
{
    const QString lower = text.trimmed().toLower();
    if (lower.isEmpty() || lower == "unknown") {
        value.clear();
        return true;
    }
    if (!media().contains(lower)) return false;
    value = lower;
    return true;
}

QString mediumName(const QString &medium)
{
    if (medium.isEmpty()) return QStringLiteral("unknown");
    return medium.toUpper();
}



const QStringList &faces()
{
    static const QStringList list = { "A", "B", "AB", "BA" };
    return list;
}

bool parseFace(const QString &text, QString &value)
{
    const QString upper = text.trimmed().toUpper();
    if (upper.isEmpty()) {
        value.clear();
        return true;
    }
    if (!faces().contains(upper)) return false;
    value = upper;
    return true;
}

}

#include "ldxcapture.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace CaptureInfo {

static QString tr(const char *text)
{
    return QCoreApplication::translate("CaptureInfo", text);
}

QVector<Row> saidRows(const QString &ldxName)
{
    QVector<Row> rows;
    LdxCapture::Said said;
    if (LdxCapture::isLdxName(ldxName)) LdxCapture::readSaid(ldxName, said);
    if (!said.medium.isEmpty()) rows.append({ tr("Medium"), mediumName(said.medium) });
    if (!said.reference.isEmpty()) rows.append({ tr("Reference"), said.reference });
    if (said.copy > 0) rows.append({ tr("Disc copy"), QString::number(said.copy) });
    if (said.capture > 0) rows.append({ tr("Capture no."), QString::number(said.capture) });
    if (!said.face.isEmpty()) rows.append({ tr("Face"), said.face });
    return rows;
}

QVector<Row> rfSourceRows(const QString &ldxName)
{
    QVector<Row> rows;
    if (!LdxCapture::isLdxName(ldxName)) return rows;
    QString path;
    qint32 track = 0;
    LdxCapture::split(ldxName, path, track);
    QByteArray json;
    QString error;
    if (!LdxCapture::readRfSource(path, track, json, error)) return rows;
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.contains("name")) rows.append({ tr("Decoded from"), object.value("name").toString() });
    if (object.contains("archive")) rows.append({ tr("In the archive"), object.value("archive").toString() });
    if (object.value("rate_hz").toDouble() > 0) rows.append({ tr("RF sample rate"), tr("%1 MHz").arg(QString::number(object.value("rate_hz").toDouble() / 1.0e6, 'f', 3)) });
    if (object.value("bits").toInt() > 0) rows.append({ tr("RF bits"), QString::number(object.value("bits").toInt()) });
    if (!object.value("file_time").toString().isEmpty()) {
        const QDateTime when = QDateTime::fromString(object.value("file_time").toString(), Qt::ISODate);
        rows.append({ tr("RF file date"), when.isValid() ? when.toLocalTime().toString(Qt::ISODate).replace('T', ' ') : object.value("file_time").toString() });
    }
    if (!object.value("origin").toString().isEmpty()) rows.append({ tr("RF made from"), object.value("origin").toString() });
    for (const QJsonValue &sidecar : object.value("sidecars").toArray()) rows.append({ tr("Beside the RF file"), sidecar.toObject().value("name").toString() });
    return rows;
}

}
