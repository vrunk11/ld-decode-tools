/******************************************************************************
 * document.cpp
 * ldx-packer - the list of captures being made: those of an .ldx file and the ones added to them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "document.h"

#include "cvbs.h"
#include "lddecodemetadata.h"
#include "tbcfiles.h"
#include "ldxcapture.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>

namespace {

constexpr double kDefaultPcmRate = 44100.0;       // the rate of the audio of a LaserDisc (ld-container --pcm-rate)

bool isVideo(const Ldx::Track &track)
{
    return (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
}

bool isChroma(const Ldx::Track &track)
{
    return (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() == "chroma";
}

// The notes of a track are plain text; an annotations track with a namespace of its own is the data of another tool
bool isNotes(const Ldx::Track &track)
{
    return track.kind == Ldx::Kind::Annotations && !track.parameters.contains("namespace");
}

// What changed between the parameters of a track: the arguments --set key=value for what is new or different (the value as JSON), and the keys that are gone
QStringList parameterChanges(const QJsonObject &was, const QJsonObject &now, QStringList *unset)
{
    QStringList set;
    for (auto it = now.begin(); it != now.end(); ++it) {
        if (was.contains(it.key()) && was.value(it.key()) == it.value()) continue;
        QByteArray json = QJsonDocument(QJsonArray { it.value() }).toJson(QJsonDocument::Compact);
        json = json.mid(1, json.size() - 2);
        set << it.key() + QLatin1Char('=') + QString::fromUtf8(json);
    }
    if (unset) {
        for (auto it = was.begin(); it != was.end(); ++it) {
            if (!now.contains(it.key())) *unset << it.key();
        }
    }
    return set;
}

// What a person says of a capture (specification 3.1) as options of ld-container pack, add and tag: --medium=ld, --capture-number=2... What is new or different is given;
// what is gone is given empty (which takes it away) when `takeAway` (ld-container tag), and left out otherwise (a capture that is made has nothing to take away)
QStringList saidOptions(const QJsonObject &was, const QJsonObject &now, bool takeAway)
{
    QStringList options;
    for (const QString &key : { QStringLiteral("medium"), QStringLiteral("reference"), QStringLiteral("copy"), QStringLiteral("capture"), QStringLiteral("face") }) {
        if (was.value(key) == now.value(key)) continue;
        const QJsonValue value = now.value(key);
        const QString text = value.isString() ? value.toString() : (value.isDouble() ? QString::number(value.toInt()) : QString());
        const QString option = QStringLiteral("--") + (key == QLatin1String("capture") ? QStringLiteral("capture-number") : key) + QLatin1Char('=');
        if (text.isEmpty() || text == QLatin1String("0")) {
            if (takeAway) options << option;
        } else {
            options << option + text;
        }
    }
    return options;
}

// Whether a track of audio has FLAC at the rate it was coded at: the rate cannot be changed with the file as it is
bool needsCoding(const Companion &c)
{
    return c.codec == Ldx::Codec::Flac && c.kind == Ldx::Kind::Pcm && (c.parameters.value("sampleRate") != c.originalParameters.value("sampleRate") || c.declareRate);
}

// What a track holds, in a few words. The metadata: how they are kept (the whole set in one block, or the parameters and a block for each field,
// as a producer in a pipe sends them), the fields they describe and what else they hold (section 8 of the specification).
QString describe(Ldx::Reader &reader, const Ldx::Track &track, int *widescreen = nullptr)
{
    const qint64 blocks = reader.blockCount(track.number);
    QStringList parts;
    if (track.kind == Ldx::Kind::Metadata && !Ldx::isCaptureMetadata(track)) {
        parts << (track.parameters.value("namespace").toString() == QLatin1String("ldx.rfsource") ? QObject::tr("where the capture comes from (the RF file)") : track.parameters.value("namespace").toString());
        parts << QObject::tr("%n block(s)", nullptr, static_cast<int>(blocks));
    } else if (track.kind == Ldx::Kind::Metadata) {
        parts << track.parameters.value("format").toString(QStringLiteral("unknown format"));
        // The current base: the last block that has videoParameters (what came before it no longer counts)
        qint64 base = -1;
        QJsonObject root;
        Ldx::Block block;
        for (qint64 i = blocks - 1; i >= 0 && base < 0; --i) {
            if (!reader.readBlock(track.number, i, block)) continue;
            const QJsonObject object = QJsonDocument::fromJson(block.data).object();
            if (object.contains("videoParameters")) {
                base = i;
                root = object;
            }
        }
        if (base < 0) {
            parts << QObject::tr("no parameters");
        } else {
            const qint64 fields = root.contains("numberOfSequentialFields") && root.value("numberOfSequentialFields").toInt() > 0
                                      ? root.value("numberOfSequentialFields").toInt() : root.value("fields").toArray().size();
            const qint64 later = blocks - 1 - base;
            if (widescreen) *widescreen = root.value("videoParameters").toObject().contains("isWidescreen") ? (root.value("videoParameters").toObject().value("isWidescreen").toBool() ? 1 : 0) : -1;
            if (!root.value("fields").toArray().isEmpty()) parts << QObject::tr("whole set in one block");
            else parts << QObject::tr("parameters, then a block for each field");
            if (later > 0) parts << QObject::tr("%n later block(s)", nullptr, static_cast<int>(later));
            if (fields > 0) parts << QObject::tr("%n field(s)", nullptr, static_cast<int>(fields));
            if (root.contains("ldxMap")) parts << QObject::tr("with a map");
            if (root.contains("ldxDecoding")) parts << QObject::tr("with decoding parameters");
            if (root.value("fields").toArray().isEmpty() && later == 0) parts << QObject::tr("no field yet");
        }
        parts << QObject::tr("%n block(s) in all", nullptr, static_cast<int>(blocks));
    } else if (track.kind == Ldx::Kind::Pcm) {
        parts << (track.parameters.value("source").toString() == QLatin1String("efm") ? QObject::tr("digital audio decoded from EFM") : QObject::tr("analogue audio"));
        parts << QObject::tr("%1 channels").arg(track.parameters.value("channels").toInt(2));
    } else if (track.kind == Ldx::Kind::Efm) {
        parts << QObject::tr("EFM t-values");
    } else if (track.kind == Ldx::Kind::Ac3) {
        parts << QObject::tr("AC3 data");
    }
    if (track.parameters.value("role").toString() == QLatin1String("chroma")) parts << QObject::tr("chroma of the Y/C capture");
    if (track.kind != Ldx::Kind::Metadata && blocks > 0) parts << QObject::tr("%n block(s)", nullptr, static_cast<int>(blocks));
    return parts.join(QStringLiteral(" · "));
}

// Where the samples of a 16-bit stereo PCM .wav file are (its rate, the start and the length of its data)
bool readWav(const QString &path, double *rate, qint64 *start, qint64 *length, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    const QByteArray head = file.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE") return fail(QObject::tr("it is not a WAV file"));
    const auto le = [](const QByteArray &b, int at, int size) {
        quint32 v = 0;
        for (int i = size - 1; i >= 0; --i) v = (v << 8) | static_cast<quint8>(b[at + i]);
        return v;
    };
    bool format = false;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(8);
        if (chunk.size() < 8) break;
        const qint64 size = le(chunk, 4, 4);
        if (chunk.startsWith("fmt ")) {
            const QByteArray f = file.read(std::min<qint64>(size, 16));
            if (f.size() < 16) return fail(QObject::tr("its format is cut short"));
            const quint32 tag = le(f, 0, 2);
            if ((tag != 1 && tag != 0xFFFE) || le(f, 2, 2) != 2 || le(f, 14, 2) != 16) return fail(QObject::tr("only 16-bit stereo PCM audio goes in the file"));
            *rate = le(f, 4, 4);
            format = true;
            file.seek(file.pos() + (size - f.size()) + (size & 1));
        } else if (chunk.startsWith("data")) {
            if (!format) return fail(QObject::tr("its data come before its format"));
            *start = file.pos();
            *length = std::min<qint64>(size, file.size() - file.pos());
            return *length > 0 ? true : fail(QObject::tr("it holds no audio"));
        } else {
            file.seek(file.pos() + size + (size & 1));
        }
    }
    return fail(QObject::tr("it holds no audio"));
}

}

QString codecArgument(Codec codec)
{
    switch (codec) {
    case Codec::Auto: return QStringLiteral("auto");
    case Codec::Flac: return QStringLiteral("flac");
    case Codec::Ffv1: return QStringLiteral("ffv1");
    case Codec::Raw:
    case Codec::AsIs: break;
    }
    return QStringLiteral("raw");
}

bool isSamples(Ldx::Kind kind)
{
    return kind == Ldx::Kind::Tbc || kind == Ldx::Kind::Rf || kind == Ldx::Kind::Pcm || kind == Ldx::Kind::Cvbs;
}

// As ld-container recompress does: a codec that is for other tracks leaves this one as it is (raw is for all)
Ldx::Codec codecFor(Codec asked, Ldx::Kind kind, int depth, Ldx::Codec current)
{
    if (asked == Codec::AsIs) return current;
    const bool sixteen = (kind == Ldx::Kind::Tbc || kind == Ldx::Kind::Rf || kind == Ldx::Kind::Pcm) && depth == 16;
    switch (asked) {
    case Codec::Raw: return Ldx::Codec::Raw;
    case Codec::Auto: return sixteen ? Ldx::Codec::Flac : (kind == Ldx::Kind::Cvbs ? Ldx::Codec::Ffv1 : Ldx::Codec::Raw);
    case Codec::Flac: return sixteen ? Ldx::Codec::Flac : current;
    case Codec::Ffv1: return kind == Ldx::Kind::Cvbs ? Ldx::Codec::Ffv1 : current;
    case Codec::AsIs: break;
    }
    return current;
}

bool Document::open(const QString &path, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    Ldx::Reader reader;
    QString reason;
    if (!reader.open(path, reason)) return fail(reason);
    if (reader.profile() < 1) return fail(tr("it is not an LDX file"));
    if (!reader.buildIndex(reason)) return fail(reason);

    const QVector<Ldx::Track> tracks = reader.tracks();
    std::vector<Entry> entries;
    for (const Ldx::Track &track : tracks) {
        if (!isVideo(track)) continue;
        Entry e;
        e.fromArchive = true;
        e.video = track;
        e.aspect = e.originalAspect = track.parameters.value("displayAspect").toString();
        e.videoParameters = track.parameters;
        {
            QByteArray rf;
            QString rfError;
            if (LdxCapture::readRfSource(path, static_cast<qint32>(track.number), rf, rfError)) e.rfSource = e.originalRfSource = QJsonDocument::fromJson(rf).toJson(QJsonDocument::Compact);
        }
        e.name = track.name;
        e.blocks = reader.blockCount(track.number);
        e.seconds = e.blocks > 0 ? static_cast<double>(reader.blockTimeNs(track.number, e.blocks - 1)) / 1e9 : 0.0;
        entries.push_back(e);
    }
    if (entries.empty()) return fail(tr("it holds no video track"));

    // The other tracks go with the video track they are about (the first one when they do not say)
    const auto entryOf = [&entries](const Ldx::Track &track) -> Entry & {
        const quint32 about = static_cast<quint32>(track.parameters.value("track").toInt(0));
        for (Entry &e : entries) {
            if (e.video.number == about) return e;
        }
        return entries.front();
    };
    for (const Ldx::Track &track : tracks) {
        if (isVideo(track)) continue;
        Entry &e = entryOf(track);
        if (isChroma(track)) e.chroma = track;      // (and it is a line of the tracks that go with the capture, like the others)
        if (isNotes(track) && e.notesTrack == 0) {
            e.notesTrack = track.number;
            QByteArray text;
            Ldx::Block block;
            for (qint64 i = 0; i < reader.blockCount(track.number); ++i) {
                if (reader.readBlock(track.number, i, block)) text += block.data;
            }
            e.notes = e.originalNotes = QString::fromUtf8(text);
            continue;
        }
        Companion c;
        c.number = track.number;
        c.chroma = isChroma(track);
        c.kind = track.kind;
        c.codec = track.codec;
        c.depth = track.depth;
        c.name = track.name;
        c.sampleRate = track.parameters.value("sampleRate").toDouble();
        int wide = -1;
        c.detail = describe(reader, track, &wide);
        if (Ldx::isCaptureMetadata(track) && e.metadataWide < 0) e.metadataWide = wide;
        c.originalName = track.name;
        c.parameters = c.originalParameters = track.parameters;
        c.headerRate = track.headerRate;
        e.companions.push_back(c);
    }

    m_entries = std::move(entries);
    m_archive = path;
    m_archiveSize = QFileInfo(path).size();
    m_hasIndex = reader.cuesPosition() != 0;
    m_stream = !m_hasIndex;
    m_history = reader.history();
    // The cover of the file, if it has one
    m_cover.clear();
    m_coverChange = CoverChange::Keep;
    m_coverNew.clear();
    m_coverSource.clear();
    m_coverLink.clear();
    for (const Ldx::Reader::Attachment &attachment : reader.attachments()) {
        const QString lower = attachment.name.toLower();
        if (lower == QLatin1String("cover.png") || lower == QLatin1String("cover.jpg") || lower == QLatin1String("cover.jpeg")) m_cover = attachment.data;
        else if (lower == QLatin1String("cover.url")) m_coverSource = QString::fromUtf8(attachment.data).trimmed();
        else if (lower == QLatin1String("link.url")) m_coverLink = QString::fromUtf8(attachment.data).trimmed();
    }
    m_coverSourceOriginal = m_coverSource;
    m_coverLinkOriginal = m_coverLink;
    // The session of the file: one line for each source (a track of the file, or a link)
    m_hasSession = false;
    m_sessionLines.clear();
    m_sessionChange = SessionChange::Keep;
    m_sessionFile.clear();
    QByteArray sessionData;
    if (reader.attachment(QStringLiteral("session.stack"), sessionData)) {
        m_hasSession = true;
        for (const QJsonValue &value : QJsonDocument::fromJson(sessionData).object().value("sources").toArray()) {
            const QJsonObject source = value.toObject();
            const int track = source.value("track").toInt(0);
            const QString file = source.value("file").toString();
            QString line;
            if (file.isEmpty()) {
                QString name;
                for (const Ldx::Track &t : tracks) {
                    if (static_cast<int>(t.number) == track) name = t.name;
                }
                line = tr("track %1").arg(track) + (name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name));
            } else {
                line = tr("link: %1").arg(file) + (track > 0 ? QStringLiteral("#%1").arg(track) : QString());
            }
            const QString id = source.value("id").toString();
            m_sessionLines << (id.isEmpty() ? line : id + QStringLiteral(": ") + line);
        }
    }
    m_codec = Codec::AsIs;
    m_touched = false;
    emit changed();
    return true;
}

bool Document::addArchive(const QString &path, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    const QFileInfo info(path);
    if (!info.isFile()) return fail(tr("the file does not exist"));
    if (info.absoluteFilePath() == QFileInfo(m_archive).absoluteFilePath()) return fail(tr("it is the file that is opened"));
    // The list needs a file or a capture to be packed to put them in
    const bool base = !m_archive.isEmpty() || std::any_of(m_entries.begin(), m_entries.end(), [](const Entry &e) { return !e.removed && e.ldxFile.isEmpty(); });
    if (!base) return fail(tr("there is nothing to put them in: open the file instead"));
    Document other;
    QString why;
    if (!other.open(path, &why)) return fail(why);
    for (int i = 0; i < other.count(); ++i) {
        Entry e = other.at(i);
        e.fromArchive = false;
        e.ldxFile = info.absoluteFilePath();
        e.ldxTrack = e.video.number;
        e.capture = info.absoluteFilePath();
        e.name = e.video.name;
        e.removed = false;
        e.aspect = e.originalAspect;     // (as the file has it; it is kept by the merge, and changed with a step if it is changed)
        e.rfSource = e.originalRfSource;
        e.newMetadata.clear();
        e.forceDisplay = false;
        m_entries.push_back(e);
    }
    emit changed();
    return true;
}

bool Document::addCapture(const QString &path, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    const QFileInfo info(path);
    if (!info.exists()) return fail(tr("the file does not exist"));
    const bool tbc = info.suffix().compare(QLatin1String("tbc"), Qt::CaseInsensitive) == 0;
    if (!tbc && !Cvbs::isDataFileName(path)) return fail(tr("it is not a .tbc or a CVBS capture"));
    if (!tbc && Cvbs::channel(path) == Cvbs::Channel::Chroma) return fail(tr("it is the chroma file of a Y/C capture: add its luma file, which takes it"));
    if (LdDecodeMetaData::findMetadataFile(path).isEmpty()) return fail(tr("its metadata (.tbc.db or .tbc.json) was not found beside it"));

    Entry e;
    e.name = info.fileName();
    e.capture = info.absoluteFilePath();
    const QString base = info.absolutePath() + QLatin1Char('/') + info.completeBaseName();
    const struct { const char *suffix; Ldx::Kind kind; int depth; double rate; } beside[] = {
        { ".pcm", Ldx::Kind::Pcm, 16, kDefaultPcmRate }, { ".efm", Ldx::Kind::Efm, 8, 0.0 }, { ".ac3", Ldx::Kind::Ac3, 8, 0.0 }
    };
    for (const auto &candidate : beside) {
        const QString file = base + QLatin1String(candidate.suffix);
        if (!QFileInfo::exists(file)) continue;
        Companion c;
        c.kind = candidate.kind;
        c.depth = candidate.depth;
        c.sampleRate = candidate.rate;
        c.file = file;
        c.name = QFileInfo(file).fileName();
        e.companions.push_back(c);
    }
    // The chroma file that is beside it (a Y/C capture): ld-container takes it with the capture
    {
        QString chromaFile;
        if (tbc) {
            const QString candidate = TbcFiles::chromaFileName(path);
            if (!candidate.isEmpty() && candidate != path && !TbcFiles::isChromaFile(path)) chromaFile = candidate;
        } else if (Cvbs::channel(path) == Cvbs::Channel::Luma) {
            chromaFile = Cvbs::pairedDataFileName(path);
        }
        if (!chromaFile.isEmpty() && QFileInfo::exists(chromaFile)) {
            Companion c;
            c.chroma = true;
            c.beside = true;
            c.kind = tbc ? Ldx::Kind::Tbc : Ldx::Kind::Cvbs;
            c.depth = tbc ? 16 : 10;
            c.file = chromaFile;
            c.name = QFileInfo(chromaFile).fileName();
            e.companions.push_back(c);
            e.besideChroma = true;
        }
    }
    m_entries.push_back(e);
    emit changed();
    return true;
}

void Document::clear()
{
    m_entries.clear();
    m_archive.clear();
    m_archiveSize = 0;
    m_hasIndex = true;
    m_stream = false;
    m_history = QJsonArray();
    m_hasSession = false;
    m_sessionLines.clear();
    m_sessionChange = SessionChange::Keep;
    m_sessionFile.clear();
    m_cover.clear();
    m_coverNew.clear();
    m_coverChange = CoverChange::Keep;
    m_codec = Codec::Raw;
    m_touched = false;
    emit changed();
    m_coverSource.clear();
    m_coverSourceOriginal.clear();
    m_coverLink.clear();
    m_coverLinkOriginal.clear();
}

void Document::setStream(bool stream)
{
    if (stream == m_stream) return;
    m_stream = stream;
    emit changed();
}

void Document::setCodec(Codec codec)
{
    if (codec == m_codec) return;
    m_codec = codec;
    emit changed();
}

void Document::remove(int i)
{
    Entry &e = m_entries[static_cast<size_t>(i)];
    if (e.fromArchive) {
        e.removed = true;
    } else {
        m_entries.erase(m_entries.begin() + i);
    }
    emit changed();
}

void Document::restore(int i)
{
    m_entries[static_cast<size_t>(i)].removed = false;
    emit changed();
}

bool Document::canMove(int i, int delta) const
{
    const int j = i + delta;
    return i >= 0 && j >= 0 && i < count() && j < count();
}

void Document::move(int i, int delta)
{
    if (!canMove(i, delta)) return;
    std::swap(m_entries[static_cast<size_t>(i)], m_entries[static_cast<size_t>(i + delta)]);
    emit changed();
}

bool Document::wavRate(const QString &path, double *rate, QString *error)
{
    qint64 start = 0, length = 0;
    return readWav(path, rate, &start, &length, error);
}

bool Document::addTrack(int i, const QString &file, Ldx::Kind kind, const QString &name, double sampleRate, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (!QFileInfo(file).isFile()) return fail(tr("the file does not exist"));
    Companion c;
    c.kind = kind;
    switch (kind) {
    case Ldx::Kind::Pcm: {
        c.depth = 16;
        c.sampleRate = sampleRate > 0.0 ? sampleRate : kDefaultPcmRate;
        // A .wav is read into raw audio when the file is written, at its own rate
        if (QFileInfo(file).suffix().compare(QLatin1String("wav"), Qt::CaseInsensitive) == 0) {
            QString why;
            if (!wavRate(file, &c.sampleRate, &why)) return fail(why);
        }
        break;
    }
    case Ldx::Kind::Efm:
    case Ldx::Kind::Ac3:
        c.depth = 8;
        break;
    case Ldx::Kind::Annotations:
        c.depth = 0;
        break;
    default:
        return fail(tr("only audio, EFM, AC3 and notes are added from a file"));
    }
    c.file = file;
    c.name = name.isEmpty() ? QFileInfo(file).fileName() : name;
    if (kind == Ldx::Kind::Pcm) c.parameters["sampleRate"] = c.sampleRate;
    edit(i).companions.push_back(c);
    touched();
    return true;
}

bool Document::addChroma(int i, const QString &file, const QString &name, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    const QFileInfo info(file);
    if (!info.isFile()) return fail(tr("the file does not exist"));
    const QString suffix = info.suffix().toLower();
    const bool cvbs = suffix == QLatin1String("cvbsc");
    if (suffix != QLatin1String("tbc") && suffix != QLatin1String("tbcc") && !cvbs) return fail(tr("the chroma of a Y/C capture is a .tbc (_chroma.tbc), .tbcc or .cvbsc file"));
    Entry &e = edit(i);
    for (const Companion &c : e.companions) {
        if (c.chroma && !c.removed) return fail(tr("the capture has a chroma already: take it out first"));
    }
    const bool lumaIsCvbs = e.fromArchive ? e.video.kind == Ldx::Kind::Cvbs : Cvbs::isDataFileName(e.capture);
    if (lumaIsCvbs != cvbs) return fail(lumaIsCvbs ? tr("the luma of the capture is CVBS: its chroma is a .cvbsc file") : tr("the luma of the capture is TBC: its chroma is a .tbc file"));
    Companion c;
    c.chroma = true;
    c.kind = cvbs ? Ldx::Kind::Cvbs : Ldx::Kind::Tbc;
    c.depth = cvbs ? 10 : 16;
    c.file = info.absoluteFilePath();
    c.name = name.isEmpty() ? info.fileName() : name;
    e.companions.push_back(c);
    touched();
    return true;
}

bool Document::addCompanion(int i, const QString &file, QString *error)
{
    Companion c;
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == QLatin1String("pcm")) {
        c.kind = Ldx::Kind::Pcm;
        c.depth = 16;
        c.sampleRate = kDefaultPcmRate;
    } else if (suffix == QLatin1String("wav")) {
        qint64 start = 0, length = 0;
        QString why;
        c.kind = Ldx::Kind::Pcm;
        c.depth = 16;
        if (!readWav(file, &c.sampleRate, &start, &length, &why)) {
            if (error) *error = why;
            return false;
        }
    } else if (suffix == QLatin1String("efm")) {
        c.kind = Ldx::Kind::Efm;
        c.depth = 8;
    } else if (suffix == QLatin1String("ac3")) {
        c.kind = Ldx::Kind::Ac3;
        c.depth = 8;
    } else {
        if (error) {
            *error = (suffix == QLatin1String("db") || suffix == QLatin1String("json"))
                         ? tr("the metadata of a capture are read from the file beside its .tbc when it is added: they cannot be put in one that is in the file")
                         : tr("only .pcm or .wav (audio), .efm and .ac3 files go with a capture");
        }
        return false;
    }
    c.file = file;
    c.name = QFileInfo(file).fileName();
    edit(i).companions.push_back(c);
    touched();
    return true;
}

void Document::removeCompanion(int i, int which)
{
    std::vector<Companion> &list = edit(i).companions;
    if (which < 0 || which >= static_cast<int>(list.size())) return;
    if (list[static_cast<size_t>(which)].isNew()) {
        list.erase(list.begin() + which);
    } else {
        list[static_cast<size_t>(which)].removed = !list[static_cast<size_t>(which)].removed;
    }
    touched();
}

bool Document::readMetadataFile(const QString &path, QJsonObject *root, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    LdDecodeMetaData metadata;
    if (!metadata.read(path)) return fail(QObject::tr("%1 is not metadata of ld-decode (.json or .db)").arg(QFileInfo(path).fileName()));
    QTemporaryDir temporary;
    const QString json = temporary.filePath(QStringLiteral("metadata.json"));
    QFile file(json);
    if (!metadata.write(json) || !file.open(QIODevice::ReadOnly)) return fail(QObject::tr("the metadata of %1 cannot be read").arg(QFileInfo(path).fileName()));
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!document.isObject()) return fail(parse.errorString());
    if (root) *root = document.object();
    return true;
}

bool Document::metadataOf(int i, QJsonObject *root, QString *error) const
{
    const Entry &e = at(i);
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (!e.newMetadata.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(e.newMetadata);
        if (root) *root = document.object();
        return document.isObject();
    }
    if (!e.fromArchive && e.ldxFile.isEmpty()) {
        const QString file = LdDecodeMetaData::findMetadataFile(e.capture);
        if (file.isEmpty()) return fail(tr("its metadata were not found beside it"));
        return readMetadataFile(file, root, error);
    }
    LdxCapture capture;
    QByteArray json;
    QString reason;
    if (!capture.open(QStringLiteral("%1#%2").arg(e.ldxFile.isEmpty() ? m_archive : e.ldxFile).arg(e.ldxFile.isEmpty() ? e.video.number : e.ldxTrack), reason) || !capture.readMetadata(json, reason)) return fail(reason);
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return fail(tr("the metadata are not JSON"));
    if (root) *root = document.object();
    return true;
}

void Document::setMetadata(int i, const QJsonObject &root)
{
    edit(i).newMetadata = QJsonDocument(root).toJson(QJsonDocument::Compact);
    touched();
}

void Document::clearMetadata(int i)
{
    edit(i).newMetadata.clear();
    touched();
}

bool Document::setSession(const QString &path, bool embed, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
    if (session.value("sources").toArray().isEmpty()) {
        if (error) *error = tr("it is not a session with sources (a .stack file)");
        return false;
    }
    m_sessionChange = SessionChange::Set;
    m_sessionFile = QFileInfo(path).absoluteFilePath();
    m_sessionEmbed = embed;
    touched();
    return true;
}

void Document::setCover(const QByteArray &image, const QString &suffix)
{
    m_coverNew = image;
    m_coverNewSuffix = suffix.toLower() == QLatin1String("jpg") ? QStringLiteral("jpg") : QStringLiteral("png");
    m_coverChange = CoverChange::Set;
    touched();
}

void Document::removeCover()
{
    m_coverNew.clear();
    // (a file that has no cover has nothing to take out)
    m_coverChange = m_cover.isEmpty() ? CoverChange::Keep : CoverChange::Remove;
    touched();
}

void Document::keepCover()
{
    m_coverNew.clear();
    m_coverChange = CoverChange::Keep;
    touched();
}

void Document::setCoverSource(const QString &source)
{
    m_coverSource = source.trimmed();
    touched();
}

void Document::setCoverLink(const QString &link)
{
    m_coverLink = link.trimmed();
    touched();
}

void Document::undoCover()
{
    m_coverNew.clear();
    m_coverChange = CoverChange::Keep;
    m_coverSource = m_coverSourceOriginal;
    m_coverLink = m_coverLinkOriginal;
    touched();
}

void Document::removeSession()
{
    m_sessionChange = SessionChange::Remove;
    m_sessionFile.clear();
    touched();
}

void Document::keepSession()
{
    m_sessionChange = SessionChange::Keep;
    m_sessionFile.clear();
    touched();
}

void Document::touched()
{
    m_touched = true;
    emit changed();
}

bool Document::needsRecode(int i) const
{
    const Entry &e = at(i);
    if (!e.fromArchive || m_codec == Codec::AsIs) return false;
    const auto differs = [this](const Ldx::Track &track) {
        return isSamples(track.kind) && codecFor(m_codec, track.kind, track.depth, track.codec) != track.codec;
    };
    if (differs(e.video) || (e.chroma.number != 0 && differs(e.chroma))) return true;
    for (const Companion &c : e.companions) {
        if (!c.isNew() && !c.removed && isSamples(c.kind) && codecFor(m_codec, c.kind, c.depth, c.codec) != c.codec) return true;
    }
    return false;
}

bool Document::companionsEdited(const Entry &e) const
{
    if (e.notes != e.originalNotes || !e.newMetadata.isEmpty() || e.aspect != e.originalAspect || e.rfSource != e.originalRfSource || e.forceDisplay || (e.fromArchive && (e.name != e.video.name || e.videoParameters != e.video.parameters))) return true;
    return std::any_of(e.companions.begin(), e.companions.end(), [](const Companion &c) { return c.isNew() || c.removed || (c.name != c.originalName) || (c.parameters != c.originalParameters) || c.declareRate; });
}

EntryState Document::state(int i) const
{
    const Entry &e = at(i);
    if (e.removed) return EntryState::Removed;
    if (!e.fromArchive) return EntryState::New;
    if (needsRecode(i)) return EntryState::Recode;
    return companionsEdited(e) ? EntryState::Edited : EntryState::Kept;
}

Summary Document::summary() const
{
    Summary s;
    for (int i = 0; i < count(); ++i) {
        switch (state(i)) {
        case EntryState::Kept: ++s.kept; break;
        case EntryState::Edited: ++s.kept; ++s.edited; break;
        case EntryState::Recode: ++s.kept; ++s.recoded; break;
        case EntryState::New: ++s.added; break;
        case EntryState::Removed: ++s.removed; break;
        }
    }
    return s;
}

bool Document::orderChanged() const
{
    // The order the file would have with no move: the captures it has, by their tracks, then the ones that are added, in the order of the list
    std::vector<int> natural;
    std::vector<int> listed;
    for (int i = 0; i < count(); ++i) {
        if (!at(i).removed) listed.push_back(i);
    }
    natural = listed;
    std::stable_sort(natural.begin(), natural.end(), [this](int a, int b) {
        const Entry &x = at(a);
        const Entry &y = at(b);
        if (x.fromArchive != y.fromArchive) return x.fromArchive;
        if (!x.fromArchive && x.ldxFile.isEmpty() != y.ldxFile.isEmpty()) return x.ldxFile.isEmpty();     // (the captures taken from a file are put in after the ones that are packed)
        return x.fromArchive && x.video.number < y.video.number;
    });
    return natural != listed;
}

QString Document::displayProblem(int i, QString *fix) const
{
    const Entry &e = at(i);
    // Only for a shape that was chosen (4:3 or 16:9): not set is 1:1 and stays so, whatever the metadata say
    if (!e.fromArchive || e.removed || e.originalAspect.isEmpty() || e.aspect != e.originalAspect || e.forceDisplay) return QString();
    Ldx::Track video = e.video;
    video.parameters["displayAspect"] = e.originalAspect;
    // The metadata are what ld-analyse reads: the shape that makes all agree is theirs when they say, else the one of the track
    const QString target = e.metadataWide >= 0 ? (e.metadataWide == 1 ? QStringLiteral("16:9") : QStringLiteral("4:3")) : e.originalAspect;
    const int width = e.video.headerPixelWidth > 0 ? static_cast<int>(e.video.headerPixelWidth) : e.video.parameters.value("fieldWidth").toInt();
    const int height = e.video.headerPixelHeight > 0 ? static_cast<int>(e.video.headerPixelHeight) : e.video.parameters.value("fieldHeight").toInt();
    int expectedWidth = 0, expectedHeight = 0;
    if (Ldx::displayShape(video, width, height, &expectedWidth, &expectedHeight)
        && (static_cast<int>(e.video.headerDisplayWidth) != expectedWidth || static_cast<int>(e.video.headerDisplayHeight) != expectedHeight)) {
        if (fix) *fix = target;
        return e.video.headerDisplayWidth == 0
                   ? tr("The header of the track says no shape, and the track says %1: a player shows the image with square pixels.").arg(e.originalAspect)
                   : tr("The header of the track says %1×%2, and the track says %3 (%4×%5): a player shows the wrong shape.")
                         .arg(e.video.headerDisplayWidth).arg(e.video.headerDisplayHeight).arg(e.originalAspect).arg(expectedWidth).arg(expectedHeight);
    }
    // The metadata say whether the capture is widescreen (ld-analyse reads that): the shape follows
    if (e.metadataWide >= 0 && (e.metadataWide == 1) != (e.originalAspect == QLatin1String("16:9"))) {
        if (fix) *fix = target;
        return tr("The track says %1, and the metadata say the capture is %2: a player shows the other shape than ld-analyse.")
            .arg(e.originalAspect, e.metadataWide == 1 ? tr("widescreen (16:9)") : tr("not widescreen (4:3)"));
    }
    return QString();
}

bool Document::canUpdate() const
{
    // Only what the file takes in its place, with no copy of it and no part files: new metadata (a block added at the end, section 8.3), the shape
    // and the name of a capture (its track in the list of tracks, which has room to change), and notes where there were none (a track of annotations)
    if (m_archive.isEmpty() || !m_hasIndex || m_stream || m_sessionChange != SessionChange::Keep || orderChanged()) return false;
    // The cover, put in or taken out at the end of the file (a file that has none has nothing to take out)
    const bool coverChanged = m_coverChange == CoverChange::Set || (m_coverChange == CoverChange::Remove && !m_cover.isEmpty()) || coverReferenceChanged();
    bool any = coverChanged;
    for (int i = 0; i < count(); ++i) {
        const Entry &e = at(i);
        const bool notesAdded = e.notes != e.originalNotes && e.notesTrack == 0 && e.originalNotes.isEmpty() && !e.notes.isEmpty();
        const bool otherEdits = e.removed || !e.fromArchive || needsRecode(i) || (e.notes != e.originalNotes && !notesAdded)
                                || std::any_of(e.companions.begin(), e.companions.end(), [](const Companion &c) { return c.isNew() || c.removed || needsCoding(c); });
        if (otherEdits) return false;
        const bool tracksChanged = e.videoParameters != e.video.parameters
                                   || std::any_of(e.companions.begin(), e.companions.end(), [](const Companion &c) { return c.name != c.originalName || c.parameters != c.originalParameters || c.declareRate; });
        any = any || !e.newMetadata.isEmpty() || e.aspect != e.originalAspect || e.rfSource != e.originalRfSource || e.forceDisplay || e.name != e.video.name || notesAdded || tracksChanged || coverChanged;
    }
    return any;
}

bool Document::isChanged() const
{
    if (m_archive.isEmpty()) return count() > 0;
    if (orderChanged()) return true;
    // (an opened file is written as it is unless the list says another way: with an index it is a file, with none a stream)
    if (m_stream == m_hasIndex || m_sessionChange != SessionChange::Keep || coverEdited()) return true;
    for (int i = 0; i < count(); ++i) {
        if (state(i) != EntryState::Kept) return true;
    }
    return false;
}

Plan Document::toPlan(const QString &output, bool verify) const
{
    Plan plan;
    plan.output = output;
    const QString program = QStringLiteral("ld-container");
    // (in the folder of the output: the file that is done is only renamed, whatever the disk)
    plan.directory = std::make_shared<QTemporaryDir>(QFileInfo(output).absolutePath() + QStringLiteral("/.ldx-packer-XXXXXX"));
    if (!plan.directory->isValid() || !isChanged()) return plan;

    QString current = m_archive;
    int made = 0;
    const auto nextFile = [&plan, &made] { return plan.directory->filePath(QStringLiteral("part%1.ldx").arg(++made)); };
    // What a step makes replaces the file it was made from, which goes when the step is done
    const auto toward = [&current](const QString &made_) {
        const QString was = current;
        current = made_;
        return was;
    };
    const QString opened = m_archive;
    const auto cleanup = [opened](const QString &was) {
        return [was, opened](const QString &) {
            if (was != opened) QFile::remove(was);
        };
    };
    const auto program_step = [&](const QString &description, const QStringList &arguments, std::function<void(const QString &)> after = nullptr) {
        Step step;
        step.description = description;
        step.program = program;
        step.arguments = [arguments] { return arguments; };
        step.after = after;
        plan.steps.append(step);
    };
    // The options of ld-container cover for what was changed about the cover: the picture, where it is kept outside the file, the link
    int coverFiles = 0;
    const auto coverOptions = [&]() {
        QStringList options;
        if (m_coverChange == CoverChange::Set && !m_coverNew.isEmpty()) {
            const QString picture = plan.directory->filePath(QStringLiteral("cover%1.%2").arg(++coverFiles).arg(m_coverNewSuffix));
            QFile file(picture);
            if (file.open(QIODevice::WriteOnly)) file.write(m_coverNew);
            options << QStringLiteral("--set") << picture;
        } else if (m_coverChange == CoverChange::Remove && !m_cover.isEmpty()) {
            options << QStringLiteral("--remove");
        }
        if (m_coverSource != m_coverSourceOriginal) {
            if (m_coverSource.isEmpty()) options << QStringLiteral("--remove-source");
            else options << QStringLiteral("--source") << m_coverSource;
        }
        if (m_coverLink != m_coverLinkOriginal) {
            if (m_coverLink.isEmpty()) options << QStringLiteral("--remove-link");
            else options << QStringLiteral("--link") << m_coverLink;
        }
        return options;
    };

    // Only new metadata, over the file that was opened: they are a block added at the end of the file (section 8.3), which is done in its place,
    // with no copy of the file and no part files (what ld-container metadata does with no output)
    const bool overOpened = !m_archive.isEmpty() && QFileInfo(output).absoluteFilePath() == QFileInfo(m_archive).absoluteFilePath();
    if (overOpened && canUpdate()) {
        int files = 0;
        for (const Entry &e : m_entries) {
            const QString number = QString::number(e.video.number);
            if (!e.newMetadata.isEmpty()) {
                const QString file = plan.directory->filePath(QStringLiteral("metadata%1.json").arg(++files));
                QFile json(file);
                if (json.open(QIODevice::WriteOnly)) json.write(e.newMetadata);
                program_step(tr("Writing the metadata of %1 in the file").arg(e.name), { QStringLiteral("metadata"), m_archive, QStringLiteral("--id"), number, QStringLiteral("--input"), file });
            }
            if (e.aspect != e.originalAspect || e.forceDisplay) {
                program_step(tr("Showing %1 as %2 in the file").arg(e.name, e.aspect.isEmpty() ? tr("1:1") : e.aspect),
                             { QStringLiteral("display"), m_archive, QStringLiteral("--id"), number, QStringLiteral("--aspect"), e.aspect.isEmpty() ? QStringLiteral("none") : e.aspect });
            }
            if (e.rfSource != e.originalRfSource) {
                QStringList arguments { QStringLiteral("rfsource"), m_archive, QStringLiteral("--id"), number };
                if (e.rfSource.isEmpty()) {
                    arguments << QStringLiteral("--remove");
                } else {
                    const QString file = plan.directory->filePath(QStringLiteral("rfsource%1.json").arg(++files));
                    QFile json(file);
                    if (json.open(QIODevice::WriteOnly)) json.write(e.rfSource);
                    arguments << QStringLiteral("--set") << file;
                }
                program_step(e.rfSource.isEmpty() ? tr("Taking out where %1 comes from, in the file").arg(e.name) : tr("Saying where %1 comes from, in the file").arg(e.name), arguments);
            }
            if (e.name != e.video.name) {
                program_step(tr("Naming track %1 %2 in the file").arg(number, e.name), { QStringLiteral("rename"), m_archive, QStringLiteral("--id"), number, QStringLiteral("--name"), e.name });
            }
            // The name and the parameters of the tracks that were changed (the sample rate of the audio, the system of the video...)
            const auto settrack = [&](const QString &what, const QString &id, const QString &name, bool setName, const QJsonObject &was, const QJsonObject &now, bool declare = false) {
                QStringList unset;
                QStringList set = parameterChanges(was, now, &unset);
                if (declare && now.contains("sampleRate") && std::none_of(set.begin(), set.end(), [](const QString &item) { return item.startsWith(QStringLiteral("sampleRate=")); })) {
                    set << QStringLiteral("sampleRate=") + QString::number(now.value("sampleRate").toDouble(), 'f', 0);
                }
                if (!setName && set.isEmpty() && unset.isEmpty()) return;
                QStringList command { QStringLiteral("settrack"), m_archive, QStringLiteral("--id"), id };
                if (setName) command << QStringLiteral("--name") << name;
                for (const QString &item : set) command << QStringLiteral("--set") << item;
                for (const QString &key : unset) command << QStringLiteral("--unset") << key;
                program_step(tr("Changing %1 in the file").arg(what), command);
            };
            settrack(e.name, number, QString(), false, e.video.parameters, e.videoParameters);
            for (const Companion &c : e.companions) {
                if (c.isNew() || c.removed) continue;
                settrack(c.name, QString::number(c.number), c.name, c.name != c.originalName, c.originalParameters, c.parameters, c.declareRate);
            }
            if (e.notes != e.originalNotes) {
                const QString file = plan.directory->filePath(QStringLiteral("notes%1.txt").arg(++files));
                QFile text(file);
                if (text.open(QIODevice::WriteOnly)) text.write(e.notes.toUtf8());
                program_step(tr("Adding the notes of %1 in the file").arg(e.name),
                             { QStringLiteral("addtrack"), m_archive, QStringLiteral("--kind"), QStringLiteral("annotations"), QStringLiteral("--input"), file, QStringLiteral("--about"), number,
                               QStringLiteral("--name"), tr("notes"), QStringLiteral("--update") });
            }
        }
        // The cover: the attachments written at the end of the file (ld-container cover with no output)
        const QStringList cover = coverOptions();
        if (!cover.isEmpty()) program_step(tr("Changing the cover of the file"), QStringList { QStringLiteral("cover"), m_archive } + cover);
        if (verify) program_step(tr("Checking %1").arg(QFileInfo(output).fileName()), { QStringLiteral("verify"), output });
        return plan;
    }

    // Tracks that go: a capture that is removed, with what is about it that the removal of the video track does not take (the audio, the
    // metadata and the notes it does); the files taken off a capture that stays
    QList<QPair<quint32, QString>> gone;
    for (const Entry &e : m_entries) {
        if (!e.fromArchive) continue;
        if (e.removed) {
            gone.append({ e.video.number, e.name });
            for (const Companion &c : e.companions) {
                if (!c.isNew() && (c.kind == Ldx::Kind::Efm || c.kind == Ldx::Kind::Ac3)) gone.append({ c.number, c.name });
            }
            continue;
        }
        for (const Companion &c : e.companions) {
            if (c.removed && !c.isNew()) gone.append({ c.number, c.name });
        }
        if (e.notes != e.originalNotes && e.notesTrack != 0) gone.append({ e.notesTrack, tr("the notes of %1").arg(e.name) });
    }
    for (const auto &track : gone) {
        const QString from = current;
        const QString to = nextFile();
        program_step(tr("Removing track %1 (%2)").arg(track.first).arg(track.second), { QStringLiteral("remove"), from, QStringLiteral("--id"), QString::number(track.first), QStringLiteral("-o"), to },
                     cleanup(toward(to)));
    }

    // The tracks of samples that stay, kept as the list says
    bool recode = false;
    for (int i = 0; i < count(); ++i) {
        recode = recode || (!at(i).removed && needsRecode(i));
    }
    if (recode) {
        const QString from = current;
        const QString to = nextFile();
        program_step(tr("Keeping the samples as %1").arg(codecArgument(m_codec)),
                     { QStringLiteral("recompress"), from, QStringLiteral("--codec"), codecArgument(m_codec), QStringLiteral("-o"), to }, cleanup(toward(to)));
    }

    // Writes the notes of a capture to a file, for addtrack
    int notesFiles = 0;
    const auto notesFile = [&plan, &notesFiles](const QString &text) {
        const QString path = plan.directory->filePath(QStringLiteral("notes%1.txt").arg(++notesFiles));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(text.toUtf8());
        return path;
    };
    // The file with the samples of an audio file: the .wav is read into a raw one first (what ld-container takes)
    int audioFiles = 0;
    const auto audioFile = [&](const Companion &c) {
        if (c.kind != Ldx::Kind::Pcm || QFileInfo(c.file).suffix().compare(QLatin1String("wav"), Qt::CaseInsensitive) != 0) return c.file;
        const QString path = plan.directory->filePath(QStringLiteral("audio%1.pcm").arg(++audioFiles));
        Step step;
        step.description = tr("Reading the audio of %1").arg(c.name);
        step.action = [source = c.file, path](QString *error) {
            qint64 start = 0, length = 0;
            double rate = 0.0;
            QString why;
            QFile in(source);
            QFile out(path);
            if (!readWav(source, &rate, &start, &length, &why) || !in.open(QIODevice::ReadOnly) || !in.seek(start) || !out.open(QIODevice::WriteOnly)) {
                if (error) *error = why.isEmpty() ? QObject::tr("%1 cannot be read").arg(QDir::toNativeSeparators(source)) : why;
                return false;
            }
            while (length > 0) {
                const QByteArray part = in.read(std::min<qint64>(length, 1 << 20));
                if (part.isEmpty() || out.write(part) != part.size()) {
                    if (error) *error = QObject::tr("the audio of %1 cannot be copied").arg(QDir::toNativeSeparators(source));
                    return false;
                }
                length -= part.size();
            }
            return true;
        };
        plan.steps.append(step);
        return path;
    };
    // The track a file is added as: for a track of kind pcm, efm or ac3 (the notes are annotations)
    const Codec listCodec = m_codec;
    const auto addTrack = [&](const QString &description, const QString &kind, const QString &file, const QString &name, double rate, const std::shared_ptr<quint32> &about) {
        const QString from = current;
        const QString to = nextFile();
        Step step;
        step.description = description;
        step.program = program;
        step.arguments = [=] {
            QStringList arguments { QStringLiteral("addtrack"), from, QStringLiteral("--kind"), kind, QStringLiteral("--input"), file, QStringLiteral("--about"),
                                    QString::number(*about), QStringLiteral("--name"), name };
            if (kind == QLatin1String("pcm") && rate > 0.0) arguments << QStringLiteral("--rate") << QString::number(rate, 'f', 0);
            // The audio is kept as the list says (FLAC for 16-bit samples with FLAC or Auto)
            if (kind == QLatin1String("pcm") && codecFor(listCodec, Ldx::Kind::Pcm, 16, Ldx::Codec::Raw) == Ldx::Codec::Flac) arguments << QStringLiteral("--codec") << QStringLiteral("flac");
            arguments << QStringLiteral("-o") << to;
            return arguments;
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };
    const auto kindName = [](Ldx::Kind kind) { return Ldx::kindName(kind); };

    // New metadata for a capture: a block that is the current metadata from then on (ld-container metadata)
    int metadataFiles = 0;
    const auto metadataStep = [&](const QString &description, const QByteArray &json, const std::shared_ptr<quint32> &about) {
        const QString file = plan.directory->filePath(QStringLiteral("metadata%1.json").arg(++metadataFiles));
        QFile out(file);
        if (out.open(QIODevice::WriteOnly)) out.write(json);
        const QString from = current;
        const QString to = nextFile();
        Step step;
        step.description = description;
        step.program = program;
        step.arguments = [from, to, file, about] {
            return QStringList { QStringLiteral("metadata"), from, QStringLiteral("--id"), QString::number(*about), QStringLiteral("--input"), file, QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };

    // The shape a capture is shown in by video players: the header of its video track, and isWidescreen in its metadata (ld-container display)
    const auto displayStep = [&](const QString &name, const QString &aspect, const std::shared_ptr<quint32> &about) {
        const QString from = current;
        const QString to = nextFile();
        const QString shape = aspect.isEmpty() ? QStringLiteral("none") : aspect;
        Step step;
        step.description = aspect.isEmpty() ? tr("Showing %1 with square pixels (1:1)").arg(name) : tr("Showing %1 as %2").arg(name, aspect);
        step.program = program;
        step.arguments = [from, to, shape, about] {
            return QStringList { QStringLiteral("display"), from, QStringLiteral("--id"), QString::number(*about), QStringLiteral("--aspect"), shape, QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };


    // What a person said of a capture that comes from another file, changed (ld-container tag)
    const auto tagSaidStep = [&](const QString &name, const QStringList &options, const std::shared_ptr<quint32> &about) {
        const QString from = current;
        const QString to = nextFile();
        Step step;
        step.description = tr("Saying what %1 is").arg(name);
        step.program = program;
        step.arguments = [from, to, options, about] {
            return QStringList { QStringLiteral("tag"), from, QStringLiteral("--id"), QString::number(*about) } + options + QStringList { QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };

    // Where a capture comes from, changed (ld-container rfsource): the JSON of a file, or taken out
    int rfFiles = 0;
    const auto rfSourceStep = [&](const QString &name, const QByteArray &json, const std::shared_ptr<quint32> &about) {
        QString file;
        if (!json.isEmpty()) {
            file = plan.directory->filePath(QStringLiteral("rfsource%1.json").arg(++rfFiles));
            QFile out(file);
            if (out.open(QIODevice::WriteOnly)) out.write(json);
        }
        const QString from = current;
        const QString to = nextFile();
        Step step;
        step.description = json.isEmpty() ? tr("Taking out where %1 comes from").arg(name) : tr("Saying where %1 comes from").arg(name);
        step.program = program;
        step.arguments = [from, to, file, about] {
            QStringList arguments { QStringLiteral("rfsource"), from, QStringLiteral("--id"), QString::number(*about) };
            if (file.isEmpty()) arguments << QStringLiteral("--remove");
            else arguments << QStringLiteral("--set") << file;
            return arguments << QStringLiteral("-o") << to;
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };

    // The name of a capture that was in the file (the name of its video track)
    const auto renameStep = [&](const Entry &e) {
        const QString from = current;
        const QString to = nextFile();
        const QString number = QString::number(e.video.number);
        const QString name = e.name;
        Step step;
        step.description = tr("Naming track %1 %2").arg(number, name);
        step.program = program;
        step.arguments = [from, to, number, name] {
            return QStringList { QStringLiteral("rename"), from, QStringLiteral("--id"), number, QStringLiteral("--name"), name, QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };

    // The name and the parameters of tracks of the file that were changed (ld-container settrack, with the file made on the way as its output)
    const auto setTrackStep = [&](const QString &what, quint32 number, const QString &name, bool setName, const QJsonObject &was, const QJsonObject &now, bool declare = false) {
        QStringList unset;
        QStringList set = parameterChanges(was, now, &unset);
        if (declare && now.contains("sampleRate") && std::none_of(set.begin(), set.end(), [](const QString &item) { return item.startsWith(QStringLiteral("sampleRate=")); })) {
            set << QStringLiteral("sampleRate=") + QString::number(now.value("sampleRate").toDouble(), 'f', 0);
        }
        if (!setName && set.isEmpty() && unset.isEmpty()) return;
        const QString from = current;
        const QString to = nextFile();
        QStringList command { QStringLiteral("settrack"), from, QStringLiteral("--id"), QString::number(number) };
        if (setName) command << QStringLiteral("--name") << name;
        for (const QString &item : set) command << QStringLiteral("--set") << item;
        for (const QString &key : unset) command << QStringLiteral("--unset") << key;
        command << QStringLiteral("-o") << to;
        program_step(tr("Changing %1").arg(what), command, cleanup(toward(to)));
    };

    // The chroma file of a Y/C capture that is added to a capture that has none (ld-container addchroma), kept as the list says
    const auto chromaStep = [&](const QString &description, const QString &file, const std::shared_ptr<quint32> &about, bool = false) {
        const QString from = current;
        const QString to = nextFile();
        const QString chromaCodec = codecArgument(m_codec);
        Step step;
        step.description = description;
        step.program = program;
        step.arguments = [from, to, file, about, chromaCodec] {
            return QStringList { QStringLiteral("addchroma"), from, QStringLiteral("--about"), QString::number(*about), QStringLiteral("--input"), file,
                                 QStringLiteral("--codec"), chromaCodec, QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    };

    // The captures that are added, in the order of the list
    const QString codec = codecArgument(m_codec);
    std::vector<std::shared_ptr<quint32>> ids(m_entries.size());
    for (int pass = 0; pass < 2; ++pass)    // (the captures that are packed first, then the ones taken from files)
    for (size_t index = 0; index < m_entries.size(); ++index) {
        const Entry &e = m_entries[index];
        if (e.fromArchive || e.removed || (pass == 1) == e.ldxFile.isEmpty()) continue;
        const auto about = std::make_shared<quint32>(0);
        ids[index] = about;
        // A capture taken from another .ldx file: its tracks are put in the file as they are (ld-container merge), except the ones that were taken out; the name,
        // the codec of the list and the notes that were changed are applied with it
        if (!e.ldxFile.isEmpty()) {
            if (current.isEmpty()) continue;        // (there is nothing to put it in: addArchive does not let that happen)
            QStringList excluded;
            for (const Companion &c : e.companions) {
                if (c.removed && !c.isNew()) excluded << QString::number(c.number);
            }
            if (e.notes != e.originalNotes && e.notesTrack != 0) excluded << QString::number(e.notesTrack);
            const QString from = current;
            const QString to = nextFile();
            QStringList command { QStringLiteral("merge"), from, e.ldxFile, QStringLiteral("--id"), QString::number(e.ldxTrack) };
            for (const QString &id : excluded) command << QStringLiteral("--exclude") << id;
            if (e.name != e.video.name) command << QStringLiteral("--name") << e.name;
            command << QStringLiteral("--codec") << (m_codec == Codec::AsIs ? QStringLiteral("asis") : codecArgument(m_codec)) << QStringLiteral("-o") << to;
            const auto was = cleanup(toward(to));
            const quint32 wanted = e.ldxTrack;
            program_step(tr("Putting %1 of %2 in the file").arg(e.name, QFileInfo(e.ldxFile).fileName()), command, [about, was, from, wanted](const QString &said) {
                // "... as the tracks: 1->4 2->5": the id the capture has in the file now
                const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("\\b%1->(\\d+)").arg(wanted)).match(said);
                if (match.hasMatch()) *about = match.captured(1).toUInt();
                was(from);
            });
            if (e.notes != e.originalNotes && !e.notes.isEmpty()) addTrack(tr("Adding the notes of %1").arg(e.name), QStringLiteral("annotations"), notesFile(e.notes), tr("notes"), 0.0, about);
            if (!e.newMetadata.isEmpty()) metadataStep(tr("Writing the metadata of %1").arg(e.name), e.newMetadata, about);
            if (e.aspect != e.originalAspect) displayStep(e.name, e.aspect, about);
            if (e.rfSource != e.originalRfSource) rfSourceStep(e.name, e.rfSource, about);
            if (const QStringList said = saidOptions(e.video.parameters, e.videoParameters, true); !said.isEmpty()) tagSaidStep(e.name, said, about);
            continue;
        }
        QStringList arguments;
        QString pcm;
        double pcmRate = kDefaultPcmRate;
        QString efm, ac3;
        QList<const Companion *> more;
        QList<const Companion *> chromas;
        for (const Companion &c : e.companions) {
            if (c.chroma) chromas.append(&c);
            else if (c.kind == Ldx::Kind::Pcm && pcm.isEmpty()) { pcm = audioFile(c); pcmRate = c.sampleRate; }
            else if (c.kind == Ldx::Kind::Efm && efm.isEmpty() && current.isEmpty()) efm = c.file;
            else if (c.kind == Ldx::Kind::Ac3 && ac3.isEmpty() && current.isEmpty()) ac3 = c.file;
            else more.append(&c);
        }
        const QString from = current;
        const QString to = nextFile();
        QStringList command = from.isEmpty() ? QStringList { QStringLiteral("pack"), e.capture } : QStringList { QStringLiteral("add"), from, e.capture };
        command << QStringLiteral("--codec") << codec << QStringLiteral("--name") << e.name;
        // The chroma file that is beside the capture goes with it when ld-container takes the capture, unless its line was taken out or another one was chosen
        const bool keepsBeside = std::any_of(chromas.begin(), chromas.end(), [](const Companion *c) { return c->beside; });
        const bool chosen = std::any_of(chromas.begin(), chromas.end(), [](const Companion *c) { return !c->beside; });
        if ((e.besideChroma && !keepsBeside) || chosen) command << QStringLiteral("--no-chroma");
        if (!pcm.isEmpty()) command << QStringLiteral("--pcm") << pcm << QStringLiteral("--pcm-rate") << QString::number(pcmRate, 'f', 0);
        if (!efm.isEmpty()) command << QStringLiteral("--efm") << efm;
        if (!ac3.isEmpty()) command << QStringLiteral("--ac3") << ac3;
        // What a person said of the capture
        command << saidOptions(QJsonObject(), e.videoParameters, false);
        command << QStringLiteral("-o") << to;
        const auto was = cleanup(toward(to));
        program_step(from.isEmpty() ? tr("Packing %1").arg(e.name) : tr("Adding %1").arg(e.name), command, [about, was, from](const QString &said) {
            static const QRegularExpression track(QStringLiteral("as track (\\d+)"));
            const QRegularExpressionMatch match = track.match(said);
            if (match.hasMatch()) *about = match.captured(1).toUInt();
            was(from);
        });
        for (const Companion *c : chromas) {
            if (!c->beside) chromaStep(tr("Adding the chroma of %1").arg(e.name), c->file, about);
        }
        for (const Companion *c : more) {
            addTrack(tr("Adding %1 to %2").arg(c->name, e.name), kindName(c->kind), audioFile(*c), c->name, c->sampleRate, about);
        }
        if (!e.notes.isEmpty()) addTrack(tr("Adding the notes of %1").arg(e.name), QStringLiteral("annotations"), notesFile(e.notes), tr("notes"), 0.0, about);
        if (!e.newMetadata.isEmpty()) metadataStep(tr("Writing the metadata of %1").arg(e.name), e.newMetadata, about);
        if ((e.fromArchive || !e.ldxFile.isEmpty()) ? (e.aspect != e.originalAspect || e.forceDisplay) : !e.aspect.isEmpty()) displayStep(e.name, e.aspect, about);
        if (e.rfSource != e.originalRfSource) rfSourceStep(e.name, e.rfSource, about);
    }

    // What is added to the captures that were opened
    for (const Entry &e : m_entries) {
        if (!e.fromArchive || e.removed) continue;
        const auto about = std::make_shared<quint32>(e.video.number);
        for (const Companion &c : e.companions) {
            if (c.isNew() && c.chroma) chromaStep(tr("Adding the chroma of %1").arg(e.name), c.file, about, true);
            else if (c.isNew()) addTrack(tr("Adding %1 to %2").arg(c.name, e.name), kindName(c.kind), audioFile(c), c.name, c.sampleRate, about);
        }
        if (e.notes != e.originalNotes && !e.notes.isEmpty()) {
            addTrack(tr("Writing the notes of %1").arg(e.name), QStringLiteral("annotations"), notesFile(e.notes), tr("notes"), 0.0, about);
        }
        if (!e.newMetadata.isEmpty()) metadataStep(tr("Writing the metadata of %1").arg(e.name), e.newMetadata, about);
        if ((e.fromArchive || !e.ldxFile.isEmpty()) ? (e.aspect != e.originalAspect || e.forceDisplay) : !e.aspect.isEmpty()) displayStep(e.name, e.aspect, about);
        if (e.rfSource != e.originalRfSource) rfSourceStep(e.name, e.rfSource, about);
        if (e.name != e.video.name) renameStep(e);
        setTrackStep(e.name, e.video.number, QString(), false, e.video.parameters, e.videoParameters);
        for (const Companion &c : e.companions) {
            if (!c.isNew() && !c.removed) setTrackStep(c.name, c.number, c.name, c.name != c.originalName, c.originalParameters, c.parameters, c.declareRate);
        }
    }

    // The captures in the order of the list: the file is written again with its tracks numbered to follow it (the ids of the captures that
    // were added are known once they are)
    if (!m_archive.isEmpty() && orderChanged()) {
        const QString from = current;
        const QString to = nextFile();
        std::vector<std::pair<quint32, std::shared_ptr<quint32>>> listed;
        for (size_t index = 0; index < m_entries.size(); ++index) {
            if (!m_entries[index].removed) listed.push_back({ m_entries[index].video.number, ids[index] });
        }
        Step step;
        step.description = tr("Putting the captures in the order of the list");
        step.program = program;
        step.arguments = [from, to, listed] {
            QStringList order;
            for (const auto &entry : listed) order << QString::number(entry.second ? *entry.second : entry.first);
            return QStringList { QStringLiteral("reorder"), from, QStringLiteral("--order"), order.join(QLatin1Char(',')), QStringLiteral("-o"), to };
        };
        step.after = cleanup(toward(to));
        plan.steps.append(step);
    }
    // The session of the file (after the order of the captures: its sources name the tracks as they are then)
    if (m_sessionChange == SessionChange::Remove && m_hasSession && !current.isEmpty()) {
        const QString from = current;
        const QString to = nextFile();
        program_step(tr("Taking the session out"), { QStringLiteral("unstack"), from, QStringLiteral("-o"), to }, cleanup(toward(to)));
    } else if (m_sessionChange == SessionChange::Set && !current.isEmpty()) {
        const QString from = current;
        const QString to = nextFile();
        QStringList command { QStringLiteral("stack"), from, m_sessionFile, QStringLiteral("-o"), to };
        if (m_sessionEmbed) command << QStringLiteral("--embed") << QStringLiteral("--codec") << codec;
        program_step(tr("Keeping the session %1").arg(QFileInfo(m_sessionFile).fileName()), command, cleanup(toward(to)));
    }
    // The cover of the file (ld-container cover): the picture, where it is kept outside the file, the link; as the list says
    {
        const QStringList cover = coverOptions();
        if (!cover.isEmpty() && !current.isEmpty()) {
            const QString from = current;
            const QString to = nextFile();
            program_step(tr("Changing the cover of the file"), QStringList { QStringLiteral("cover"), from } + cover + QStringList { QStringLiteral("-o"), to }, cleanup(toward(to)));
        }
    }
    // As a stream (no index): the last file made, written again without its cues
    if (m_stream && !current.isEmpty()) {
        const QString from = current;
        const QString to = nextFile();
        program_step(tr("Writing as a stream"), { QStringLiteral("stream"), from, QStringLiteral("-o"), to }, cleanup(toward(to)));
    }
    // A stream that was saved as it came, and is to be a file: written again with its index (any other step already does)
    if (!m_stream && !m_archive.isEmpty() && !m_hasIndex && plan.steps.isEmpty()) {
        const QString from = current;
        const QString to = nextFile();
        program_step(tr("Writing with its index"), { QStringLiteral("index"), from, QStringLiteral("-o"), to }, cleanup(toward(to)));
    }
    if (plan.steps.isEmpty()) return plan;

    // The file that is done takes its name
    const QString done = current;
    Step rename;
    rename.description = tr("Writing %1").arg(QFileInfo(output).fileName());
    rename.action = [done, output](QString *error) {
        if (QFileInfo::exists(output) && !QFile::remove(output)) {
            if (error) *error = QObject::tr("%1 cannot be replaced").arg(QDir::toNativeSeparators(output));
            return false;
        }
        QFile file(done);
        if (!file.rename(output)) {
            if (error) *error = QObject::tr("%1 cannot be renamed to %2: %3").arg(QDir::toNativeSeparators(done), QDir::toNativeSeparators(output), file.errorString());
            return false;
        }
        return true;
    };
    plan.steps.append(rename);
    if (verify) program_step(tr("Checking %1").arg(QFileInfo(output).fileName()), { QStringLiteral("verify"), output });
    return plan;
}
