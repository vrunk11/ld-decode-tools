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
#include "lddecodemetadata.h"
#include "tbcfiles.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>

static QJsonArray measureArray(const QVector<float> &values);
static QVector<float> measureValues(const QJsonArray &array);

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

bool StackProject::singleCapture(const QString &sessionName, QString &captureFile, QString &error)
{
    StackProject session;
    if (!session.load(sessionName, error)) return false;
    QStringList captures;
    for (const StackSource &source: session.sources) {
        if (source.enabled) captures << source.reference();
    }
    if (captures.size() == 1) {
        captureFile = captures.first();
        return true;
    }
    error = captures.isEmpty() ? QObject::tr("No source is on in %1").arg(sessionName)
                               : QObject::tr("%1 has %2 sources on, and the metadata that are written are those of a capture: give the captures one by one (%3)")
                                     .arg(sessionName).arg(captures.size()).arg(captures.join(QStringLiteral(", ")));
    return false;
}

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
        source.followMap = object.value("followMap").toBool(true);
        if (object.contains("map")) {
            QString mapError;
            if (FrameMap::fromJson(object.value("map").toObject(), source.map, mapError)) source.hasMap = true;
            else qWarning().noquote() << "The map of the source" << source.id << "in" << fileName << "is not used:" << mapError;
        }
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
    if (stacking.contains("copyStreams")) {
        loaded.stacking.copyStreamsSet = true;
        for (const QJsonValue &value: stacking.value("copyStreams").toArray()) {
            const QJsonObject entry = value.toObject();
            loaded.stacking.copyStreams.append({ entry.value("source").toString(), entry.value("track").toInt(0) });
        }
    }

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

    // What was measured, if the session says
    for (const QJsonValue &value: root.value("measures").toArray()) {
        const QJsonObject object = value.toObject();
        StackMeasure measure;
        measure.key = object.value("key").toString();
        if (measure.key.isEmpty()) continue;
        measure.white = measureValues(object.value("white").toArray());
        measure.black = measureValues(object.value("black").toArray());
        measure.residual = measureValues(object.value("residual").toArray());
        loaded.measures.append(measure);
    }

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
        if (!source.followMap) object["followMap"] = false;
        if (source.hasMap) object["map"] = source.map.toJson();
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
    if (stacking.copyStreamsSet) {
        QJsonArray streams;
        for (const auto &entry: stacking.copyStreams) streams.append(QJsonObject{ { "source", entry.first }, { "track", entry.second } });
        stackingObject["copyStreams"] = streams;
    }

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
    if (!measures.isEmpty()) {
        QJsonArray measureList;
        for (const StackMeasure &measure: measures) {
            QJsonObject object;
            object["key"] = measure.key;
            if (!measure.white.isEmpty()) object["white"] = measureArray(measure.white);
            if (!measure.black.isEmpty()) object["black"] = measureArray(measure.black);
            if (!measure.residual.isEmpty()) object["residual"] = measureArray(measure.residual);
            measureList.append(object);
        }
        root["measures"] = measureList;
    }

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

// Whether the metadata of a capture are there: in a file, or in an LDX file
static bool metadataThere(const QString &metadataFile)
{
    if (LdxCapture::isLdxName(metadataFile)) {
        QString path;
        qint32 track;
        LdxCapture::split(metadataFile, path, track);
        return QFileInfo::exists(path);
    }
    return QFileInfo::exists(metadataFile);
}

QString StackSource::resolvedReference() const
{
    const QString name = reference();
    const bool ldx = LdxCapture::isLdxName(name);
    // A source with a map of its own is the capture seen through it: no mapped copy beside it is looked for
    if (hasMap && followMap) return name;
    if (!followMap) {
        // The capture the mapping was made from, when this is a mapping
        const QString original = ldx ? LdxCapture::originalOfMapped(name) : TbcFiles::originalFileName(name);
        if (original.isEmpty() || original == name || (!ldx && !QFileInfo::exists(original))) return name;
        return original;
    }
    QString mapped;
    if (ldx) {
        mapped = LdxCapture::mappedOfOriginal(name);
    } else {
        const QString beside = TbcFiles::mappedFileName(name);
        if (!beside.isEmpty() && QFileInfo::exists(beside)) mapped = beside;
    }
    if (mapped.isEmpty() || mapped == name) return name;
    if (!metadataThere(LdDecodeMetaData::findMetadataFile(mapped))) return name;
    // (a source whose metadata say it is mapped already is the mapping)
    LdDecodeMetaData own;
    const QString ownMetadata = LdDecodeMetaData::findMetadataFile(name);
    if (metadataThere(ownMetadata) && own.read(ownMetadata) && own.getVideoParameters().isMapped) return name;
    return mapped;
}

// A capture by what tells it from another one, and that is the same on another machine: its name (and track) and its size
static QString captureStamp(const QString &reference)
{
    QString path;
    qint32 track;
    LdxCapture::split(reference, path, track);
    const QFileInfo info(path);
    return QStringLiteral("%1#%2|%3").arg(info.fileName()).arg(track).arg(info.size());
}

QString StackSource::measureKey() const
{
    return QStringLiteral("source|") + captureStamp(resolvedReference()) + (activeMap() ? QStringLiteral("|map:") + QString::number(qHash(activeMap()->toJsonBytes())) : QString());
}

QString StackProject::stackMeasureKey(bool withResidual) const
{
    QString key = withResidual ? QStringLiteral("stack+dropouts") : QStringLiteral("stack");
    for (const StackSource &source: sources) {
        if (!source.enabled) continue;
        key += QStringLiteral("|%1@%2").arg(captureStamp(source.resolvedReference())).arg(source.offset);
        if (source.activeMap()) key += QStringLiteral("|map:") + QString::number(qHash(source.activeMap()->toJsonBytes()));
    }
    key += QStringLiteral("|%1,%2,%3,%4,%5").arg(stacking.mode).arg(stacking.smartThreshold).arg(stacking.diffDod).arg(stacking.passthrough).arg(stacking.reverseFields);
    return key;
}

const StackMeasure *StackProject::measureFor(const QString &key) const
{
    for (const StackMeasure &measure: measures) {
        if (measure.key == key) return &measure;
    }
    return nullptr;
}

static QJsonArray measureArray(const QVector<float> &values)
{
    QJsonArray array;
    for (const float value: values) {
        // (two decimals are enough for dB and for lengths; a frame with none is null)
        if (std::isfinite(value)) array.append(std::round(static_cast<double>(value) * 100.0) / 100.0);
        else array.append(QJsonValue::Null);
    }
    return array;
}

static QVector<float> measureValues(const QJsonArray &array)
{
    QVector<float> values;
    values.reserve(array.size());
    for (const QJsonValue &value: array) values.append(value.isDouble() ? static_cast<float>(value.toDouble()) : std::nanf(""));
    return values;
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
