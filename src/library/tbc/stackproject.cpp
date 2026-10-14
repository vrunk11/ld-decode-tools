/******************************************************************************
 * stackproject.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "stackproject.h"

#include "ldxcapture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString toStored(const QString &path, const QDir &base)
{
    if (path.isEmpty()) return path;
    return base.relativeFilePath(path);
}

QString fromStored(const QString &path, const QDir &base)
{
    if (path.isEmpty()) return path;
    if (QFileInfo(path).isAbsolute()) return QDir::cleanPath(path);
    return QDir::cleanPath(base.absoluteFilePath(path));
}

} // namespace

QString StackProject::newSourceId() const
{
    // a, b, c ... z, then s27, s28 ...
    for (qint32 n = 0;; n++) {
        const QString id = n < 26 ? QString(QChar('a' + n)) : QString("s%1").arg(n + 1);
        if (indexOfSource(id) < 0) return id;
    }
}

qint32 StackProject::indexOfSource(const QString &id) const
{
    for (qint32 i = 0; i < sources.size(); i++) {
        if (sources[i].id == id) return i;
    }
    return -1;
}

bool StackProject::load(const QString &fileName, QString &error)
{
    // A session is a file of its own, or the one an LDX file keeps
    const bool inLdx = LdxCapture::isLdxName(fileName);
    QByteArray text;
    if (inLdx) {
        if (!LdxCapture::readSession(fileName, text, error)) return false;
    } else {
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QObject::tr("Could not open %1").arg(fileName);
            return false;
        }
        text = file.readAll();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text, &parseError);
    if (document.isNull() || !document.isObject()) {
        error = QObject::tr("%1 is not a session file: %2").arg(fileName, parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    const qint32 version = root.value("version").toInt(0);
    if (version < 1 || version > CURRENT_VERSION) {
        error = QObject::tr("%1 is a session file of version %2, which this program does not read").arg(fileName).arg(version);
        return false;
    }

    const QDir base = QFileInfo(fileName).absoluteDir();
    StackProject loaded;

    for (const QJsonValue &value: root.value("sources").toArray()) {
        const QJsonObject object = value.toObject();
        StackSource source;
        source.file = fromStored(object.value("file").toString(), base);
        source.track = object.value("track").toInt(0);
        // A track of the LDX file the session is kept in
        if (inLdx && source.file.isEmpty() && source.track > 0) source.file = QDir::cleanPath(QFileInfo(fileName).absoluteFilePath());
        if (source.file.isEmpty() && source.track < 1) continue;
        source.id = object.value("id").toString();
        if (source.id.isEmpty() || loaded.indexOfSource(source.id) >= 0) source.id = loaded.newSourceId();
        source.label = object.value("label").toString();
        source.enabled = object.value("enabled").toBool(true);
        source.offset = object.value("offset").toInt(0);
        loaded.sources.append(source);
    }

    const QJsonObject stacking = root.value("stacking").toObject();
    QString modeName = stacking.value("mode").isString() ? stacking.value("mode").toString() : QString();
    if (stacking.value("mode").isDouble()) {
        loaded.stacking.mode = stacking.value("mode").toInt(-1);
    } else if (!modeName.isEmpty() && modeName != "auto") {
        static const QStringList names = { "mean", "median", "smart-mean", "smart-neighbour", "neighbour" };
        const qint32 index = names.indexOf(modeName);
        loaded.stacking.mode = index;
    }
    loaded.stacking.smartThreshold = stacking.value("smartThreshold").toInt(15);
    loaded.stacking.diffDod = stacking.value("diffDod").toBool(true);
    loaded.stacking.passthrough = stacking.value("passthrough").toBool(false);
    loaded.stacking.integrity = stacking.value("integrity").toBool(false);
    loaded.stacking.reverseFields = stacking.value("reverseFields").toBool(false);

    const QJsonObject output = root.value("output").toObject();
    loaded.outputFile = fromStored(output.value("file").toString(), base);
    loaded.metadataFormat = output.value("metadataFormat").toString("db");

    const QJsonObject view = root.value("view").toObject();
    loaded.chroma = view.value("chroma").toBool(false);
    loaded.zoom = view.value("zoom").toDouble(1.0);
    loaded.aspect = view.value("aspect").toBool(true);
    loaded.fieldView = view.value("fieldView").toBool(false);
    loaded.fieldSplit = view.value("fieldSplit").toBool(false);
    loaded.fieldIndex = qBound(0, view.value("fieldIndex").toInt(0), 1);
    loaded.selected = view.value("selected").toString();
    // (an earlier form had "stack" as the thing selected)
    loaded.showStack = view.contains("stack") ? view.value("stack").toBool(true) : (loaded.selected.isEmpty() || loaded.selected == "stack");
    if (loaded.selected == "stack") loaded.selected.clear();

    *this = loaded;
    return true;
}

bool StackProject::save(const QString &fileName, QString &error) const
{
    const QDir base = QFileInfo(fileName).absoluteDir();

    QJsonArray sourceArray;
    for (const StackSource &source: sources) {
        QJsonObject object;
        object["id"] = source.id;
        if (source.track > 0) object["track"] = source.track;
        if (!source.file.isEmpty() || source.track < 1) object["file"] = toStored(source.file, base);
        if (!source.label.isEmpty()) object["label"] = source.label;
        object["enabled"] = source.enabled;
        object["offset"] = source.offset;
        sourceArray.append(object);
    }

    static const QStringList modeNames = { "mean", "median", "smart-mean", "smart-neighbour", "neighbour" };
    QJsonObject stackingObject;
    stackingObject["mode"] = (stacking.mode >= 0 && stacking.mode < modeNames.size()) ? modeNames[stacking.mode] : QString("auto");
    stackingObject["smartThreshold"] = stacking.smartThreshold;
    stackingObject["diffDod"] = stacking.diffDod;
    stackingObject["passthrough"] = stacking.passthrough;
    stackingObject["integrity"] = stacking.integrity;
    stackingObject["reverseFields"] = stacking.reverseFields;

    QJsonObject outputObject;
    outputObject["file"] = toStored(outputFile, base);
    outputObject["metadataFormat"] = metadataFormat;

    QJsonObject viewObject;
    viewObject["chroma"] = chroma;
    viewObject["zoom"] = zoom;
    viewObject["aspect"] = aspect;
    viewObject["fieldView"] = fieldView;
    viewObject["fieldSplit"] = fieldSplit;
    viewObject["fieldIndex"] = fieldIndex;
    viewObject["selected"] = selected;
    viewObject["stack"] = showStack;

    QJsonObject root;
    root["version"] = CURRENT_VERSION;
    root["sources"] = sourceArray;
    root["stacking"] = stackingObject;
    root["output"] = outputObject;
    root["view"] = viewObject;

    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QObject::tr("Could not write %1").arg(fileName);
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        error = QObject::tr("Could not write %1").arg(fileName);
        return false;
    }
    return true;
}

QString StackSource::reference() const
{
    if (track > 0 && LdxCapture::isLdxName(file)) return LdxCapture::trackName(file, track);
    return file;
}

// A name gives a session when it is a .stack file, or an LDX file (not one of its tracks) that keeps one
bool StackProject::isSession(const QString &name)
{
    if (name.endsWith(".stack", Qt::CaseInsensitive)) return true;
    QString path;
    qint32 track;
    LdxCapture::split(name, path, track);
    return track == 0 && path.endsWith(".ldx", Qt::CaseInsensitive) && LdxCapture::hasSession(path);
}

QString StackProject::referenceFile(QString fileName)
{
    for (qint32 depth = 0; depth < 8 && isSession(fileName); depth++) {
        StackProject session;
        QString error;
        if (!session.load(fileName, error)) return fileName;
        QString next;
        for (const StackSource &source: session.sources) {
            if (source.enabled) { next = source.reference(); break; }
        }
        if (next.isEmpty()) return fileName;
        fileName = next;
    }
    // A capture of CVBS kept in an LDX file has the format of a .cvbs file
    if (LdxCapture::isLdxName(fileName) && LdxCapture::isCvbs(fileName)) return fileName + ".cvbs";
    return fileName;
}
