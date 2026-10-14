/******************************************************************************
 * main.cpp
 * ld-container - pack, unpack and inspect LDX files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>
#include <vector>

#include "ldx.h"
#include "lddecodemetadata.h"
#include "tbc/logging.h"

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

constexpr const char *StackAttachment = "session.stack";

qint64 fieldTimeNs(qint64 field, VideoSystem system)
{
    if (system == PAL) return field * 1000000000LL / 50;
    return field * 1000000000LL * 1001 / 60000;
}

QString systemName(VideoSystem system)
{
    return system == PAL ? "PAL" : (system == NTSC ? "NTSC" : "PAL-M");
}

QString trackLine(const Ldx::Track &track)
{
    return QString("  %1  %2  %3  %4 bits  %5").arg(track.number, 3).arg(Ldx::kindName(track.kind), -11)
        .arg(Ldx::codecName(track.codec), -4).arg(track.depth, 2).arg(track.name);
}

// ---------------------------------------------------------------------------------------------------------------
// Sources of blocks. The blocks of all the sources of a file are written in the order of their time.

class BlockSource
{
public:
    virtual ~BlockSource() = default;
    virtual bool peek(Ldx::Block &block) = 0;      // the next block, without taking it. False when there are no more.
    virtual void pop() = 0;
};

// The blocks of another LDX file, without those of some of its tracks
class LdxSource : public BlockSource
{
public:
    LdxSource(Ldx::Reader &reader, const QSet<quint32> &skip) : reader(reader), skip(skip) {}
    bool peek(Ldx::Block &block) override
    {
        if (!loaded) loaded = next();
        if (loaded) block = current;
        return loaded;
    }
    void pop() override { loaded = false; }

private:
    bool next()
    {
        while (reader.nextBlock(current)) {
            if (!skip.contains(current.track)) return true;
        }
        return false;
    }
    Ldx::Reader &reader;
    QSet<quint32> skip;
    Ldx::Block current;
    bool loaded = false;
};

// One block, at the start (the metadata of a capture)
class OnceSource : public BlockSource
{
public:
    OnceSource(quint32 track, const QByteArray &data) { block.track = track; block.data = data; }
    bool peek(Ldx::Block &out) override { if (done) return false; out = block; return true; }
    void pop() override { done = true; }
private:
    Ldx::Block block;
    bool done = false;
};

// The fields of a TBC file, from a field on
class FieldSource : public BlockSource
{
public:
    FieldSource(QFile &file, quint32 track, VideoSystem system, qint64 fieldBytes, qint64 start, qint64 fields)
        : file(file), track(track), system(system), fieldBytes(fieldBytes), start(start), fields(fields)
    {
        file.seek(start * fieldBytes);
        firstTime = fieldTimeNs(start, system);
    }
    qint64 endTimeNs() const { return fieldTimeNs(start + fields, system) - firstTime; }
    qint64 written() const { return index; }

    bool peek(Ldx::Block &block) override
    {
        if (index >= fields) return false;
        if (!loaded) {
            data = file.read(fieldBytes);
            if (data.size() != fieldBytes) { fields = index; return false; }
            loaded = true;
        }
        block.track = track;
        block.timeNs = fieldTimeNs(start + index, system) - firstTime;
        block.data = data;
        return true;
    }
    void pop() override { index++; loaded = false; }

private:
    QFile &file;
    quint32 track;
    VideoSystem system;
    qint64 fieldBytes, start, fields;
    qint64 firstTime = 0, index = 0;
    QByteArray data;
    bool loaded = false;
};

// 16-bit stereo PCM in chunks, up to a time
class PcmSource : public BlockSource
{
public:
    PcmSource(QFile &file, quint32 track, double rate, qint64 limitNs) : file(file), track(track), rate(rate), limitNs(limitNs) {}
    qint64 samples() const { return sample; }

    bool peek(Ldx::Block &block) override
    {
        const qint64 time = static_cast<qint64>(sample * 1.0e9 / rate);
        if (time > limitNs) return false;
        if (!loaded) {
            data = file.read(ChunkSamples * FrameBytes);
            if (data.isEmpty()) return false;
            loaded = true;
        }
        block.track = track;
        block.timeNs = time;
        block.data = data;
        return true;
    }
    void pop() override { sample += data.size() / FrameBytes; loaded = false; }

private:
    static constexpr int ChunkSamples = 1470;
    static constexpr int FrameBytes = 4;
    QFile &file;
    quint32 track;
    double rate;
    qint64 limitNs;
    qint64 sample = 0;
    QByteArray data;
    bool loaded = false;
};

bool mergeInto(Ldx::Writer &writer, const std::vector<BlockSource *> &sources)
{
    for (;;) {
        BlockSource *best = nullptr;
        Ldx::Block bestBlock;
        for (BlockSource *source: sources) {
            Ldx::Block block;
            if (!source->peek(block)) continue;
            if (!best || block.timeNs < bestBlock.timeNs) { best = source; bestBlock = block; }
        }
        if (!best) return true;
        if (!writer.writeBlock(bestBlock.track, bestBlock.timeNs, bestBlock.data)) {
            err << "Could not write a block: " << writer.lastError() << "\n";
            return false;
        }
        best->pop();
    }
}

QJsonObject historyEntry(const QString &command, const QString &detail)
{
    QJsonObject entry;
    entry["tool"] = "ld-container";
    entry["version"] = APP_COMMIT;
    entry["command"] = command;
    entry["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!detail.isEmpty()) entry["detail"] = detail;
    return entry;
}

// ---------------------------------------------------------------------------------------------------------------
// What describes a capture: its tracks (TBC, metadata, optional audio)

struct Capture {
    Ldx::Track video;
    Ldx::Track metadata;
    Ldx::Track audio;
    bool hasAudio = false;
    QByteArray metadataJson;
    VideoSystem system = PAL;
    qint64 fieldBytes = 0;
    qint64 start = 0;
    qint64 fields = 0;
    double pcmRate = 44100;
    std::unique_ptr<QFile> tbc;
    std::unique_ptr<QFile> pcm;
};

void addCaptureOptions(QCommandLineParser &parser)
{
    parser.addOption(QCommandLineOption("pcm", "Analogue audio (16-bit stereo PCM) to add", "file"));
    parser.addOption(QCommandLineOption("pcm-rate", "Sample rate of the audio (default 44100)", "hertz", "44100"));
    parser.addOption(QCommandLineOption("start", "First field to pack (default 0)", "field", "0"));
    parser.addOption(QCommandLineOption("fields", "Number of fields to pack (default all)", "count", "0"));
    parser.addOption(QCommandLineOption("name", "Name of the capture (default: the name of the file)", "name"));
}

// Read a capture (its metadata, its TBC file, its audio) and describe its tracks. The tracks are added to the
// writer by the caller, which then sets the id of the track the metadata is about.
bool readCapture(const QCommandLineParser &parser, const QString &input, Capture &capture)
{
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(input);
    LdDecodeMetaData metadata;
    if (metadataFile.isEmpty() || !metadata.read(metadataFile)) {
        err << "Could not read the metadata of " << input << "\n";
        return false;
    }
    const LdDecodeMetaData::VideoParameters &video = metadata.getVideoParameters();
    capture.system = video.system;
    capture.fieldBytes = static_cast<qint64>(video.fieldWidth) * video.fieldHeight * 2;

    QTemporaryDir temporary;
    const QString jsonPath = temporary.filePath("metadata.json");
    if (!metadata.write(jsonPath)) {
        err << "Could not write the metadata out as JSON\n";
        return false;
    }
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        err << "Could not read back the metadata\n";
        return false;
    }
    capture.metadataJson = jsonFile.readAll();

    capture.tbc.reset(new QFile(input));
    if (!capture.tbc->open(QIODevice::ReadOnly)) {
        err << "Could not open " << input << "\n";
        return false;
    }
    capture.start = parser.value("start").toLongLong();
    capture.fields = capture.tbc->size() / capture.fieldBytes - capture.start;
    if (parser.value("fields").toLongLong() > 0) capture.fields = qMin(capture.fields, parser.value("fields").toLongLong());
    if (capture.fields < 1) {
        err << "There is nothing to pack in " << input << "\n";
        return false;
    }

    const QString name = parser.isSet("name") ? parser.value("name") : QFileInfo(input).fileName();
    capture.video.kind = Ldx::Kind::Tbc;
    capture.video.depth = 16;
    capture.video.name = name;
    capture.video.parameters["system"] = systemName(video.system);
    capture.video.parameters["sampleRate"] = video.sampleRate;
    capture.video.parameters["fieldWidth"] = video.fieldWidth;
    capture.video.parameters["fieldHeight"] = video.fieldHeight;

    capture.metadata.kind = Ldx::Kind::Metadata;
    capture.metadata.depth = 0;
    capture.metadata.name = name + " metadata";
    capture.metadata.parameters["namespace"] = "ldx.tbcmetadata";
    capture.metadata.parameters["format"] = "ld-decode-json";

    capture.hasAudio = parser.isSet("pcm");
    if (capture.hasAudio) {
        capture.pcm.reset(new QFile(parser.value("pcm")));
        if (!capture.pcm->open(QIODevice::ReadOnly)) {
            err << "Could not open " << parser.value("pcm") << "\n";
            return false;
        }
        capture.pcmRate = parser.value("pcm-rate").toDouble();
        capture.audio.kind = Ldx::Kind::Pcm;
        capture.audio.depth = 16;
        capture.audio.name = QFileInfo(capture.pcm->fileName()).fileName();
        capture.audio.parameters["sampleRate"] = capture.pcmRate;
        capture.audio.parameters["channels"] = 2;
    }
    return true;
}

void addCaptureTracks(Ldx::Writer &writer, Capture &capture)
{
    writer.addTrack(capture.video);
    capture.metadata.parameters["track"] = static_cast<int>(capture.video.number);
    writer.addTrack(capture.metadata);
    if (capture.hasAudio) {
        capture.audio.parameters["track"] = static_cast<int>(capture.video.number);
        writer.addTrack(capture.audio);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The session of a stack kept in the file

// The ids of the tracks a session names as its sources
QSet<quint32> sessionTracks(const QByteArray &session)
{
    QSet<quint32> tracks;
    for (const QJsonValue &value: QJsonDocument::fromJson(session).object().value("sources").toArray()) {
        const int track = value.toObject().value("track").toInt(0);
        if (track > 0) tracks.insert(static_cast<quint32>(track));
    }
    return tracks;
}

// The session without the sources that are some tracks. Gives how many were taken out.
QByteArray sessionWithout(const QByteArray &session, const QSet<quint32> &tracks, int &taken)
{
    taken = 0;
    QJsonObject root = QJsonDocument::fromJson(session).object();
    QJsonArray sources;
    for (const QJsonValue &value: root.value("sources").toArray()) {
        if (tracks.contains(static_cast<quint32>(value.toObject().value("track").toInt(0)))) { taken++; continue; }
        sources.append(value);
    }
    root["sources"] = sources;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ---------------------------------------------------------------------------------------------------------------
// Rewriting a file: its tracks (but those taken out), its attachments and its history are put in a new file, to
// which tracks are then added, and the blocks are copied.

struct Rewrite {
    Ldx::Reader reader;
    QVector<Ldx::Track> tracks;
    QVector<Ldx::Reader::Attachment> attachments;
    QJsonArray history;
};

bool openForRewrite(Rewrite &rewrite, const QString &fileName)
{
    QString error;
    if (!rewrite.reader.open(fileName, error)) {
        err << error << "\n";
        return false;
    }
    if (rewrite.reader.profile() < 1) {
        err << fileName << " is not an LDX file\n";
        return false;
    }
    rewrite.tracks = rewrite.reader.tracks();
    rewrite.attachments = rewrite.reader.attachments();
    rewrite.history = rewrite.reader.history();
    return true;
}

void prepareWriter(Rewrite &rewrite, Ldx::Writer &writer, const QSet<quint32> &skip)
{
    for (Ldx::Track track: rewrite.tracks) {
        if (skip.contains(track.number)) continue;
        writer.addTrack(track);
    }
    writer.setNextTrackId(rewrite.reader.nextTrackId());
    for (const Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        writer.addAttachment(attachment.name, attachment.mediaType, attachment.data);
    }
}

bool sameFile(const QString &a, const QString &b)
{
    return QFileInfo(a).absoluteFilePath() == QFileInfo(b).absoluteFilePath();
}

// ---------------------------------------------------------------------------------------------------------------
// The commands

// ld-container pack input.tbc [--pcm file] -o output.ldx
int pack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Pack a TBC capture, its metadata and its audio into an LDX file");
    parser.addHelpOption();
    parser.addPositionalArgument("input", "The .tbc file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    addCaptureOptions(parser);
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString input = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(input, output)) {
        err << "The output must not be the input\n";
        return 1;
    }

    Capture capture;
    if (!readCapture(parser, input, capture)) return 1;

    Ldx::Writer writer;
    addCaptureTracks(writer, capture);
    writer.setHistory(QJsonArray { historyEntry("pack", QFileInfo(input).fileName()) });

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }

    OnceSource metadata(capture.metadata.number, capture.metadataJson);
    FieldSource fields(*capture.tbc, capture.video.number, capture.system, capture.fieldBytes, capture.start, capture.fields);
    PcmSource pcm(capture.hasAudio ? *capture.pcm : *capture.tbc, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    std::vector<BlockSource *> sources { &metadata, &fields };
    if (capture.hasAudio) sources.push_back(&pcm);
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Packed " << fields.written() << " fields";
    if (capture.hasAudio) out << " and " << pcm.samples() << " audio samples";
    out << " as track " << capture.video.number << " of " << output << "\n";
    return 0;
}

// ld-container add file.ldx input.tbc -o output.ldx
int addStream(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Add a capture to an LDX file, as a track of its own (for the sources of a stack, or\n"
                                     "several captures of the same VHS tape). The file is written again, to the output.");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("input", "The .tbc file to add");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    addCaptureOptions(parser);
    parser.process(arguments);
    if (parser.positionalArguments().size() != 2 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().at(0);
    const QString input = parser.positionalArguments().at(1);
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    Capture capture;
    if (!readCapture(parser, input, capture)) return 1;

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    addCaptureTracks(writer, capture);
    rewrite.history.append(historyEntry("add", QString("track %1: %2").arg(capture.video.number).arg(QFileInfo(input).fileName())));
    writer.setHistory(rewrite.history);

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, {});
    OnceSource metadata(capture.metadata.number, capture.metadataJson);
    FieldSource fields(*capture.tbc, capture.video.number, capture.system, capture.fieldBytes, capture.start, capture.fields);
    PcmSource pcm(capture.hasAudio ? *capture.pcm : *capture.tbc, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    std::vector<BlockSource *> sources { &old, &metadata, &fields };
    if (capture.hasAudio) sources.push_back(&pcm);
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Added " << QFileInfo(input).fileName() << " as track " << capture.video.number << " to " << output << "\n";
    return 0;
}

// ld-container remove file.ldx --id x -o output.ldx
int removeStream(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Remove the track with an id from an LDX file, with the metadata about it. The file is\n"
                                     "written again, to the output. The id is not given to another track.");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track to remove", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("id") || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }
    const quint32 id = parser.value("id").toUInt();

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;

    // The track, and what is about it
    QSet<quint32> skip;
    bool found = false;
    for (const Ldx::Track &track: rewrite.tracks) {
        if (track.number == id) found = true;
    }
    if (!found) {
        err << "There is no track " << id << " in " << source << "\n";
        return 1;
    }
    skip.insert(id);
    for (const Ldx::Track &track: rewrite.tracks) {
        if ((track.kind == Ldx::Kind::Metadata || track.kind == Ldx::Kind::Annotations || track.kind == Ldx::Kind::Pcm)
            && track.parameters.value("track").toInt(0) == static_cast<int>(id)) {
            skip.insert(track.number);
            out << "  also removing track " << track.number << " (" << Ldx::kindName(track.kind) << ", about track " << id << ")\n";
        }
    }

    // The session kept in the file can no longer have it as a source
    for (Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        if (attachment.name != StackAttachment) continue;
        int taken = 0;
        attachment.data = sessionWithout(attachment.data, skip, taken);
        if (taken > 0) out << "  the session " << StackAttachment << " had " << taken << " source(s) on it, which are taken out\n";
    }

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, skip);
    rewrite.history.append(historyEntry("remove", QString("track %1").arg(id)));
    writer.setHistory(rewrite.history);

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, skip);
    if (!mergeInto(writer, { &old }) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Removed track " << id << ", written to " << output << "\n";
    return 0;
}

// ld-container stack file.ldx session.stack -o output.ldx
int setStack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Keep a stacking session in an LDX file. The sources of the session are tracks of the\n"
                                     "file, named by their id (\"track\": 2), and not paths. The file is written again, to the output.");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("session", "The .stack file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    if (parser.positionalArguments().size() != 2 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().at(0);
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }

    QFile sessionFile(parser.positionalArguments().at(1));
    if (!sessionFile.open(QIODevice::ReadOnly)) {
        err << "Could not open " << sessionFile.fileName() << "\n";
        return 1;
    }
    const QByteArray session = sessionFile.readAll();
    if (QJsonDocument::fromJson(session).object().value("sources").toArray().isEmpty()) {
        err << sessionFile.fileName() << " is not a session with sources\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;

    // Each source is a track of the file
    QSet<quint32> existing;
    for (const Ldx::Track &track: rewrite.tracks) existing.insert(track.number);
    for (const QJsonValue &value: QJsonDocument::fromJson(session).object().value("sources").toArray()) {
        const int track = value.toObject().value("track").toInt(0);
        if (track < 1) {
            err << "A source of the session is a file and not a track: name its track with \"track\": id, and leave out \"file\"\n";
            return 1;
        }
        if (!existing.contains(static_cast<quint32>(track))) {
            err << "The session has a source on track " << track << ", which is not in " << source << "\n";
            return 1;
        }
    }

    bool replaced = false;
    for (Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        if (attachment.name == StackAttachment) { attachment.data = session; attachment.mediaType = "application/json"; replaced = true; }
    }
    if (!replaced) rewrite.attachments.append({ StackAttachment, "application/json", session });

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("stack", QString("%1 sources").arg(sessionTracks(session).size())));
    writer.setHistory(rewrite.history);

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, {});
    if (!mergeInto(writer, { &old }) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "The session is kept in " << output << "\n";
    return 0;
}

// ld-container info file.ldx
int info(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Show what an LDX file holds");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1) {
        err << parser.helpText();
        return 1;
    }

    Ldx::Reader reader;
    QString error;
    if (!reader.open(parser.positionalArguments().first(), error)) {
        err << error << "\n";
        return 1;
    }
    out << "Profile: " << (reader.profile() ? QString::number(reader.profile()) : QString("none (not an LDX file)")) << "\n";

    // The blocks give the size of each track
    QMap<quint32, qint64> blocks, bytes, firstTime, lastTime;
    Ldx::Block block;
    while (reader.nextBlock(block)) {
        blocks[block.track]++;
        bytes[block.track] += block.data.size();
        if (!firstTime.contains(block.track)) firstTime[block.track] = block.timeNs;
        lastTime[block.track] = block.timeNs;
    }

    out << "Tracks (the next id is " << reader.nextTrackId() << "):\n";
    for (const Ldx::Track &track: reader.tracks()) {
        out << trackLine(track) << "\n";
        out << "       " << blocks.value(track.number) << " blocks, " << bytes.value(track.number) << " bytes";
        if (blocks.value(track.number) > 1) {
            out << ", from " << firstTime.value(track.number) / 1000000.0 << " ms to " << lastTime.value(track.number) / 1000000.0 << " ms";
        }
        out << "\n       " << QJsonDocument(track.parameters).toJson(QJsonDocument::Compact) << "\n";
    }
    out << "Attachments:\n";
    for (const Ldx::Reader::Attachment &attachment: reader.attachments()) {
        out << "  " << attachment.name << " (" << attachment.mediaType << ", " << attachment.data.size() << " bytes)\n";
        if (attachment.name == StackAttachment) {
            QStringList tracks;
            for (const quint32 track: sessionTracks(attachment.data)) tracks << QString::number(track);
            tracks.sort();
            out << "       a stacking session, with the tracks " << tracks.join(", ") << " as sources\n";
        }
    }
    out << "History:\n";
    for (const QJsonValue &entry: reader.history()) {
        out << "  " << QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact) << "\n";
    }
    return 0;
}

// ld-container unpack file.ldx -o directory
int unpack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write the tracks and the attachments of an LDX file out as separate files");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The directory to write to", "directory"));
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }

    Ldx::Reader reader;
    QString error;
    if (!reader.open(parser.positionalArguments().first(), error)) {
        err << error << "\n";
        return 1;
    }
    const QDir directory(parser.value("output"));
    QDir().mkpath(directory.absolutePath());
    const QString base = QFileInfo(parser.positionalArguments().first()).completeBaseName();

    std::map<quint32, std::unique_ptr<QFile>> files;
    for (const Ldx::Track &track: reader.tracks()) {
        QString suffix = ".bin";
        if (track.kind == Ldx::Kind::Tbc) suffix = ".tbc";
        else if (track.kind == Ldx::Kind::Cvbs) suffix = ".cvbs";
        else if (track.kind == Ldx::Kind::Pcm) suffix = ".pcm";
        else if (track.kind == Ldx::Kind::Efm) suffix = ".efm";
        else if (track.kind == Ldx::Kind::Ac3) suffix = ".ac3";
        else if (track.kind == Ldx::Kind::Metadata) suffix = ".json";
        else if (track.kind == Ldx::Kind::Annotations) suffix = ".annotations.json";
        // The files are told apart by the id of their track
        const QString name = QString("%1.%2%3").arg(base).arg(track.number).arg(track.kind == Ldx::Kind::Metadata ? ".tbc" + suffix : suffix);
        auto file = std::make_unique<QFile>(directory.filePath(name));
        if (!file->open(QIODevice::WriteOnly)) {
            err << "Could not write " << file->fileName() << "\n";
            return 1;
        }
        out << "  track " << track.number << " -> " << name << "\n";
        files[track.number] = std::move(file);
    }

    Ldx::Block block;
    while (reader.nextBlock(block)) {
        const auto found = files.find(block.track);
        if (found != files.end()) found->second->write(block.data);
    }

    for (const Ldx::Reader::Attachment &attachment: reader.attachments()) {
        QFile file(directory.filePath(QFileInfo(attachment.name).fileName()));
        if (!file.open(QIODevice::WriteOnly)) {
            err << "Could not write " << file.fileName() << "\n";
            return 1;
        }
        file.write(attachment.data);
        out << "  attachment " << attachment.name << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(debugOutputHandler);
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("ld-container");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));

    QStringList arguments = application.arguments();
    if (arguments.size() < 2 || arguments[1] == "-h" || arguments[1] == "--help") {
        out << "ld-container - pack, unpack and inspect LDX files\n\n"
               "Usage: ld-container <command> [options]\n\n"
               "Commands:\n"
               "  pack     pack a .tbc, its metadata and its audio into an .ldx file\n"
               "  add      add a capture to an .ldx file, as a track of its own\n"
               "  remove   remove the track with an id (and what is about it) from an .ldx file\n"
               "  stack    keep a stacking session in an .ldx file, its sources being tracks\n"
               "  unpack   write the tracks and attachments of an .ldx file out as separate files\n"
               "  info     show what an .ldx file holds\n\n"
               "Use ld-container <command> --help for the options of a command.\n";
        return arguments.size() < 2 ? 1 : 0;
    }

    const QString command = arguments[1];
    // The command is not an option of the parser of the command
    arguments.removeAt(1);
    if (command == "pack") return pack(arguments);
    if (command == "add") return addStream(arguments);
    if (command == "remove") return removeStream(arguments);
    if (command == "stack") return setStack(arguments);
    if (command == "unpack") return unpack(arguments);
    if (command == "info") return info(arguments);
    err << "Unknown command " << command << "\n";
    return 1;
}
