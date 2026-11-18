/******************************************************************************
 * main.cpp
 * ld-container - pack, unpack and inspect LDX files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <map>
#include <functional>
#include <memory>
#include <vector>

#include "ldx.h"
#include "captureinfo.h"
#include "ldxcapture.h"
#include "cvbs.h"
#include "cvbsaudio.h"
#include "tbcfiles.h"
#include "lddecodemetadata.h"
#include "ldxpicture.h"
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

// The metadata of each field, each just before its video: the blocks have the time of the video they are about
class FieldMetadataSource : public BlockSource
{
public:
    FieldMetadataSource(quint32 track, const std::vector<QByteArray> &json, VideoSystem system, qint64 start, int unitFields)
        : track(track), json(json), system(system), start(start), unitFields(unitFields) {}
    bool peek(Ldx::Block &block) override
    {
        if (index >= json.size()) return false;
        block.track = track;
        block.timeNs = fieldTimeNs(start + (static_cast<qint64>(index) / unitFields) * unitFields, system) - fieldTimeNs(start, system);
        block.data = json[index];
        return true;
    }
    void pop() override { index++; }
private:
    quint32 track;
    const std::vector<QByteArray> &json;
    VideoSystem system;
    qint64 start;
    int unitFields;
    size_t index = 0;
};

// The fields of a TBC file, or the frames of a CVBS file (which are two fields each, and whose samples are given as
// 10-bit values), from a field on
class FieldSource : public BlockSource
{
public:
    using Convert = std::function<QByteArray(const QByteArray &)>;

    FieldSource(QFile &file, quint32 track, VideoSystem system, qint64 unitBytes, qint64 start, qint64 fields,
                int unitFields = 1, Convert convert = Convert())
        : file(file), track(track), system(system), unitBytes(unitBytes), start(start), unitFields(unitFields),
          units(fields / unitFields), convert(std::move(convert))
    {
        file.seek((start / unitFields) * unitBytes);
        firstTime = fieldTimeNs(start, system);
    }
    qint64 endTimeNs() const { return fieldTimeNs(start + units * unitFields, system) - firstTime; }
    qint64 written() const { return index * unitFields; }

    bool peek(Ldx::Block &block) override
    {
        if (index >= units) return false;
        if (!loaded) {
            data = file.read(unitBytes);
            if (data.size() != unitBytes) { units = index; return false; }
            if (convert) data = convert(data);
            loaded = true;
        }
        block.track = track;
        block.timeNs = fieldTimeNs(start + index * unitFields, system) - firstTime;
        block.data = data;
        return true;
    }
    void pop() override { index++; loaded = false; }

private:
    QFile &file;
    quint32 track;
    VideoSystem system;
    qint64 unitBytes, start;
    int unitFields;
    qint64 units;
    Convert convert;
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

// The 24-bit stereo audio of a CVBS capture (a WAV file), in chunks of whole samples (6 bytes each), up to a time
class Wav24Source : public BlockSource
{
public:
    Wav24Source(std::shared_ptr<CvbsAudio::WavReader> reader, quint32 track, double rate, qint64 limitNs)
        : reader(std::move(reader)), track(track), rate(rate), limitNs(limitNs), total(this->reader->info().numberOfSamples) {}
    qint64 samples() const { return sample; }

    bool peek(Ldx::Block &block) override
    {
        const qint64 time = static_cast<qint64>(sample * 1.0e9 / rate);
        if (time > limitNs || sample >= total) return false;
        if (!loaded) {
            data = reader->read(sample, qMin<qint64>(ChunkSamples, total - sample));
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
    static constexpr int FrameBytes = 6;
    std::shared_ptr<CvbsAudio::WavReader> reader;
    quint32 track;
    double rate;
    qint64 limitNs;
    qint64 total;
    qint64 sample = 0;
    QByteArray data;
    bool loaded = false;
};

// A file of bytes with no time of its own (EFM t-values, AC3 data), in chunks of one size, which are spread over the
// time of the video in proportion to where they are in the file
class ChunkSource : public BlockSource
{
public:
    ChunkSource(QFile &file, quint32 track, qint64 durationNs) : file(file), track(track), durationNs(durationNs), total(file.size()) {}
    qint64 bytes() const { return position; }

    bool peek(Ldx::Block &block) override
    {
        if (!loaded) {
            data = file.read(ChunkBytes);
            if (data.isEmpty()) return false;
            loaded = true;
        }
        block.track = track;
        block.timeNs = total > 0 ? static_cast<qint64>(static_cast<double>(position) / static_cast<double>(total) * static_cast<double>(durationNs)) : 0;
        block.data = data;
        return true;
    }
    void pop() override { position += data.size(); loaded = false; }

private:
    static constexpr int ChunkBytes = 65536;
    QFile &file;
    quint32 track;
    qint64 durationNs;
    qint64 total;
    qint64 position = 0;
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
    return LdxCapture::historyEntry(command, detail);
}

// ---------------------------------------------------------------------------------------------------------------
// What describes a capture: its tracks (TBC, metadata, optional audio)

struct Capture {
    Ldx::Track video;
    Ldx::Track metadata;
    Ldx::Track audio;
    bool hasAudio = false;
    QByteArray metadataJson;
    std::vector<QByteArray> fieldJson;      // with live metadata: the metadata of each field
    VideoSystem system = PAL;
    bool cvbs = false;                      // the frames of a CVBS file, and not the fields of a TBC
    qint64 unitBytes = 0;                   // what is read from the file for a block: a field, or a frame
    int unitFields = 1;                     // and how many fields that is
    std::function<QByteArray(const QByteArray &)> convert;
    qint64 start = 0;
    qint64 fields = 0;
    double pcmRate = 44100;
    std::unique_ptr<QFile> tbc;
    std::unique_ptr<QFile> pcm;
    std::shared_ptr<CvbsAudio::WavReader> wav;      // the audio of a CVBS capture: a 24-bit WAV, not a .pcm
};

void addCaptureOptions(QCommandLineParser &parser)
{
    parser.addOption(QCommandLineOption("pcm", "Analogue audio (16-bit stereo PCM) to add", "file"));
    parser.addOption(QCommandLineOption("pcm-rate", "Sample rate of the audio (default 44100)", "hertz", "44100"));
    parser.addOption(QCommandLineOption("start", "First field to pack (default 0)", "field", "0"));
    parser.addOption(QCommandLineOption("fields", "Number of fields to pack (default all)", "count", "0"));
    parser.addOption(QCommandLineOption("name", "Name of the capture (default: the name of the file)", "name"));
    parser.addOption(QCommandLineOption("no-chroma", "Leave out the chroma file of a Y/C capture that is beside the luma file (it can be added later with addchroma)"));
    parser.addOption(QCommandLineOption("codec", "How the video and the audio are kept: raw (default), flac (TBC and audio), ffv1 (CVBS), or auto (the best for each)", "codec", "raw"));
    // What a person says of the capture (as ld-container tag does later): the words of the .rfx archives of RF-tools
    parser.addOption(QCommandLineOption("medium", "What was captured: ld, vhs, vhd or cvbs", "medium"));
    parser.addOption(QCommandLineOption("reference", "A reference of the disc or the tape (a catalogue number, a title...)", "text"));
    parser.addOption(QCommandLineOption("copy", "The number of the physical copy of the disc, 1 or more", "n"));
    parser.addOption(QCommandLineOption("capture-number", "The number of the capture, 1 or more", "n"));
    parser.addOption(QCommandLineOption("face", "The face of the disc: A, B, AB or BA", "face"));
    parser.addOption(QCommandLineOption("system", "The system, when the metadata do not say it: PAL, NTSC, PAL-M, SECAM (the 625 lines of PAL) or NTSC-VHD (the 525 lines of NTSC)", "system"));
}

// The codec a track of a kind is kept in, for the codec asked for: raw; flac for the tracks of 16-bit samples; ffv1 for CVBS;
// auto, the best for each. A codec that is for other tracks leaves this one raw.
bool codecFor(const QString &asked, Ldx::Kind kind, Ldx::Codec &codec, int depth = 16)
{
    const bool sixteen = (kind == Ldx::Kind::Tbc || kind == Ldx::Kind::Rf || kind == Ldx::Kind::Pcm) && depth == 16;
    codec = Ldx::Codec::Raw;
    if (asked == "raw") return true;
    if (asked == "auto") { codec = sixteen ? Ldx::Codec::Flac : (kind == Ldx::Kind::Cvbs ? Ldx::Codec::Ffv1 : Ldx::Codec::Raw); return true; }
    if (asked == "flac") { if (sixteen) codec = Ldx::Codec::Flac; return true; }
    if (asked == "ffv1") { if (kind == Ldx::Kind::Cvbs) codec = Ldx::Codec::Ffv1; return true; }
    return false;
}

struct CaptureOptions {
    qint64 start = 0;
    qint64 fields = 0;          // 0 for all of them
    QString pcm;                // the audio, if any
    double pcmRate = 44100;
    QString name;               // the name of the capture, the name of its file when empty
    QString codec = "raw";      // how the video and the audio are kept
    bool live = false;          // the metadata of the fields go before their video, one block each (see the pipes of the specification)
    bool asChroma = false;      // the file is the chroma file of a Y/C capture (read for the track about the luma)
    // What a person said of the capture (empty, and 0: not said): parameters of the video track
    QString medium, reference, face;
    QString system;             // the system as the files spell it (PAL, NTSC, PAL-M, SECAM, NTSC-VHD); empty: what the metadata say
    int copy = 0, captureNumber = 0;
    bool invalid = false;       // an option said it wrongly (the reason is written)
};

CaptureOptions captureOptions(const QCommandLineParser &parser)
{
    CaptureOptions options;
    const auto number = [&](const char *option, int &value) {
        if (!parser.isSet(option)) return;
        bool ok = false;
        value = parser.value(option).toInt(&ok);
        if (!ok || value < 1) {
            err << "--" << option << " is a number, 1 or more\n";
            options.invalid = true;
        }
    };
    if (parser.isSet("medium") && !CaptureInfo::parseMedium(parser.value("medium"), options.medium)) {
        err << "--medium is ld, vhs, vhd or cvbs\n";
        options.invalid = true;
    }
    if (parser.isSet("system")) {
        VideoSystem asked;
        QString variant;
        if (!parseVideoSystemVariant(parser.value("system"), asked, variant)) {
            err << "--system is PAL, NTSC, PAL-M, SECAM or NTSC-VHD\n";
            options.invalid = true;
        } else {
            options.system = videoSystemFileName(asked, variant);
        }
    }
    if (parser.isSet("face") && !CaptureInfo::parseFace(parser.value("face"), options.face)) {
        err << "--face is A, B, AB or BA\n";
        options.invalid = true;
    }
    if (parser.isSet("reference")) {
        options.reference = parser.value("reference").trimmed();
        if (options.reference.contains('\n')) {
            err << "--reference is one line\n";
            options.invalid = true;
        }
    }
    number("copy", options.copy);
    number("capture-number", options.captureNumber);
    options.start = parser.value("start").toLongLong();
    options.fields = parser.value("fields").toLongLong();
    options.pcm = parser.value("pcm");
    options.pcmRate = parser.value("pcm-rate").toDouble();
    options.name = parser.value("name");
    options.codec = parser.value("codec");
    return options;
}

// Read a capture (its metadata, its TBC file, its audio) and describe its tracks. The tracks are added to the
// writer by the caller, which then sets the id of the track the metadata is about.
bool readCapture(const CaptureOptions &options, const QString &input, Capture &capture)
{
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(input);
    LdDecodeMetaData metadata;
    if (metadataFile.isEmpty() || !metadata.read(metadataFile)) {
        err << "Could not read the metadata of " << input << "\n";
        return false;
    }
    const LdDecodeMetaData::VideoParameters &video = metadata.getVideoParameters();
    capture.system = video.system;
    capture.cvbs = Cvbs::isDataFileName(input);
    if (capture.cvbs && Cvbs::channel(input) == Cvbs::Channel::Chroma && !options.asChroma) {
        err << input << " is the chroma file of a Y/C capture: pack its luma file " << Cvbs::pairedDataFileName(input) << ", which takes it\n";
        return false;
    }

    // What a unit of the video is: a field of a TBC, or a frame of CVBS (two fields) with its samples in 10 bits
    Cvbs::Geometry geometry;
    Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
    if (capture.cvbs) {
        geometry = Cvbs::geometry(video.system);
        if (geometry.fieldLength() != static_cast<qint32>(video.fieldWidth * video.fieldHeight)) {
            err << "The metadata of " << input << " are not those of a " << Cvbs::presetName(video.system) << " capture\n";
            return false;
        }
        const QString coreMetadata = Cvbs::metaFileName(input);
        if (QFileInfo::exists(coreMetadata)) {
            Cvbs::FileMetadata fileMetadata;
            if (fileMetadata.read(coreMetadata)) encoding = fileMetadata.sampleEncoding;
        }
        capture.unitBytes = geometry.frameSamples * 2;
        capture.unitFields = 2;
        const auto converter = std::make_shared<Cvbs::SampleConverter>(encoding, video.system);
        const qint64 samples = geometry.frameSamples;
        capture.convert = [converter, samples](const QByteArray &stored) {
            // The 10-bit value of each sample, in a word: what the 16-bit TBC domain is, divided by 64
            QByteArray values(static_cast<int>(samples * 2), 0);
            const qint16 *source = reinterpret_cast<const qint16 *>(stored.constData());
            quint16 *target = reinterpret_cast<quint16 *>(values.data());
            for (qint64 i = 0; i < samples; i++) target[i] = converter->toWord(source[i]);
            return values;
        };
    } else {
        capture.unitBytes = static_cast<qint64>(video.fieldWidth) * video.fieldHeight * 2;
    }

    capture.tbc.reset(new QFile(input));
    if (!capture.tbc->open(QIODevice::ReadOnly)) {
        err << "Could not open " << input << "\n";
        return false;
    }
    capture.start = options.start;
    capture.fields = (capture.tbc->size() / capture.unitBytes) * capture.unitFields - capture.start;
    if (options.fields > 0) capture.fields = qMin(capture.fields, options.fields);
    if (capture.cvbs) {
        // (whole frames)
        if (capture.start % 2 != 0) {
            err << "A CVBS capture is packed from a frame: --start is a number of fields, and must be even\n";
            return false;
        }
        capture.fields -= capture.fields % 2;
    }
    if (capture.fields < 1) {
        err << "There is nothing to pack in " << input << "\n";
        return false;
    }

    // The metadata of the part that is packed: its fields, numbered again from 1
    const qint64 total = metadata.getNumberOfFields();
    if (capture.start + capture.fields > total) {
        err << "The metadata of " << input << " describe " << total << " fields, and the video has more\n";
        return false;
    }
    LdDecodeMetaData part;
    const LdDecodeMetaData *written = &metadata;
    if (capture.start != 0 || capture.fields != total) {
        part.setVideoParameters(video);
        part.setPcmAudioParameters(metadata.getPcmAudioParameters());
        part.setIsFirstFieldFirst(metadata.getIsFirstFieldFirst());
        for (qint64 number = 1; number <= capture.fields; number++) {
            part.appendField(metadata.getField(static_cast<qint32>(capture.start + number)));
        }
        written = &part;
    }

    LdDecodeMetaData parameters;
    if (options.live) {
        // The parameters of the video now, the fields as they come
        LdDecodeMetaData::VideoParameters noFields = video;
        noFields.numberOfSequentialFields = 0;      // (the fields come later)
        parameters.setVideoParameters(noFields);
        parameters.setPcmAudioParameters(metadata.getPcmAudioParameters());
        parameters.setIsFirstFieldFirst(metadata.getIsFirstFieldFirst());
        for (qint64 number = 1; number <= capture.fields; number++) {
            LdDecodeMetaData::Field field = const_cast<LdDecodeMetaData *>(written)->getField(static_cast<qint32>(number));
            field.seqNo = static_cast<qint32>(number);
            capture.fieldJson.push_back(LdDecodeMetaData::fieldToJson(field));
        }
        written = &parameters;
    }

    QTemporaryDir temporary;
    const QString jsonPath = temporary.filePath("metadata.json");
    if (!written->write(jsonPath)) {
        err << "Could not write the metadata out as JSON\n";
        return false;
    }
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        err << "Could not read back the metadata\n";
        return false;
    }
    capture.metadataJson = jsonFile.readAll();
    // The system a person said (what the metadata of the file say is kept when none is given): it can be one that has the lines of the capture
    if (!options.system.isEmpty()) {
        VideoSystem asked;
        QString variant;
        parseVideoSystemVariant(options.system, asked, variant);
        if ((asked == PAL) != (video.system == PAL)) {
            err << "--system " << options.system << " has " << (asked == PAL ? 625 : 525) << " lines, and the capture has " << (video.system == PAL ? 625 : 525) << "\n";
            return false;
        }
        QJsonObject root = QJsonDocument::fromJson(capture.metadataJson).object();
        QJsonObject videoParameters = root.value("videoParameters").toObject();
        videoParameters["system"] = options.system;
        root["videoParameters"] = videoParameters;
        capture.metadataJson = QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    const QString name = !options.name.isEmpty() ? options.name : QFileInfo(input).fileName();
    Ldx::Codec videoCodec, audioCodec;
    const Ldx::Kind videoKind = capture.cvbs ? Ldx::Kind::Cvbs : Ldx::Kind::Tbc;
    if (!codecFor(options.codec, videoKind, videoCodec) || !codecFor(options.codec, Ldx::Kind::Pcm, audioCodec)) {
        err << "The codec is raw, flac, ffv1 or auto, and not " << options.codec << "\n";
        return false;
    }
    capture.video.kind = videoKind;
    capture.video.depth = capture.cvbs ? 10 : 16;
    capture.video.codec = videoCodec;
    capture.video.name = name;
    capture.video.parameters["system"] = !options.system.isEmpty() ? options.system : videoSystemFileName(video.system, video.systemVariant);
    capture.video.parameters["sampleRate"] = video.sampleRate;
    capture.video.parameters["fieldWidth"] = video.fieldWidth;
    capture.video.parameters["fieldHeight"] = video.fieldHeight;
    // What a person said of the capture (section 3.1)
    if (!options.medium.isEmpty()) capture.video.parameters["medium"] = options.medium;
    if (!options.reference.isEmpty()) capture.video.parameters["reference"] = options.reference;
    if (options.copy > 0) capture.video.parameters["copy"] = options.copy;
    if (options.captureNumber > 0) capture.video.parameters["capture"] = options.captureNumber;
    if (!options.face.isEmpty()) capture.video.parameters["face"] = options.face;
    // The active area of the capture, as the metadata say it (the shape of the video track in its header does not use it; it is what a tool shows the active area with): the same
    // parameters ld-analyse shows the active area with (the shape is only written when it is chosen: ld-container display, or ldx-packer)
    if (video.activeVideoStart >= 0 && video.activeVideoEnd > video.activeVideoStart) {
        capture.video.parameters["activeVideoStart"] = video.activeVideoStart;
        capture.video.parameters["activeVideoEnd"] = video.activeVideoEnd;
        capture.video.parameters["firstActiveFieldLine"] = video.firstActiveFieldLine;
        capture.video.parameters["lastActiveFieldLine"] = video.lastActiveFieldLine;
        capture.video.parameters["firstActiveFrameLine"] = video.firstActiveFrameLine;
        capture.video.parameters["lastActiveFrameLine"] = video.lastActiveFrameLine;
    }
    if (capture.cvbs) {
        // What a frame is (the length of a line, and its samples) is what the codec needs, and what is needed to give the file back
        capture.video.parameters["lineWidth"] = geometry.lineWidth;
        capture.video.parameters["frameSamples"] = static_cast<double>(geometry.frameSamples);
        capture.video.parameters["sampleEncoding"] = Cvbs::sampleEncodingName(encoding);
        capture.video.parameters["padValue"] = Cvbs::standardLevels(video.system).blanking;
    }

    capture.metadata.kind = Ldx::Kind::Metadata;
    capture.metadata.depth = 0;
    capture.metadata.name = name + " metadata";
    capture.metadata.parameters["namespace"] = "ldx.tbcmetadata";
    capture.metadata.parameters["format"] = "ld-decode-json";

    // The audio of a CVBS capture is a 24-bit WAV (<name>_audio_0.wav, taken when it is there), the audio of a TBC a .pcm
    QString wavName;
    if (capture.cvbs) {
        if (!options.pcm.isEmpty() && options.pcm.endsWith(".wav", Qt::CaseInsensitive)) wavName = options.pcm;
        else if (options.pcm.isEmpty() && QFileInfo::exists(CvbsAudio::wavFileName(Cvbs::baseName(input), 0))) wavName = CvbsAudio::wavFileName(Cvbs::baseName(input), 0);
    }
    capture.hasAudio = !options.pcm.isEmpty() || !wavName.isEmpty();
    if (!wavName.isEmpty()) {
        capture.wav = std::make_shared<CvbsAudio::WavReader>();
        if (!capture.wav->open(wavName)) {
            err << "Could not read the audio " << wavName << "\n";
            return false;
        }
        capture.pcmRate = capture.wav->info().sampleRate;
        capture.audio.kind = Ldx::Kind::Pcm;
        capture.audio.depth = 24;
        capture.audio.codec = Ldx::Codec::Raw;      // (FLAC is for the tracks of 16 bits here)
        capture.audio.name = QFileInfo(wavName).fileName();
        capture.audio.parameters["sampleRate"] = capture.pcmRate;
        capture.audio.parameters["channels"] = 2;
        capture.audio.parameters["bits"] = 24;
    } else if (capture.hasAudio) {
        capture.pcm.reset(new QFile(options.pcm));

        if (!capture.pcm->open(QIODevice::ReadOnly)) {
            err << "Could not open " << options.pcm << "\n";
            return false;
        }
        capture.pcmRate = options.pcmRate;
        capture.audio.kind = Ldx::Kind::Pcm;
        capture.audio.depth = 16;
        capture.audio.codec = audioCodec;
        capture.audio.name = QFileInfo(capture.pcm->fileName()).fileName();
        capture.audio.parameters["sampleRate"] = capture.pcmRate;
        capture.audio.parameters["channels"] = 2;
    }
    return true;
}

// The chroma file of a Y/C capture (_chroma.tbc beside a .tbc, .tbcc beside a .tbcy), which goes in the file as a track about
// the luma one: a `tbc` track whose parameters say "role": "chroma" and "track": the id of the luma
struct ChromaPart {
    Capture capture;
    bool present = false;
    std::unique_ptr<BlockSource> source;
};

bool readChroma(const CaptureOptions &options, const QString &input, ChromaPart &part)
{
    if (Cvbs::isDataFileName(input)) {
        // .cvbsy with .cvbsc beside it; a .cvbs is composite and has none
        const QString partner = Cvbs::pairedDataFileName(input);
        if (partner.isEmpty() || Cvbs::channel(input) != Cvbs::Channel::Luma) return true;
        if (!QFileInfo::exists(partner)) {
            err << "The chroma file " << partner << " of the Y/C capture " << input << " does not exist\n";
            return false;
        }
        CaptureOptions chromaOptions = options;
        chromaOptions.asChroma = true;
        if (!readCapture(chromaOptions, partner, part.capture)) return false;
        part.present = true;
        return true;
    }
    if (TbcFiles::isChromaFile(input)) {
        err << input << " is the chroma file of a Y/C capture: pack its luma file " << TbcFiles::lumaFileName(input) << ", which takes it\n";
        return false;
    }
    const QString path = TbcFiles::chromaFileName(input);
    if (path.isEmpty() || path == input || Cvbs::isDataFileName(input)) return true;
    if (!readCapture(options, path, part.capture)) return false;
    part.present = true;
    return true;
}

void addChromaTrack(Ldx::Writer &writer, ChromaPart &part, quint32 luma)
{
    if (!part.present) return;
    part.capture.video.name += " (chroma)";
    part.capture.video.parameters["role"] = "chroma";
    part.capture.video.parameters["track"] = static_cast<int>(luma);
    writer.addTrack(part.capture.video);
}

void addChromaSource(ChromaPart &part, const Capture &luma, std::vector<BlockSource *> &sources)
{
    if (!part.present) return;
    part.source = std::make_unique<FieldSource>(*part.capture.tbc, part.capture.video.number, part.capture.system, part.capture.unitBytes,
                                                luma.start, luma.fields, part.capture.unitFields, part.capture.convert);
    sources.push_back(part.source.get());
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
        const QJsonObject object = value.toObject();
        const int track = object.value("track").toInt(0);
        if (track > 0 && object.value("file").toString().isEmpty()) tracks.insert(static_cast<quint32>(track));
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
        const QJsonObject object = value.toObject();
        if (object.value("file").toString().isEmpty() && tracks.contains(static_cast<quint32>(object.value("track").toInt(0)))) { taken++; continue; }
        sources.append(value);
    }
    root["sources"] = sources;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// The standard input and output are a pipe: "-" for a file
bool isStdio(const QString &name)
{
    return name == "-";
}

// What goes to the standard output is the data of a pipe: the messages go to the standard error then
void messagesToStderr()
{
    static QFile standardError;
    if (!standardError.isOpen()) standardError.open(stderr, QIODevice::WriteOnly);
    out.setDevice(&standardError);
}

bool openReader(Ldx::Reader &reader, const QString &name, QString &error)
{
    return isStdio(name) ? reader.openStream(stdin, error) : reader.open(name, error);
}

bool openWriter(Ldx::Writer &writer, const QString &name, QString &error)
{
    if (isStdio(name)) {
        messagesToStderr();
        return writer.openStream(stdout, error);
    }
    return writer.open(name, error);
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
    if (!openReader(rewrite.reader, fileName, error)) {
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
    // What was added at the end of the file by changes made in its place (the history, the tracks as they are now, the attachments): they are read with the index, by another
    // reader, since this one goes on to read the blocks in order. A pipe is read once, and has none of it.
    if (!isStdio(fileName)) {
        Ldx::Reader current;
        QString problem;
        if (current.open(fileName, problem) && current.buildIndex(problem)) {
            rewrite.tracks = current.tracks();
            rewrite.attachments = current.attachments();
            rewrite.history = current.history();
        }
    }
    return true;
}

void prepareWriter(Rewrite &rewrite, Ldx::Writer &writer, const QSet<quint32> &skip, bool withAttachments = true)
{
    for (Ldx::Track track: rewrite.tracks) {
        if (skip.contains(track.number)) continue;
        writer.addTrack(track);
    }
    writer.setNextTrackId(rewrite.reader.nextTrackId());
    if (!withAttachments) return;
    for (const Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        writer.addAttachment(attachment.name, attachment.mediaType, attachment.data);
    }
}

bool sameFile(const QString &a, const QString &b)
{
    if (isStdio(a) || isStdio(b)) return false;
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
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("input", "The .tbc file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("efm", "The EFM t-values of the capture (.efm) to add", "file"));
    parser.addOption(QCommandLineOption("ac3", "The AC3 data of the capture to add", "file"));
    parser.addOption(QCommandLineOption("live-metadata", "Send the metadata field by field, each just before its video, as a producer does that does not have them all at the start"));
    addCaptureOptions(parser);
    parser.process(arguments);
    processStandardDebugOptions(parser);
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
    CaptureOptions captured = captureOptions(parser);
    if (captured.invalid) return 1;
    captured.live = parser.isSet("live-metadata");
    if (!readCapture(captured, input, capture)) return 1;
    ChromaPart chroma;
    if (!parser.isSet("no-chroma") && !readChroma(captured, input, chroma)) return 1;

    Ldx::Writer writer;
    addCaptureTracks(writer, capture);
    addChromaTrack(writer, chroma, capture.video.number);

    // What goes with the video as bytes: EFM and AC3
    struct ByteTrack {
        QString option;
        Ldx::Kind kind;
        Ldx::Track track;
        std::unique_ptr<QFile> file;
    };
    std::vector<ByteTrack> byteTracks;
    byteTracks.push_back({ "efm", Ldx::Kind::Efm, {}, nullptr });
    byteTracks.push_back({ "ac3", Ldx::Kind::Ac3, {}, nullptr });
    for (ByteTrack &bytes: byteTracks) {
        if (!parser.isSet(bytes.option)) continue;
        bytes.file.reset(new QFile(parser.value(bytes.option)));
        if (!bytes.file->open(QIODevice::ReadOnly)) {
            err << "Could not open " << bytes.file->fileName() << "\n";
            return 1;
        }
        bytes.track.kind = bytes.kind;
        bytes.track.depth = 8;
        bytes.track.name = QFileInfo(bytes.file->fileName()).fileName();
        bytes.track.parameters["track"] = static_cast<int>(capture.video.number);
        writer.addTrack(bytes.track);
    }
    writer.setHistory(QJsonArray { historyEntry("pack", QFileInfo(input).fileName()) });

    QString error;
    if (!openWriter(writer, output, error)) {
        err << error << "\n";
        return 1;
    }

    OnceSource metadata(capture.metadata.number, capture.metadataJson);
    FieldSource fields(*capture.tbc, capture.video.number, capture.system, capture.unitBytes, capture.start, capture.fields, capture.unitFields, capture.convert);
    PcmSource pcm(capture.pcm ? *capture.pcm : *capture.tbc, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    std::unique_ptr<Wav24Source> wav;
    if (capture.wav) wav = std::make_unique<Wav24Source>(capture.wav, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    FieldMetadataSource fieldMetadata(capture.metadata.number, capture.fieldJson, capture.system, capture.start, capture.unitFields);
    std::vector<BlockSource *> sources { &metadata, &fieldMetadata, &fields };
    addChromaSource(chroma, capture, sources);
    if (wav) sources.push_back(wav.get());
    else if (capture.hasAudio) sources.push_back(&pcm);
    std::vector<std::unique_ptr<ChunkSource>> chunkSources;
    for (ByteTrack &bytes: byteTracks) {
        if (!bytes.file) continue;
        chunkSources.push_back(std::make_unique<ChunkSource>(*bytes.file, bytes.track.number, fields.endTimeNs()));
        sources.push_back(chunkSources.back().get());
    }
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Packed " << fields.written() << " fields";
    if (capture.hasAudio) out << " and " << (wav ? wav->samples() : pcm.samples()) << " audio samples";
    for (const std::unique_ptr<ChunkSource> &chunks: chunkSources) out << ", " << chunks->bytes() << " bytes of EFM or AC3";
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
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("input", "The .tbc file to add");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("mapped-from", "The capture is the mapping (ld-discmap) of the video track with this id, which is in the file", "id"));
    addCaptureOptions(parser);
    parser.process(arguments);
    processStandardDebugOptions(parser);
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
    const CaptureOptions addOptions = captureOptions(parser);
    if (addOptions.invalid) return 1;
    if (!readCapture(addOptions, input, capture)) return 1;
    ChromaPart chroma;
    if (!parser.isSet("no-chroma") && !readChroma(addOptions, input, chroma)) return 1;

    // A mapping says which capture of the file it is the mapping of (section 7.4 of the specification): a video track that is there, and not a chroma one
    if (parser.isSet("mapped-from")) {
        bool numeric = false;
        const quint32 original = parser.value("mapped-from").toUInt(&numeric);
        bool valid = false;
        for (const Ldx::Track &track: rewrite.tracks) {
            if (track.number == original && numeric && (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs)
                && track.parameters.value("role").toString() != "chroma") valid = true;
        }
        if (!valid) {
            err << "--mapped-from " << parser.value("mapped-from") << ": there is no video track with that id in " << source << "\n";
            return 1;
        }
        capture.video.parameters["mappedFrom"] = static_cast<int>(original);
    }

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    addCaptureTracks(writer, capture);
    addChromaTrack(writer, chroma, capture.video.number);
    rewrite.history.append(historyEntry("add", QString("track %1: %2").arg(capture.video.number).arg(QFileInfo(input).fileName())
                                         + (parser.isSet("mapped-from") ? QString(" (the mapping of track %1)").arg(parser.value("mapped-from")) : QString())));
    writer.setHistory(rewrite.history);

    QString error;
    if (!openWriter(writer, output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, {});
    OnceSource metadata(capture.metadata.number, capture.metadataJson);
    FieldSource fields(*capture.tbc, capture.video.number, capture.system, capture.unitBytes, capture.start, capture.fields, capture.unitFields, capture.convert);
    PcmSource pcm(capture.pcm ? *capture.pcm : *capture.tbc, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    std::unique_ptr<Wav24Source> wav;
    if (capture.wav) wav = std::make_unique<Wav24Source>(capture.wav, capture.audio.number, capture.pcmRate, fields.endTimeNs());
    std::vector<BlockSource *> sources { &old, &metadata, &fields };
    addChromaSource(chroma, capture, sources);
    if (wav) sources.push_back(wav.get());
    else if (capture.hasAudio) sources.push_back(&pcm);
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Added " << QFileInfo(input).fileName() << " as track " << capture.video.number << " to " << output << "\n";
    return 0;
}

// ld-container addchroma file.ldx --about id --input chroma.tbc|chroma.cvbsc -o output.ldx
//
// The chroma file of a Y/C capture whose luma is in the file already (section 7.3 of the specification): a track with the role chroma, about the luma track,
// with as many units as the luma has, at the same times. The file is written again, to the output.
int addChroma(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Add the chroma file of a Y/C capture (_chroma.tbc, .tbcc or .cvbsc) to an LDX file, as the chroma track of a capture that is in it as\n"
                                     "its luma. The file is written again, to the output.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("about", "The id of the luma (video) track of the capture (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("input", "The chroma file", "file"));
    parser.addOption(QCommandLineOption("codec", "How the chroma is kept: raw (default), flac, ffv1 (CVBS) or auto", "codec", "raw"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("input") || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString input = parser.value("input");
    const QString output = parser.value("output");
    if (sameFile(source, output) || isStdio(output) || isStdio(source)) {
        err << "The output must be a file, and not the one that is read\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    int luma = parser.value("about").toInt();
    for (const Ldx::Track &track: rewrite.tracks) {
        const bool video = (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
        if (video && (luma == 0 || static_cast<int>(track.number) == luma)) { luma = static_cast<int>(track.number); break; }
    }
    const Ldx::Track *lumaTrack = nullptr;
    for (const Ldx::Track &track: rewrite.tracks) {
        if (static_cast<int>(track.number) == luma) lumaTrack = &track;
        if (track.parameters.value("role").toString() == "chroma" && track.parameters.value("track").toInt(0) == luma) {
            err << "The capture of track " << luma << " has a chroma track already (" << track.number << "): take it out first\n";
            return 1;
        }
    }
    if (!lumaTrack || (lumaTrack->kind != Ldx::Kind::Tbc && lumaTrack->kind != Ldx::Kind::Cvbs) || lumaTrack->parameters.value("role").toString() == "chroma") {
        err << "There is no video track " << parser.value("about") << " in " << source << "\n";
        return 1;
    }
    if (lumaTrack->codec == Ldx::Codec::Unknown) {
        err << "The luma track is coded in a way this program does not know\n";
        return 1;
    }
    // As many fields as the luma has
    qint64 lumaFields = 0;
    {
        Ldx::Reader counting;
        QString problem;
        if (!counting.open(source, problem) || !counting.buildIndex(problem)) {
            err << problem << "\n";
            return 1;
        }
        lumaFields = counting.blockCount(static_cast<quint32>(luma)) * (lumaTrack->kind == Ldx::Kind::Cvbs ? 2 : 1);
    }

    CaptureOptions options;
    options.codec = parser.value("codec");
    options.fields = lumaFields;
    options.asChroma = Cvbs::isDataFileName(input);
    Capture capture;
    if (!readCapture(options, input, capture)) return 1;
    if ((lumaTrack->kind == Ldx::Kind::Cvbs) != capture.cvbs) {
        err << "The chroma file " << input << " is not " << (capture.cvbs ? "TBC" : "CVBS") << " like the luma of the capture\n";
        return 1;
    }
    if (capture.fields != lumaFields) {
        err << "The chroma file has " << capture.fields << " fields and the luma of track " << luma << " has " << lumaFields << ": a chroma has as many as its luma\n";
        return 1;
    }

    ChromaPart part;
    part.capture = std::move(capture);
    part.present = true;
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    addChromaTrack(writer, part, static_cast<quint32>(luma));
    rewrite.history.append(historyEntry("addchroma", QString("%1 is the chroma of track %2").arg(QFileInfo(input).fileName()).arg(luma)));
    writer.setHistory(rewrite.history);
    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, {});
    Capture dummy;
    std::vector<BlockSource *> sources { &old };
    part.source = std::make_unique<FieldSource>(*part.capture.tbc, part.capture.video.number, part.capture.system, part.capture.unitBytes, 0, lumaFields, part.capture.unitFields,
                                                part.capture.convert);
    sources.push_back(part.source.get());
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Added " << QFileInfo(input).fileName() << " as the chroma of track " << luma << ", written to " << output << "\n";
    return 0;
}

// ld-container remove file.ldx --id x -o output.ldx
int removeStream(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Remove the track with an id from an LDX file, with the metadata about it. The file is\n"
                                     "written again, to the output. The id is not given to another track.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track to remove", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
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
        const bool chromaOf = (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() == "chroma";
        if ((track.kind == Ldx::Kind::Metadata || track.kind == Ldx::Kind::Annotations || track.kind == Ldx::Kind::Pcm || chromaOf)
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
    if (!openWriter(writer, output, error)) {
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

// ld-container stack file.ldx session.stack -o output.ldx [--embed]
//
// A source of a session names its capture in one of three ways:
//   "track": 2                         a track of this file
//   "file": "other.ldx", "track": 2    a track of another LDX file (a link)
//   "file": "capture.tbc"              a file outside this one (a link), which --embed brings in as a track
int setStack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Keep a stacking session in an LDX file. A source of the session is either a track of the\n"
                                     "file (\"track\": 2), a link to a file or to a track of another LDX file (\"file\": ..., and\n"
                                     "\"track\": ...), or, with --embed, a .tbc file brought into this file as a track.\n"
                                     "The file is written again, to the output.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("session", "The .stack file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("embed", "Bring the sources that are .tbc files into the LDX file, as tracks (the others stay links)"));
    parser.addOption(QCommandLineOption("codec", "With --embed, how the video and the audio brought in are kept: raw (default), flac, ffv1 or auto", "codec", "raw"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
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
    QJsonObject session = QJsonDocument::fromJson(sessionFile.readAll()).object();
    const QJsonArray sourceList = session.value("sources").toArray();
    if (sourceList.isEmpty()) {
        err << sessionFile.fileName() << " is not a session with sources\n";
        return 1;
    }
    const QDir sessionDirectory = QFileInfo(sessionFile.fileName()).absoluteDir();
    const QDir outputDirectory = QFileInfo(output).absoluteDir();

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    QSet<quint32> existing;
    for (const Ldx::Track &track: rewrite.tracks) existing.insert(track.number);

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {}, false);

    // The sources, one by one
    std::vector<std::unique_ptr<Capture>> embedded;
    QJsonArray newSources;
    int links = 0, tracks = 0;
    for (const QJsonValue &value: sourceList) {
        QJsonObject object = value.toObject();
        const int track = object.value("track").toInt(0);
        const QString file = object.value("file").toString();
        const QString absolute = file.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(file).isAbsolute() ? file : sessionDirectory.absoluteFilePath(file));

        if (track < 1 && file.isEmpty()) {
            err << "A source of the session has neither a file nor a track\n";
            return 1;
        }
        if (file.isEmpty()) {
            // A track of this file
            if (!existing.contains(static_cast<quint32>(track))) {
                err << "The session has a source on track " << track << ", which is not in " << source << "\n";
                return 1;
            }
            tracks++;
        } else if (track >= 1 && absolute.endsWith(".ldx", Qt::CaseInsensitive)) {
            // A track of another LDX file: it is checked if the file is there
            if (QFile::exists(absolute)) {
                Ldx::Reader other;
                QString otherError;
                bool has = false;
                if (other.open(absolute, otherError)) {
                    for (const Ldx::Track &candidate: other.tracks()) has = has || candidate.number == static_cast<quint32>(track);
                }
                if (!has) {
                    err << "The session has a source on track " << track << " of " << file << ", which has no such track\n";
                    return 1;
                }
            } else {
                err << "  note: " << file << " is not there now, so its track " << track << " cannot be checked\n";
            }
            object["file"] = outputDirectory.relativeFilePath(absolute);
            links++;
        } else if (parser.isSet("embed") && (absolute.endsWith(".tbc", Qt::CaseInsensitive) || absolute.endsWith(".cvbs", Qt::CaseInsensitive))) {
            // A .tbc or .cvbs: brought in as a track of this file, with its audio if a .pcm of the same name is beside it
            CaptureOptions options;
            options.codec = parser.value("codec");
            options.name = object.value("label").toString().isEmpty() ? QFileInfo(absolute).fileName() : object.value("label").toString();
            const QString pcm = QFileInfo(absolute).dir().filePath(QFileInfo(absolute).completeBaseName() + ".pcm");
            if (QFileInfo::exists(pcm) && !absolute.endsWith(".cvbs", Qt::CaseInsensitive)) options.pcm = pcm;
            auto capture = std::make_unique<Capture>();
            if (!readCapture(options, absolute, *capture)) return 1;
            addCaptureTracks(writer, *capture);
            object.remove("file");
            object["track"] = static_cast<int>(capture->video.number);
            out << "  embedding " << QFileInfo(absolute).fileName() << " as track " << capture->video.number << "\n";
            embedded.push_back(std::move(capture));
            tracks++;
        } else {
            // A file outside this one: a link, kept as the path from where the LDX file is
            object["file"] = outputDirectory.relativeFilePath(absolute);
            if (parser.isSet("embed")) out << "  " << QFileInfo(absolute).fileName() << " stays a link (only .tbc and .cvbs files can be embedded for now)\n";
            links++;
        }
        newSources.append(object);
    }
    session["sources"] = newSources;
    const QByteArray sessionData = QJsonDocument(session).toJson(QJsonDocument::Indented);

    bool replaced = false;
    for (Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        if (attachment.name == StackAttachment) { attachment.data = sessionData; attachment.mediaType = "application/json"; replaced = true; }
    }
    if (!replaced) rewrite.attachments.append({ StackAttachment, "application/json", sessionData });
    for (const Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        writer.addAttachment(attachment.name, attachment.mediaType, attachment.data);
    }
    rewrite.history.append(historyEntry("stack", QString("%1 sources on tracks, %2 links").arg(tracks).arg(links)));
    writer.setHistory(rewrite.history);

    QString error;
    if (!openWriter(writer, output, error)) {
        err << error << "\n";
        return 1;
    }

    // The blocks of the file, and those of the captures brought in
    LdxSource old(rewrite.reader, {});
    std::vector<std::unique_ptr<BlockSource>> owned;
    std::vector<BlockSource *> sources { &old };
    for (const std::unique_ptr<Capture> &capture: embedded) {
        owned.push_back(std::make_unique<OnceSource>(capture->metadata.number, capture->metadataJson));
        sources.push_back(owned.back().get());
        auto fields = std::make_unique<FieldSource>(*capture->tbc, capture->video.number, capture->system, capture->unitBytes, capture->start, capture->fields, capture->unitFields, capture->convert);
        const qint64 end = fields->endTimeNs();
        sources.push_back(fields.get());
        owned.push_back(std::move(fields));
        if (capture->wav) {
            owned.push_back(std::make_unique<Wav24Source>(capture->wav, capture->audio.number, capture->pcmRate, end));
            sources.push_back(owned.back().get());
        } else if (capture->hasAudio) {
            owned.push_back(std::make_unique<PcmSource>(*capture->pcm, capture->audio.number, capture->pcmRate, end));
            sources.push_back(owned.back().get());
        }
    }
    if (!mergeInto(writer, sources) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "The session is kept in " << output << " (" << tracks << " sources on tracks, " << links << " links)\n";
    return 0;
}

// ld-container info file.ldx
int info(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Show what an LDX file holds");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1) {
        err << parser.helpText();
        return 1;
    }

    Ldx::Reader reader;
    QString error;
    if (!openReader(reader, parser.positionalArguments().first(), error)) {
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

    qint64 holds = 0;
    for (const qint64 size: bytes) holds += size;
    const qint64 fileSize = isStdio(parser.positionalArguments().first()) ? 0 : QFileInfo(parser.positionalArguments().first()).size();
    out << "File: " << fileSize << " bytes for " << holds << " bytes of data";
    if (holds > 0 && fileSize > 0) out << " (" << QString::number(100.0 * static_cast<double>(fileSize) / static_cast<double>(holds), 'f', 1) << " %)";
    out << "\n";
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

// ld-container addtrack file.ldx --kind pcm|efm|ac3|annotations --input data [--about id] [--name n] [--rate hz] [--source efm]
int addTrack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Put a file in an LDX file as a track of its own: the audio decoded from EFM (pcm, 16-bit stereo, with --source efm),\n"
                                     "AC3 data, EFM t-values or a text of annotations. The file is written again, in its place (or to the output).");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("kind", "pcm, efm, ac3 or annotations", "kind"));
    parser.addOption(QCommandLineOption("input", "The file to put in", "file"));
    parser.addOption(QCommandLineOption("about", "The id of the video track it goes with (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("name", "The name of the track (default: the name of the file)", "name"));
    parser.addOption(QCommandLineOption("rate", "Sample rate of pcm audio (default 44100)", "hertz", "44100"));
    parser.addOption(QCommandLineOption("source", "For pcm: where the audio comes from, efm for the digital audio decoded from EFM (default: the analogue audio)", "source"));
    parser.addOption(QCommandLineOption("codec", "For pcm: raw (default) or flac", "codec"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write to another file instead of in place", "file"));
    parser.addOption(QCommandLineOption("update", "For annotations: add the notes in the file itself, in the room the list of tracks has (no copy of the file, stamped after everything in it)"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("kind") || !parser.isSet("input")) {
        err << parser.helpText();
        return 1;
    }
    QString target = parser.positionalArguments().first();
    if (parser.isSet("output")) {
        if (!QFile::copy(target, parser.value("output"))) {
            err << "Could not write " << parser.value("output") << " (it exists already?)\n";
            return 1;
        }
        target = parser.value("output");
    }
    if (parser.isSet("update")) {
        if (parser.value("kind") != "annotations") {
            err << "--update is for annotations: the other tracks have data all along the capture, and the file is written again\n";
            return 1;
        }
        QFile notes(parser.value("input"));
        if (!notes.open(QIODevice::ReadOnly)) {
            err << "Could not open " << parser.value("input") << "\n";
            return 1;
        }
        QString problem;
        if (!LdxCapture::addNotesInPlace(target, notes.readAll(), parser.isSet("name") ? parser.value("name") : QFileInfo(parser.value("input")).fileName(), parser.value("about").toInt(),
                                         historyEntry("addtrack", QString("%1 as a annotations track").arg(QFileInfo(parser.value("input")).fileName())), problem)) {
            err << problem << "\n";
            if (parser.isSet("output")) QFile::remove(target);
            return 1;
        }
        out << "Added " << QFileInfo(parser.value("input")).fileName() << " as a annotations track to " << target << "\n";
        return 0;
    }
    QJsonObject parameters;
    if (parser.value("kind") == "pcm") {
        parameters["sampleRate"] = parser.value("rate").toDouble();
        parameters["channels"] = 2;
        if (parser.isSet("source")) parameters["source"] = parser.value("source");
    }
    const QString input = parser.value("input");
    QString error;
    if (!LdxCapture::addTrackFromFile(target, parser.value("kind"), input, parameters,
                                      parser.isSet("name") ? parser.value("name") : QFileInfo(input).fileName(), parser.value("about").toInt(),
                                      historyEntry("addtrack", QString("%1 as a %2 track").arg(QFileInfo(input).fileName(), parser.value("kind"))), error,
                                      parser.value("codec"))) {
        err << error << "\n";
        return 1;
    }
    out << "Added " << QFileInfo(input).fileName() << " as a " << parser.value("kind") << " track to " << target << "\n";
    return 0;
}

// ld-container wav file.ldx [-o out.wav] [--id n] [--source analogue|efm]
int wav(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write the audio of an LDX file as a WAV file: the analogue audio of the capture (16-bit, or 24-bit for a CVBS capture),\n"
                                     "or with --source efm the digital audio decoded from EFM.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The WAV file (default: the name of the LDX file, .wav)", "file"));
    parser.addOption(QCommandLineOption("id", "The id of the pcm track (default: the first that fits)", "id", "0"));
    parser.addOption(QCommandLineOption("source", "analogue (default) or efm", "source", "analogue"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1) {
        err << parser.helpText();
        return 1;
    }
    const QString file = parser.positionalArguments().first();
    Ldx::Reader reader;
    QString error;
    if (!openReader(reader, file, error)) {
        err << error << "\n";
        return 1;
    }
    const bool wantEfm = parser.value("source") == "efm";
    Ldx::Track chosen;
    bool found = false;
    for (const Ldx::Track &track: reader.tracks()) {
        if (track.kind != Ldx::Kind::Pcm) continue;
        if (parser.value("id").toUInt() != 0) {
            if (track.number != parser.value("id").toUInt()) continue;
        } else if ((track.parameters.value("source").toString() == "efm") != wantEfm) {
            continue;
        }
        chosen = track;
        found = true;
        break;
    }
    if (!found) {
        err << file << " has no " << (wantEfm ? "audio decoded from EFM" : "analogue audio") << "\n";
        return 1;
    }
    const int bits = chosen.depth == 24 ? 24 : 16;
    const quint32 rate = static_cast<quint32>(chosen.parameters.value("sampleRate").toDouble(44100));
    const QString output = parser.isSet("output") ? parser.value("output") : QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + ".wav");
    QFile wavFile(output);
    if (!wavFile.open(QIODevice::WriteOnly)) {
        err << "Could not write " << output << "\n";
        return 1;
    }
    // The header, with the sizes put in when the data are written
    auto put32 = [](quint32 value) { QByteArray b(4, 0); for (int i = 0; i < 4; i++) b[i] = static_cast<char>((value >> (8 * i)) & 0xFF); return b; };
    auto put16 = [](quint16 value) { QByteArray b(2, 0); for (int i = 0; i < 2; i++) b[i] = static_cast<char>((value >> (8 * i)) & 0xFF); return b; };
    QByteArray header;
    header += "RIFF" + put32(0) + "WAVEfmt " + put32(16) + put16(1) + put16(2) + put32(rate) + put32(rate * 2 * (bits / 8)) + put16(static_cast<quint16>(2 * (bits / 8))) + put16(static_cast<quint16>(bits)) + "data" + put32(0);
    wavFile.write(header);
    quint32 dataBytes = 0;
    Ldx::Block block;
    while (reader.nextBlock(block)) {
        if (block.track != chosen.number) continue;
        wavFile.write(block.data);
        dataBytes += static_cast<quint32>(block.data.size());
    }
    wavFile.seek(4);
    wavFile.write(put32(36 + dataBytes));
    wavFile.seek(40);
    wavFile.write(put32(dataBytes));
    wavFile.close();
    out << "Wrote " << dataBytes / (2 * (bits / 8)) << " samples of " << bits << "-bit audio at " << rate << " Hz to " << output << "\n";
    return 0;
}

// ld-container history file.ldx [--json]
int history(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Show the history of an LDX file: what was done to it, by which tool, when, and with what arguments");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file (- for a stream)");
    parser.addOption(QCommandLineOption("json", "Write the history as a JSON array, for another program"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1) {
        err << parser.helpText();
        return 1;
    }
    Ldx::Reader reader;
    QString error;
    if (!openReader(reader, parser.positionalArguments().first(), error)) {
        err << error << "\n";
        return 1;
    }
    // (what was added to a file at its end is read with the index)
    if (!isStdio(parser.positionalArguments().first())) reader.buildIndex(error);
    const QJsonArray entries = reader.history();
    if (parser.isSet("json")) {
        out << QJsonDocument(entries).toJson(QJsonDocument::Indented);
        return 0;
    }
    if (entries.isEmpty()) out << "The history is empty\n";
    int number = 0;
    for (const QJsonValue &value: entries) {
        const QJsonObject entry = value.toObject();
        out << ++number << ". " << entry.value("time").toString() << "  " << entry.value("tool").toString();
        if (entry.contains("version")) out << " (" << entry.value("version").toString() << ")";
        if (entry.contains("via")) out << " by " << entry.value("via").toString();
        out << "  " << entry.value("command").toString();
        if (entry.contains("detail")) out << ": " << entry.value("detail").toString();
        out << "\n";
        if (entry.contains("arguments")) {
            QStringList arguments;
            for (const QJsonValue &argument: entry.value("arguments").toArray()) arguments << argument.toString();
            out << "     " << arguments.join(' ') << "\n";
        }
    }
    return 0;
}

// ld-container extract file.ldx --id 3 [-o file]
int extractTrack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write the data of one track of an LDX file as it is: to the output file, or to the standard output, to\n"
                                     "be piped to a tool (for instance the EFM of a capture to ld-process-efm).");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The file to write (the standard output if omitted)", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("id")) {
        err << parser.helpText();
        return 1;
    }
    const quint32 id = parser.value("id").toUInt();

    Ldx::Reader reader;
    QString error;
    if (!openReader(reader, parser.positionalArguments().first(), error)) {
        err << error << "\n";
        return 1;
    }
    const Ldx::Track *chosen = nullptr;
    const QVector<Ldx::Track> tracks = reader.tracks();
    for (const Ldx::Track &track: tracks) {
        if (track.number == id) chosen = &track;
    }
    if (!chosen) {
        err << "There is no track " << id << " in " << parser.positionalArguments().first() << "\n";
        return 1;
    }

    QFile file;
    if (parser.isSet("output")) {
        file.setFileName(parser.value("output"));
        if (!file.open(QIODevice::WriteOnly)) {
            err << "Could not write " << file.fileName() << "\n";
            return 1;
        }
    } else {
#ifdef Q_OS_WIN
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        if (!file.open(stdout, QIODevice::WriteOnly)) {
            err << "Could not write to the standard output\n";
            return 1;
        }
    }

    // The metadata of a capture: the last block is the current one. Anything else, all its blocks in turn.
    const bool lastOnly = chosen->kind == Ldx::Kind::Metadata;
    LdxMetadataMerger last;
    Ldx::Block block;
    while (reader.nextBlock(block)) {
        if (block.track != id) continue;
        if (lastOnly) last.add(block.data); else if (file.write(block.data) != block.data.size()) {
            err << "Could not write the data\n";
            return 1;
        }
    }
    if (lastOnly) file.write(last.current());
    file.flush();
    return 0;
}

// The blocks of a file with the numbers of their tracks changed
class RenumberSource : public BlockSource
{
public:
    RenumberSource(Ldx::Reader &reader, const QMap<quint32, quint32> &numbers, const QSet<quint32> &skip = {}) : inner(reader, skip), numbers(numbers) {}
    bool peek(Ldx::Block &block) override
    {
        if (!inner.peek(block)) return false;
        block.track = numbers.value(block.track, block.track);
        return true;
    }
    void pop() override { inner.pop(); }

private:
    LdxSource inner;
    QMap<quint32, quint32> numbers;
};

// The session of a file with the tracks of this file that it names numbered again (the links to other files are not touched)
QByteArray sessionRenumbered(const QByteArray &session, const QMap<quint32, quint32> &numbers)
{
    QJsonObject root = QJsonDocument::fromJson(session).object();
    QJsonArray sources;
    QSet<QString> local;
    for (const QJsonValue &value: root.value("sources").toArray()) {
        QJsonObject object = value.toObject();
        if (object.value("file").toString().isEmpty() && object.contains("track")) {
            object["track"] = static_cast<int>(numbers.value(static_cast<quint32>(object.value("track").toInt(0)), static_cast<quint32>(object.value("track").toInt(0))));
            local.insert(object.value("id").toString());
        }
        sources.append(object);
    }
    root["sources"] = sources;
    // The streams copied from a source that is a track of this file are tracks of this file
    QJsonObject stacking = root.value("stacking").toObject();
    if (stacking.contains("copyStreams")) {
        QJsonArray streams;
        for (const QJsonValue &value: stacking.value("copyStreams").toArray()) {
            QJsonObject entry = value.toObject();
            if (local.contains(entry.value("source").toString())) {
                const quint32 track = static_cast<quint32>(entry.value("track").toInt(0));
                entry["track"] = static_cast<int>(numbers.value(track, track));
            }
            streams.append(entry);
        }
        stacking["copyStreams"] = streams;
        root["stacking"] = stacking;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ld-container merge file.ldx other.ldx [--id n ...] [--exclude n ...] [--name text] [--codec c] -o output.ldx
//
// The captures of another LDX file are put in this one, each with the tracks that go with it (its chroma, metadata, audio, EFM, AC3, notes): they are given ids
// of their own, and what names a track (the tracks about a capture, the mapping of a capture) follows. The session and the other attachments of the other file are
// not taken.
int mergeCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Put the captures of another LDX file in this one, each with its tracks (the chroma, the metadata, the audio, the EFM, the AC3 and the\n"
                                     "notes about it). They get ids of their own, and what names a track follows. The session and the attachments of the other file\n"
                                     "are not taken. The file is written again, to the output.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("other", "The LDX file whose captures are put in it");
    parser.addOption(QCommandLineOption("id", "The id of a capture of the other file to take (several times; default: all of them)", "id"));
    parser.addOption(QCommandLineOption("exclude", "The id of a track of the other file to leave out (several times)", "id"));
    parser.addOption(QCommandLineOption("name", "The new name of the capture (when one is taken)", "name"));
    parser.addOption(QCommandLineOption("codec", "How the samples taken are kept: asis (default), raw, flac, ffv1 or auto", "codec", "asis"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 2 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().at(0);
    const QString otherName = parser.positionalArguments().at(1);
    const QString output = parser.value("output");
    if (sameFile(source, output) || sameFile(otherName, output) || isStdio(output) || isStdio(source) || isStdio(otherName)) {
        err << "The output must be a file of its own, and the files that are read are not the standard input\n";
        return 1;
    }
    const QString asked = parser.value("codec");
    Ldx::Codec ignored;
    if (asked != "asis" && !codecFor(asked, Ldx::Kind::Tbc, ignored)) {
        err << "The codec is asis, raw, flac, ffv1 or auto\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    Rewrite other;
    if (!openForRewrite(other, otherName)) return 1;
    const auto isVideo = [](const Ldx::Track &track) {
        return (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
    };
    QList<quint32> videos;
    for (const Ldx::Track &track: other.tracks) {
        if (isVideo(track)) videos.append(track.number);
    }
    if (videos.isEmpty()) {
        err << otherName << " holds no capture\n";
        return 1;
    }
    QList<quint32> taken;
    for (const QString &text: parser.values("id")) {
        const quint32 id = text.toUInt();
        if (!videos.contains(id)) {
            err << "There is no capture " << text << " in " << otherName << " (its captures are the video tracks that are not chroma)\n";
            return 1;
        }
        if (!taken.contains(id)) taken.append(id);
    }
    if (taken.isEmpty()) taken = videos;
    if (parser.isSet("name") && taken.size() != 1) {
        err << "--name is for one capture: give --id\n";
        return 1;
    }
    QSet<quint32> excluded;
    for (const QString &text: parser.values("exclude")) excluded.insert(text.toUInt());

    // The tracks of each capture that is taken: the video, its chroma and the tracks about it (without "track" they are about the first video of the file)
    const quint32 first = videos.first();
    const auto aboutOf = [&](const Ldx::Track &track) -> quint32 {
        if (isVideo(track)) return 0;
        if (track.parameters.contains("track")) return static_cast<quint32>(track.parameters.value("track").toInt(0));
        const bool companion = track.kind == Ldx::Kind::Metadata || track.kind == Ldx::Kind::Annotations || track.kind == Ldx::Kind::Pcm
                               || track.kind == Ldx::Kind::Efm || track.kind == Ldx::Kind::Ac3;
        return companion ? first : 0;
    };
    quint32 next = rewrite.reader.nextTrackId();
    QMap<quint32, quint32> numbers;
    QList<quint32> sequence;
    for (quint32 video: taken) {
        sequence.append(video);
        for (const Ldx::Track &track: other.tracks) {
            if (!isVideo(track) && aboutOf(track) == video && !excluded.contains(track.number)) sequence.append(track.number);
        }
    }
    for (quint32 old: sequence) numbers.insert(old, next++);

    QVector<Ldx::Track> added;
    for (quint32 old: sequence) {
        for (Ldx::Track track: other.tracks) {
            if (track.number != old) continue;
            const quint32 about = aboutOf(track);
            if (about != 0) track.parameters["track"] = static_cast<int>(numbers.value(about, about));
            if (track.parameters.contains("mappedFrom")) {
                const quint32 from = static_cast<quint32>(track.parameters.value("mappedFrom").toInt(0));
                if (numbers.contains(from)) track.parameters["mappedFrom"] = static_cast<int>(numbers.value(from));
                else track.parameters.remove("mappedFrom");         // (the capture it is the mapping of is not taken)
            }
            if (parser.isSet("name") && isVideo(track)) track.name = parser.value("name");
            if (asked != "asis") {
                const bool samples = track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Rf || track.kind == Ldx::Kind::Pcm || track.kind == Ldx::Kind::Cvbs;
                Ldx::Codec codec;
                if (samples && codecFor(asked, track.kind, codec, track.depth) && !(codec == Ldx::Codec::Raw && asked != "raw" && asked != "auto")) track.codec = codec;
            }
            track.number = numbers.value(old);
            track.uid = 0;
            added.append(track);
        }
    }

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    for (Ldx::Track &track: added) writer.addTrack(track);
    writer.setNextTrackId(next);
    QStringList ids;
    for (quint32 video: taken) ids << QString::number(video);
    QStringList mapping;
    for (quint32 old: sequence) mapping << QString("%1->%2").arg(old).arg(numbers.value(old));
    rewrite.history.append(historyEntry("merge", QString("the captures %1 of %2, as the tracks %3").arg(ids.join(','), QFileInfo(otherName).fileName(), mapping.join(','))));
    writer.setHistory(rewrite.history);

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    QSet<quint32> skip;
    for (const Ldx::Track &track: other.tracks) {
        if (!numbers.contains(track.number)) skip.insert(track.number);
    }
    LdxSource old(rewrite.reader, {});
    RenumberSource more(other.reader, numbers, skip);
    if (!mergeInto(writer, { &old, &more }) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Put " << taken.size() << " capture(s) of " << QFileInfo(otherName).fileName() << " in " << output << ", as the tracks:";
    for (quint32 old: sequence) out << " " << old << "->" << numbers.value(old);
    out << "\n";
    return 0;
}

// The size of a picture, from its header: a PNG (IHDR) or a JPEG (the first start of frame). False when it is neither.
bool pictureSize(const QByteArray &data, int *width, int *height, QString *mediaType)
{
    const auto *p = reinterpret_cast<const quint8 *>(data.constData());
    const int n = data.size();
    if (n >= 24 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        *width = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
        *height = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
        *mediaType = "image/png";
        return *width > 0 && *height > 0;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        int i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) { ++i; continue; }
            const int marker = p[i + 1];
            if (marker == 0xFF) { ++i; continue; }
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
            const int length = (p[i + 2] << 8) | p[i + 3];
            // (the start of frame markers, but the ones that are not: DHT, JPG, DAC)
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                *height = (p[i + 5] << 8) | p[i + 6];
                *width = (p[i + 7] << 8) | p[i + 8];
                *mediaType = "image/jpeg";
                return *width > 0 && *height > 0;
            }
            if (length < 2) return false;
            i += 2 + length;
        }
    }
    return false;
}

// ld-container cover file.ldx --set picture.png|jpg -o output.ldx | --remove -o output.ldx | --export picture
//
// The cover of the file (section 9 of the specification): a picture in the attachment cover.png or cover.jpg, at most 800 pixels high. The attachments come before the
// data, so putting, changing or taking out the cover writes the file again, to the output.
int coverCommand(const QStringList &arguments)
{
    constexpr int MaxHeight = 800;
    constexpr qint64 MaxBytes = 2 * 1024 * 1024;
    QCommandLineParser parser;
    parser.setApplicationDescription("The cover of an LDX file: a picture of the capture, the disc or the tape, kept in the attachment cover.png or cover.jpg (at most 800 pixels\n"
                                     "high, any shape). --set puts one in (a PNG or a JPEG), --remove takes it out: the file is written again, to the output. --export\n"
                                     "writes the one that is there out.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("set", "Put this PNG or JPEG picture in the file as its cover", "picture"));
    parser.addOption(QCommandLineOption("remove", "Take the cover out"));
    parser.addOption(QCommandLineOption("export", "Write the cover out to this file", "picture"));
    parser.addOption(QCommandLineOption("source", "Keep the picture outside the file: its web address (http or https) or the path of a file, absolute or relative to the folder of the LDX file", "address"));
    parser.addOption(QCommandLineOption("remove-source", "Take the reference to the picture out"));
    parser.addOption(QCommandLineOption("link", "A web address (http or https) that opens when the cover is clicked", "address"));
    parser.addOption(QCommandLineOption("remove-link", "Take the link out"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write another LDX file with the change, instead of changing this one in its place (for --set and --remove)", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    const int changes = int(parser.isSet("set")) + int(parser.isSet("remove")) + int(parser.isSet("source")) + int(parser.isSet("remove-source")) + int(parser.isSet("link")) + int(parser.isSet("remove-link"));
    const int actions = parser.isSet("export") ? (changes == 0 ? 1 : 2) : (changes > 0 ? 1 : 0);
    if (parser.positionalArguments().size() != 1 || actions != 1) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const auto isCover = [](const QString &name) {
        const QString lower = name.toLower();
        return lower == "cover.png" || lower == "cover.jpg" || lower == "cover.jpeg";
    };

    if (parser.isSet("export")) {
        QByteArray image;
        QString name, error;
        if (!LdxCapture::readCover(source, image, name, error)) {
            err << error << "\n";
            return 1;
        }
        QFile file(parser.value("export"));
        if (!file.open(QIODevice::WriteOnly) || file.write(image) != image.size()) {
            err << "Could not write " << file.fileName() << "\n";
            return 1;
        }
        out << "The cover (" << name << ", " << image.size() << " bytes) is in " << file.fileName() << "\n";
        return 0;
    }

    const QString output = parser.value("output");
    if (parser.isSet("output") && (sameFile(source, output) || isStdio(output) || isStdio(source))) {
        err << "The output must be a file, and not the one that is read\n";
        return 1;
    }
    if (isStdio(source)) {
        err << "The file is not the standard input\n";
        return 1;
    }
    QByteArray picture;
    QString mediaType;
    QString attachmentName;
    if (parser.isSet("set")) {
        QFile file(parser.value("set"));
        if (!file.open(QIODevice::ReadOnly)) {
            err << "Could not open " << file.fileName() << "\n";
            return 1;
        }
        picture = file.readAll();
        const QString format = Ldx::pictureFormat(picture);
        if (format.isEmpty()) {
            err << file.fileName() << " is not a JPEG, WebP, PNG, GIF or BMP picture\n";
            return 1;
        }
        int width = 0, height = 0;
        const bool known = (format == "png" || format == "jpeg") && pictureSize(picture, &width, &height, &mediaType);
        if (known && height <= MaxHeight && picture.size() <= MaxBytes) {
            // a PNG or a JPEG that fits is kept as it is
            attachmentName = mediaType == "image/png" ? "cover.png" : "cover.jpg";
        } else {
            // any other format, or one that is too big: read, scaled down to at most 800 pixels high (keeping its shape) and written as a JPEG (a photograph:
            // JPEG and WebP) or a PNG, by ffmpeg
            Ldx::Picture decoded;
            QString problem;
            if (format != "png" && !Ldx::pictureAvailable()) {
                err << file.fileName() << ": " << (known ? QString("it is higher than %1 pixels or bigger than %2 bytes: scale it down (this build has no ffmpeg to do it)").arg(MaxHeight).arg(MaxBytes)
                                                          : QString("a %1 picture is turned into a PNG or a JPEG by ffmpeg, which this build does not have").arg(format)) << "\n";
                return 1;
            }
            if (!Ldx::decodePicture(picture, decoded, problem)) {
                err << file.fileName() << ": " << problem << "\n";
                return 1;
            }
            const bool jpeg = (format == "jpeg" || format == "webp") && Ldx::pictureAvailable();
            int target = std::min(decoded.height, MaxHeight);
            for (;;) {
                const Ldx::Picture scaled = Ldx::scaledToHeight(decoded, target);
                QByteArray encoded;
                if (!Ldx::encodePicture(scaled, jpeg, 88, encoded, problem)) {
                    err << file.fileName() << ": " << problem << "\n";
                    return 1;
                }
                picture = encoded;
                if (picture.size() <= MaxBytes || target <= 200) break;
                target = target * 4 / 5;
            }
            mediaType = jpeg ? "image/jpeg" : "image/png";
            attachmentName = jpeg ? "cover.jpg" : "cover.png";
            out << "The picture is a " << format << ": it is kept as a " << (jpeg ? "JPEG" : "PNG") << ", " << picture.size() << " bytes\n";
        }
        if (picture.size() > MaxBytes) {
            err << "The picture is " << picture.size() << " bytes: a cover is a megabyte or so, at most " << MaxBytes << "\n";
            return 1;
        }
    }

    // What is to be done to the attachments of the cover (an attachment with no data is taken out): the picture, the reference to a picture kept outside the file, the link
    QVector<LdxCapture::AttachmentEdit> edits;
    QStringList said;
    const auto isTextTarget = [](const QString &name) { return name == "cover.url" || name == "link.url"; };
    if (parser.isSet("remove") || parser.isSet("set")) {
        // (the picture that is there goes, whatever its name: the new one may be of another format)
        for (const QString &name : { QString("cover.png"), QString("cover.jpg"), QString("cover.jpeg") }) edits.append({ name, QString(), QByteArray() });
        if (parser.isSet("set")) edits.append({ attachmentName, mediaType, picture });
        said << (parser.isSet("set") ? QString("%1 is the cover").arg(QFileInfo(parser.value("set")).fileName()) : QString("the cover is taken out"));
    }
    if (parser.isSet("source")) {
        const QString text = parser.value("source").trimmed();
        if (text.isEmpty() || text.contains('\n')) {
            err << "--source is the address of a picture (http or https) or the path of a file, in one line\n";
            return 1;
        }
        if (text.contains("://") && !text.startsWith("http://", Qt::CaseInsensitive) && !text.startsWith("https://", Qt::CaseInsensitive) && !text.startsWith("file://", Qt::CaseInsensitive)) {
            err << "--source: a web address is http or https\n";
            return 1;
        }
        edits.append({ "cover.url", "text/plain", text.toUtf8() });
        said << QString("the picture of the cover is at %1").arg(text);
    }
    if (parser.isSet("remove-source")) {
        edits.append({ "cover.url", QString(), QByteArray() });
        said << QString("the reference to the picture is taken out");
    }
    if (parser.isSet("link")) {
        const QString text = parser.value("link").trimmed();
        if (!text.startsWith("http://", Qt::CaseInsensitive) && !text.startsWith("https://", Qt::CaseInsensitive)) {
            err << "--link is a web address: http or https\n";
            return 1;
        }
        edits.append({ "link.url", "text/plain", text.toUtf8() });
        said << QString("the link of the cover is %1").arg(text);
    }
    if (parser.isSet("remove-link")) {
        edits.append({ "link.url", QString(), QByteArray() });
        said << QString("the link is taken out");
    }
    Q_UNUSED(isTextTarget);

    // With no output: in the file itself, the attachments written at the end of it (the last of a name counts, section 9), so it is not written again
    if (!parser.isSet("output")) {
        QString problem;
        if (!LdxCapture::putAttachmentsInPlace(source, edits, historyEntry("cover", said.join("; ")), problem)) {
            err << problem << "\n";
            return 1;
        }
        out << said.join("; ") << ", in " << source << "\n";
        return 0;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    bool changed = false;
    for (const LdxCapture::AttachmentEdit &edit : edits) {
        for (int i = rewrite.attachments.size() - 1; i >= 0; --i) {
            if (rewrite.attachments[i].name.toLower() == edit.name) {
                rewrite.attachments.removeAt(i);
                changed = true;
            }
        }
        if (!edit.data.isEmpty()) {
            Ldx::Reader::Attachment attachment;
            attachment.name = edit.name;
            attachment.mediaType = edit.mediaType;
            attachment.data = edit.data;
            rewrite.attachments.append(attachment);
            changed = true;
        }
    }
    if (!changed) {
        err << source << " has nothing of that to take out\n";
        return 1;
    }
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("cover", said.join("; ")));
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
    out << said.join("; ") << ", written to " << output << "\n";
    return 0;
}

// ld-container reorder file.ldx --order 3,1,2 -o output.ldx: the captures in another order, the tracks numbered again to follow it
int reorder(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write an LDX file again with its captures in another order. The tracks are numbered again from 1 in the new order: each\n"
                                     "capture (its video track, its chroma track, then the metadata, audio, EFM, AC3 and notes about it), and then the other\n"
                                     "tracks. What names a track (the tracks about a capture, the mapping of a capture, the sources of the session kept in the\n"
                                     "file) follows the new numbers. Links to other files are not touched, and nothing else in another file that names a track\n"
                                     "of this one will find it: it is the one command that gives an id to another track.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("order", "The ids of all the video tracks (not the chroma ones), in the order they are to have, as 3,1,2", "ids"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("order") || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output) || isStdio(source)) {
        err << "The output must not be the file that is read, and the file is not the standard input\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    const auto isVideo = [](const Ldx::Track &track) {
        return (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
    };
    QList<quint32> videos;
    for (const Ldx::Track &track: rewrite.tracks) {
        if (isVideo(track)) videos.append(track.number);
    }
    QList<quint32> order;
    for (const QString &part: parser.value("order").split(',', Qt::SkipEmptyParts)) {
        bool numeric = false;
        const quint32 id = part.trimmed().toUInt(&numeric);
        if (!numeric || !videos.contains(id) || order.contains(id)) {
            err << "--order: " << part << " is not the id of a video track of " << source << ", or it is there twice\n";
            return 1;
        }
        order.append(id);
    }
    if (order.size() != videos.size()) {
        err << "--order must name all the " << videos.size() << " video tracks of " << source << " (and only the video tracks, not the chroma ones)\n";
        return 1;
    }

    // The tracks about a capture: the chroma, and the ones that say "track": id (without it, they are about the first video track)
    const quint32 first = videos.isEmpty() ? 0 : videos.first();
    const auto aboutOf = [&](const Ldx::Track &track) -> quint32 {
        if (isVideo(track)) return 0;
        if (track.parameters.contains("track")) return static_cast<quint32>(track.parameters.value("track").toInt(0));
        const bool companion = track.kind == Ldx::Kind::Metadata || track.kind == Ldx::Kind::Annotations || track.kind == Ldx::Kind::Pcm
                               || track.kind == Ldx::Kind::Efm || track.kind == Ldx::Kind::Ac3;
        return companion ? first : 0;
    };
    QList<quint32> sequence;
    QSet<quint32> placed;
    for (quint32 video: order) {
        sequence.append(video);
        placed.insert(video);
        for (const Ldx::Track &track: rewrite.tracks) {
            if (!isVideo(track) && aboutOf(track) == video && !placed.contains(track.number)) {
                sequence.append(track.number);
                placed.insert(track.number);
            }
        }
    }
    for (const Ldx::Track &track: rewrite.tracks) {
        if (!placed.contains(track.number)) sequence.append(track.number);
    }
    QMap<quint32, quint32> numbers;
    quint32 next = 1;
    for (quint32 old: sequence) numbers.insert(old, next++);
    bool same = true;
    for (auto it = numbers.begin(); it != numbers.end(); ++it) same = same && it.key() == it.value();
    if (same) {
        out << "The captures are in that order already: nothing to change\n";
        if (!QFile::copy(source, output)) {
            err << "Could not write " << output << " (it exists already?)\n";
            return 1;
        }
        return 0;
    }

    // What names a track follows the new numbers
    QVector<Ldx::Track> tracks;
    for (quint32 old: sequence) {
        for (Ldx::Track track: rewrite.tracks) {
            if (track.number != old) continue;
            if (track.parameters.contains("track") || (!isVideo(track) && aboutOf(track) != 0)) {
                const quint32 about = aboutOf(track);
                track.parameters["track"] = static_cast<int>(numbers.value(about, about));
            }
            if (track.parameters.contains("mappedFrom")) {
                const quint32 from = static_cast<quint32>(track.parameters.value("mappedFrom").toInt(0));
                track.parameters["mappedFrom"] = static_cast<int>(numbers.value(from, from));
            }
            track.number = numbers.value(old);
            tracks.append(track);
        }
    }
    for (Ldx::Reader::Attachment &attachment: rewrite.attachments) {
        if (attachment.name == StackAttachment) attachment.data = sessionRenumbered(attachment.data, numbers);
    }

    Ldx::Writer writer;
    for (Ldx::Track &track: tracks) writer.addTrack(track);
    // (the next id only goes up)
    writer.setNextTrackId(rewrite.reader.nextTrackId());
    for (const Ldx::Reader::Attachment &attachment: rewrite.attachments) writer.addAttachment(attachment.name, attachment.mediaType, attachment.data);
    QStringList ids;
    for (quint32 video: order) ids << QString::number(video);
    rewrite.history.append(historyEntry("reorder", QString("the video tracks in the order %1, the tracks numbered again").arg(ids.join(','))));
    writer.setHistory(rewrite.history);

    QString error;
    if (!openWriter(writer, output, error)) {
        err << error << "\n";
        return 1;
    }
    RenumberSource old(rewrite.reader, numbers);
    if (!mergeInto(writer, { &old }) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    out << "Written to " << output << " with the tracks numbered again:";
    for (quint32 oldNumber: sequence) {
        if (numbers.value(oldNumber) != oldNumber) out << " " << oldNumber << "->" << numbers.value(oldNumber);
    }
    out << "\n";
    return 0;
}

// ld-container stream file.ldx -o output.ldx: the file as an LDX stream (no cues, nothing written again), to a file or to the standard output
int streamOut(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write an LDX file as an LDX stream: the segment has no size, there are no cues, and nothing is written again, as for a\n"
                                     "pipe (section 15 of the specification). The tracks, their codecs and the metadata are kept. A program that opens the\n"
                                     "result makes its index by reading it once; ld-container recompress writes it with an index again.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The stream to write (- for the standard output)", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("stream", QString("written as a stream, with no index")));
    writer.setHistory(rewrite.history);

    QString error;
    FILE *file = nullptr;
    if (isStdio(output)) {
        messagesToStderr();
        file = stdout;
    } else {
        file = fopen(QFile::encodeName(output).constData(), "wb");
        if (!file) {
            err << "Could not write " << output << "\n";
            return 1;
        }
    }
    bool ok = writer.openStream(file, error);
    if (ok) {
        LdxSource old(rewrite.reader, {});
        ok = mergeInto(writer, { &old }) && writer.finish(error);
    }
    if (file != stdout) fclose(file);
    if (!ok) {
        if (!error.isEmpty()) err << error << "\n";
        if (file != stdout) QFile::remove(output);
        return 1;
    }
    out << "Written as a stream to " << output << "\n";
    return 0;
}

// ld-container metadata file.ldx --id n [--export file.json | --input file.json|file.db] [-o output.ldx]
int metadataCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("The metadata of a capture of an LDX file: write the current ones out as JSON (--export), or replace them with those of a\n"
                                     ".json or .db file of ld-decode (--input). The new metadata are a block added at the end of the file, which are the current\n"
                                     "ones from then on (section 8.3 of the specification); the file is changed in its place, or the output is a copy with them.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the video track of the capture (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("export", "Write the current metadata of the capture as JSON to this file", "file"));
    parser.addOption(QCommandLineOption("input", "Replace the metadata with those of this .json or .db file", "file"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "With --input: write a copy of the file with the new metadata instead of changing it", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || parser.isSet("export") == parser.isSet("input")) {
        err << parser.helpText();
        return 1;
    }
    QString target = parser.positionalArguments().first();
    int id = parser.value("id").toInt();
    if (id == 0) {
        Ldx::Reader reader;
        QString error;
        if (!reader.open(target, error)) {
            err << error << "\n";
            return 1;
        }
        for (const Ldx::Track &track: reader.tracks()) {
            if ((track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma") { id = static_cast<int>(track.number); break; }
        }
    }
    QString error;
    if (parser.isSet("export")) {
        LdxCapture capture;
        QByteArray json;
        if (!capture.open(QString("%1#%2").arg(target).arg(id), error) || !capture.readMetadata(json, error)) {
            err << error << "\n";
            return 1;
        }
        QFile file(parser.value("export"));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) {
            err << "Could not write " << file.fileName() << "\n";
            return 1;
        }
        out << "The metadata of track " << id << " are in " << file.fileName() << "\n";
        return 0;
    }

    LdDecodeMetaData metadata;
    if (!metadata.read(parser.value("input"))) {
        err << "Could not read the metadata of " << parser.value("input") << "\n";
        return 1;
    }
    QTemporaryDir temporary;
    const QString jsonPath = temporary.filePath("metadata.json");
    QFile jsonFile(jsonPath);
    if (!metadata.write(jsonPath) || !jsonFile.open(QIODevice::ReadOnly)) {
        err << "Could not write the metadata out as JSON\n";
        return 1;
    }
    const QByteArray json = jsonFile.readAll();
    if (parser.isSet("output")) {
        if (sameFile(target, parser.value("output")) || !QFile::copy(target, parser.value("output"))) {
            err << "Could not write " << parser.value("output") << " (it exists already, or it is the file that is read?)\n";
            return 1;
        }
        target = parser.value("output");
    }
    if (!LdxCapture::writeMetadata(QString("%1#%2").arg(target).arg(id), json, error)) {
        err << error << "\n";
        if (parser.isSet("output")) QFile::remove(target);
        return 1;
    }
    out << "The metadata of track " << id << " are those of " << QFileInfo(parser.value("input")).fileName() << " (" << metadata.getNumberOfFields() << " fields), in " << target << "\n";
    return 0;
}

// ld-container unstack file.ldx -o output.ldx: the file without its session
int unstack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write an LDX file again without its session (the attachment session.stack). The captures it names stay in the file.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output) || isStdio(output)) {
        err << "The output must be a file, and not the one that is read\n";
        return 1;
    }
    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    bool had = false;
    for (int i = rewrite.attachments.size() - 1; i >= 0; --i) {
        if (rewrite.attachments[i].name == StackAttachment) {
            rewrite.attachments.removeAt(i);
            had = true;
        }
    }
    if (!had) {
        err << source << " has no session\n";
        return 1;
    }
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("unstack", QString("the session is taken out")));
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
    out << "The session is taken out, written to " << output << "\n";
    return 0;
}

// The metadata of a capture say isWidescreen as the shape that was chosen, as a block that is the current one from then on (when they do not already)
static bool syncWidescreen(const QString &path, int id, const QString &aspect, QString &error)
{
    if (aspect == "none") return true;
    LdxCapture capture;
    QByteArray json;
    const QString name = QString("%1#%2").arg(path).arg(id);
    if (!capture.open(name, error) || !capture.readMetadata(json, error)) return true;      // (no metadata to keep in agreement)
    QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonObject parameters = root.value("videoParameters").toObject();
    const bool wide = aspect == "16:9";
    if (!root.contains("videoParameters") || parameters.value("isWidescreen").toBool(false) == wide) return true;
    parameters["isWidescreen"] = wide;
    root["videoParameters"] = parameters;
    if (!LdxCapture::writeMetadata(name, QJsonDocument(root).toJson(QJsonDocument::Compact), error)) return false;
    out << "  isWidescreen is now " << (wide ? "true" : "false") << " in the metadata\n";
    return true;
}

// ld-container display file.ldx --id n --aspect 4:3|16:9|none -o output.ldx
int displayCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("The shape a video track of a capture is shown in by the players (VLC, ffplay, mpv): 4:3 or 16:9, the two width adjustments of ld-analyse applied to the whole image\n"
                                     "(PAL +103 or -196, NTSC +122 or -150, over the lines of a frame), or none (square pixels: 1:1). It is written in the header of the track, so the file is\n"
                                     "written again, to the output (or, with no output, changed in the file itself, in the room the list of tracks has, with no copy). With 4:3 or 16:9 the metadata of the capture get isWidescreen to match (a block added at the\n"
                                     "end, as ld-container metadata does); ld-analyse goes on reading isWidescreen as it did.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the video track of the capture (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("aspect", "4:3, 16:9 or none", "shape"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    const QString aspect = parser.value("aspect");
    if (parser.positionalArguments().size() != 1 || (aspect != "4:3" && aspect != "16:9" && aspect != "none")) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    // With no output: in the file itself, in the room the list of tracks has (no copy of the file)
    if (!parser.isSet("output")) {
        int id = parser.value("id").toInt();
        {
            Ldx::Reader reader;
            QString problem;
            if (!reader.open(source, problem)) {
                err << problem << "\n";
                return 1;
            }
            for (const Ldx::Track &track: reader.tracks()) {
                const bool video = (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
                if (video && (id == 0 || static_cast<int>(track.number) == id)) { id = static_cast<int>(track.number); break; }
            }
        }
        QString problem;
        const QString shown = aspect == "none" ? QString("1:1 (the whole image)") : aspect;
        QJsonObject changes;
        QStringList removes;
        if (aspect == "none") removes << "displayAspect";
        else changes["displayAspect"] = aspect;
        if (!LdxCapture::updateTrackInPlace(source, id, QString(), false, changes, removes, historyEntry("display", QString("track %1 shown as %2").arg(id).arg(shown)), problem)
            || !syncWidescreen(source, id, aspect, problem)) {
            err << problem << "\n";
            return 1;
        }
        out << "Track " << id << " is shown as " << shown << ", in " << source << "\n";
        return 0;
    }
    const QString output = parser.value("output");
    if (sameFile(source, output) || isStdio(output) || isStdio(source)) {
        err << "The output must be a file, and not the one that is read\n";
        return 1;
    }
    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    int id = parser.value("id").toInt();
    for (const Ldx::Track &track: rewrite.tracks) {
        const bool video = (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
        if (video && (id == 0 || static_cast<int>(track.number) == id)) { id = static_cast<int>(track.number); break; }
    }
    bool found = false;
    for (Ldx::Track &track: rewrite.tracks) {
        if (static_cast<int>(track.number) != id) continue;
        found = true;
        if (aspect == "none") track.parameters.remove("displayAspect");
        else track.parameters["displayAspect"] = aspect;
    }
    if (!found) {
        err << "There is no video track " << parser.value("id") << " in " << source << "\n";
        return 1;
    }
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("display", QString("track %1 shown as %2").arg(id).arg(aspect == "none" ? QString("1:1 (the whole image)") : aspect)));
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

    if (!syncWidescreen(output, id, aspect, error)) {
        err << "The shape is written, but not isWidescreen in the metadata: " << error << "\n";
        return 1;
    }
    out << "Track " << id << " is shown as " << (aspect == "none" ? QString("1:1 (the whole image)") : aspect) << ", written to " << output << "\n";
    return 0;
}

// The system a video track says (PAL, NTSC, PAL-M, SECAM or NTSC-VHD) is also in the metadata of its capture (videoParameters.system): a tool that reads the metadata reads it from there,
// so the two are kept to say the same (a block added at the end of the file, as ld-container metadata does). Nothing to do for a capture that has no metadata, or one that says it already.
static bool syncSystem(const QString &path, int id, const QString &system, QString &error)
{
    VideoSystem asked;
    QString variant;
    if (!parseVideoSystemVariant(system, asked, variant)) return true;
    const QString fileName = videoSystemFileName(asked, variant);
    LdxCapture capture;
    QByteArray json;
    const QString name = QString("%1#%2").arg(path).arg(id);
    if (!capture.open(name, error) || !capture.readMetadata(json, error)) {
        error.clear();
        return true;
    }
    QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonObject videoParameters = root.value("videoParameters").toObject();
    if (!root.contains("videoParameters") || videoParameters.value("system").toString() == fileName) return true;
    videoParameters["system"] = fileName;
    root["videoParameters"] = videoParameters;
    if (!LdxCapture::writeMetadata(name, QJsonDocument(root).toJson(QJsonDocument::Compact), error)) return false;
    out << "  the system is now " << fileName << " in the metadata\n";
    return true;
}

// ld-container settrack file.ldx --id n [--name text] [--set key=value ...] [--unset key ...] [-o output.ldx]
//
// The name and the parameters of a track (the sample rate of the audio, the system or the sample rate of a video, and the like). The value is JSON (a number,
// true, "text"), or text when it is not. Without an output the file itself is changed, in the room the list of tracks has (no copy of the file); a
// FLAC track whose sample rate changes is coded again, so that is done with an output.
int setTrackCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Change the name and the parameters of a track of an LDX file: the sample rate of the audio (sampleRate), the system of a video\n"
                                     "(system: PAL, NTSC or PAL-M), its sample rate, the names. The value of --set is JSON (a number, true, \"text\"), or text.\n"
                                     "Without an output the file itself is changed, in the room the list of tracks has (no copy of the file). A FLAC track whose\n"
                                     "sample rate changes is coded again, which is done with an output.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track", "id"));
    parser.addOption(QCommandLineOption("name", "The new name", "name"));
    parser.addOption(QCommandLineOption("set", "A parameter to set, as key=value (it can be given several times)", "key=value"));
    parser.addOption(QCommandLineOption("unset", "A parameter to remove (it can be given several times)", "key"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write another file with the change instead of changing this one", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("id") || (!parser.isSet("name") && !parser.isSet("set") && !parser.isSet("unset"))) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const int id = parser.value("id").toInt();
    QJsonObject changes;
    for (const QString &assignment: parser.values("set")) {
        const int equals = assignment.indexOf('=');
        if (equals < 1) {
            err << "--set takes key=value, and not " << assignment << "\n";
            return 1;
        }
        const QByteArray text = assignment.mid(equals + 1).toUtf8();
        const QJsonDocument parsed = QJsonDocument::fromJson("[" + text + "]");
        changes[assignment.left(equals)] = (parsed.isArray() && parsed.array().size() == 1) ? parsed.array().first() : QJsonValue(QString::fromUtf8(text));
    }
    // The system of a video: one of the five names (the lines are those of PAL or of NTSC for SECAM and NTSC-VHD)
    if (changes.contains("system")) {
        VideoSystem asked;
        QString variant;
        if (!parseVideoSystemVariant(changes.value("system").toString(), asked, variant)) {
            err << "The system is PAL, NTSC, PAL-M, SECAM or NTSC-VHD, and not " << changes.value("system").toString() << "\n";
            return 1;
        }
        changes["system"] = videoSystemFileName(asked, variant);
    }
    const QStringList removes = parser.values("unset");
    const QString detail = QString("track %1:%2%3%4").arg(id).arg(parser.isSet("name") ? " name " + parser.value("name") : QString())
                               .arg(changes.isEmpty() ? QString() : " set " + QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)))
                               .arg(removes.isEmpty() ? QString() : " unset " + removes.join(","));

    // What the track is now: the room of a FLAC track of audio is for the rate it was coded at
    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    int index = -1;
    for (int i = 0; i < rewrite.tracks.size(); ++i) {
        if (static_cast<int>(rewrite.tracks[i].number) == id) index = i;
    }
    if (index < 0) {
        err << "There is no track " << id << " in " << source << "\n";
        return 1;
    }
    Ldx::Track &track = rewrite.tracks[index];
    const bool rateChanges = changes.contains("sampleRate") && changes.value("sampleRate").toDouble() != track.parameters.value("sampleRate").toDouble();

    if (!parser.isSet("output")) {
        if (track.codec == Ldx::Codec::Flac && track.kind == Ldx::Kind::Pcm && rateChanges) {
            err << "The audio of track " << id << " is FLAC, coded at its sample rate: changing it needs the file to be written again (give an output)\n";
            return 1;
        }
        QString error;
        if (!LdxCapture::updateTrackInPlace(source, id, parser.value("name"), parser.isSet("name"), changes, removes, historyEntry("settrack", detail), error)) {
            err << error << "\n";
            return 1;
        }
        out << "Track " << id << " changed in " << source << "\n";
        if (changes.contains("system") && !syncSystem(source, id, changes.value("system").toString(), error)) {
            err << error << "\n";
            return 1;
        }
        return 0;
    }

    const QString output = parser.value("output");
    if (sameFile(source, output) || isStdio(output)) {
        err << "The output must be a file, and not the one that is read\n";
        return 1;
    }
    if (parser.isSet("name")) track.name = parser.value("name");
    for (auto it = changes.begin(); it != changes.end(); ++it) track.parameters[it.key()] = it.value();
    for (const QString &key: removes) track.parameters.remove(key);
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("settrack", detail));
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
    out << "Track " << id << " changed, written to " << output << "\n";
    if (changes.contains("system") && !syncSystem(output, id, changes.value("system").toString(), error)) {
        err << error << "\n";
        return 1;
    }
    return 0;
}

// The text of a value as JSON (a string stays one, even when it looks like a number): what --set of settrack takes
static QString jsonText(const QJsonValue &value)
{
    QString text = QString::fromUtf8(QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact));
    return text.mid(1, text.size() - 2);
}

// The video track of a capture: the one named, or the first that is not a chroma track (0 if there is none)
static int videoTrackOf(const QString &path, int id)
{
    Ldx::Reader reader;
    QString problem;
    if (!reader.open(path, problem)) return 0;
    for (const Ldx::Track &track: reader.tracks()) {
        const bool video = (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) && track.parameters.value("role").toString() != "chroma";
        if (video && (id == 0 || static_cast<int>(track.number) == id)) return static_cast<int>(track.number);
    }
    return 0;
}

// ld-container tag file.ldx [--id n] [--medium ld|vhs|vhd|cvbs] [--reference text] [--copy n] [--capture-number n] [--face A|B|AB|BA] [--system sys] [-o output.ldx]
//
// What a person says of a capture (section 3.1 and 8.1 of the specification): the medium, the reference, the number of the copy and of the capture, the face (parameters of
// the video track), and the system (SECAM and NTSC-VHD too: the track and the metadata of the capture say it). An empty text, or 0 for a number, takes the value away.
int tagCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Say what a capture is, as a person knows it: what was captured (--medium: ld, vhs, vhd or cvbs), a reference of the disc (--reference), the number of the\n"
                                     "physical copy (--copy) and of the capture (--capture-number), the face (--face: A, B, AB or BA) and the system (--system: PAL, NTSC, PAL-M,\n"
                                     "SECAM or NTSC-VHD: the track and the metadata of the capture both say it). An empty text, or 0 for a number, takes the value away. Without an output the file\n"
                                     "itself is changed, in the room the list of tracks has (no copy of the file).");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the video track of the capture (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("medium", "ld, vhs, vhd or cvbs", "medium"));
    parser.addOption(QCommandLineOption("reference", "A reference of the disc or the tape (a catalogue number, a title...)", "text"));
    parser.addOption(QCommandLineOption("copy", "The number of the physical copy of the disc, 1 or more (0: none)", "n"));
    parser.addOption(QCommandLineOption("capture-number", "The number of the capture, 1 or more (0: none)", "n"));
    parser.addOption(QCommandLineOption("face", "A, B, AB or BA", "face"));
    parser.addOption(QCommandLineOption("system", "PAL, NTSC, PAL-M, SECAM or NTSC-VHD (the track and the metadata of the capture both say it)", "system"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write another file with the change instead of changing this one", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    const bool any = parser.isSet("medium") || parser.isSet("reference") || parser.isSet("copy") || parser.isSet("capture-number") || parser.isSet("face") || parser.isSet("system");
    if (parser.positionalArguments().size() != 1 || !any) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const int id = videoTrackOf(source, parser.value("id").toInt());
    if (id == 0) {
        err << "There is no video track " << parser.value("id") << " in " << source << "\n";
        return 1;
    }

    // What is said, as the parameters of the track (the system is also in the metadata of the capture, which settrack keeps in agreement)
    QStringList sets, unsets;
    const auto put = [&](const QString &key, const QString &value) {
        if (value.isEmpty()) unsets << key;
        else sets << key + "=" + jsonText(value);
    };
    const auto putNumber = [&](const QString &key, const QString &option) -> bool {
        bool ok = false;
        const int number = parser.value(option).trimmed().isEmpty() ? 0 : parser.value(option).toInt(&ok);
        if (!parser.value(option).trimmed().isEmpty() && (!ok || number < 0)) {
            err << "--" << option << " is a number, 1 or more (0 takes it away)\n";
            return false;
        }
        if (number == 0) unsets << key;
        else sets << key + "=" + QString::number(number);
        return true;
    };
    QString value;
    if (parser.isSet("medium")) {
        if (!CaptureInfo::parseMedium(parser.value("medium"), value)) {
            err << "--medium is ld, vhs, vhd or cvbs\n";
            return 1;
        }
        put("medium", value);
    }
    if (parser.isSet("reference")) {
        const QString text = parser.value("reference").trimmed();
        if (text.contains('\n')) {
            err << "--reference is one line\n";
            return 1;
        }
        put("reference", text);
    }
    if (parser.isSet("copy") && !putNumber("copy", "copy")) return 1;
    if (parser.isSet("capture-number") && !putNumber("capture", "capture-number")) return 1;
    if (parser.isSet("face")) {
        if (!CaptureInfo::parseFace(parser.value("face"), value)) {
            err << "--face is A, B, AB or BA\n";
            return 1;
        }
        put("face", value);
    }
    if (parser.isSet("system")) {
        VideoSystem asked;
        QString variant;
        if (!parseVideoSystemVariant(parser.value("system"), asked, variant)) {
            err << "--system is PAL, NTSC, PAL-M, SECAM or NTSC-VHD\n";
            return 1;
        }
        // (the track says it, and the metadata of the capture, which settrack makes say the same)
        sets << "system=" + jsonText(videoSystemFileName(asked, variant));
    }

    QString target = source;
    if (parser.isSet("output")) {
        const QString output = parser.value("output");
        if (sameFile(source, output) || isStdio(output)) {
            err << "The output must be a file, and not the one that is read\n";
            return 1;
        }
        if (sets.isEmpty() && unsets.isEmpty()) {
            // (nothing for the track to change: a copy)
            if (!QFile::copy(source, output)) {
                err << "Could not write " << output << " (it exists already?)\n";
                return 1;
            }
        }
        target = output;
    }
    if (!sets.isEmpty() || !unsets.isEmpty()) {
        QStringList settings = { arguments.first(), source, "--id", QString::number(id) };
        for (const QString &set: sets) settings << "--set" << set;
        for (const QString &unset: unsets) settings << "--unset" << unset;
        if (parser.isSet("output")) settings << "-o" << target;
        if (setTrackCommand(settings) != 0) return 1;
    }
    return 0;
}

// ld-container rfsource file.ldx [--id n] [--set file.json | --export | --remove] [-o output.ldx]
//
// Where a capture comes from (the track ldx.rfsource, section 8.4): what is known of the RF file it was decoded from. It is information for people, with no other use.
int rfSourceCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Where a capture comes from: what is known of the RF file it was decoded from (its name, size, sha256, sample rate, bits, date, the files that were beside it, and the signal),\n"
                                     "kept in the track of metadata ldx.rfsource (section 8.4 of the specification) as the entry of the capture in an .rfx archive of RF-tools spells it. It is\n"
                                     "information to trace the capture, and has no other use. With no option the track is shown; --set puts the JSON of a file in, --export writes it out and\n"
                                     "--remove takes it away. It is put in the file itself, at its end (no copy of the file), or in a copy with -o.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the video track of the capture (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("set", "Put the JSON object of this file in", "file"));
    parser.addOption(QCommandLineOption("export", "Write the JSON out to this file (- for the standard output)", "file"));
    parser.addOption(QCommandLineOption("remove", "Take it out"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write a copy with the change instead of changing the file", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || (parser.isSet("set") + parser.isSet("export") + parser.isSet("remove")) > 1) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const int id = videoTrackOf(source, parser.value("id").toInt());
    if (id == 0) {
        err << "There is no video track " << parser.value("id") << " in " << source << "\n";
        return 1;
    }
    if (!parser.isSet("set") && !parser.isSet("remove")) {
        QByteArray json;
        QString error;
        if (!LdxCapture::readRfSource(source, id, json, error)) {
            if (!error.isEmpty()) err << error << "\n";
            else out << "Track " << id << " has no information on where it comes from\n";
            return error.isEmpty() ? 0 : 1;
        }
        if (parser.isSet("export")) {
            const QString target = parser.value("export");
            if (target == "-") {
                out << QJsonDocument::fromJson(json).toJson(QJsonDocument::Indented);
                return 0;
            }
            QFile file(target);
            if (!file.open(QIODevice::WriteOnly)) {
                err << "Could not write " << target << "\n";
                return 1;
            }
            file.write(QJsonDocument::fromJson(json).toJson(QJsonDocument::Indented));
            out << "Written to " << target << "\n";
            return 0;
        }
        // (what is shown: the members, and the size of the files that sat beside it)
        const QJsonObject object = QJsonDocument::fromJson(json).object();
        out << "Track " << id << " comes from:\n";
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == "sidecars") {
                for (const QJsonValue &sidecar: it.value().toArray()) {
                    const QJsonObject entry = sidecar.toObject();
                    out << "  beside it: " << entry.value("name").toString() << " (" << entry.value("data").toArray().size() << " bytes)\n";
                }
            } else if (it.key() == "tail") {
                continue;
            } else {
                out << "  " << it.key() << ": " << (it.value().isString() ? it.value().toString() : jsonText(it.value())) << "\n";
            }
        }
        return 0;
    }

    QByteArray json;
    if (parser.isSet("set")) {
        QFile file(parser.value("set"));
        if (!file.open(QIODevice::ReadOnly)) {
            err << "Could not open " << parser.value("set") << "\n";
            return 1;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (!document.isObject() || document.object().isEmpty()) {
            err << parser.value("set") << " is not a JSON object" << (parseError.error != QJsonParseError::NoError ? ": " + parseError.errorString() : QString()) << "\n";
            return 1;
        }
        json = document.toJson(QJsonDocument::Compact);
    }
    QString target = source;
    if (parser.isSet("output")) {
        const QString output = parser.value("output");
        if (sameFile(source, output) || isStdio(output)) {
            err << "The output must be a file, and not the one that is read\n";
            return 1;
        }
        if (!QFile::copy(source, output)) {
            err << "Could not write " << output << " (it exists already?)\n";
            return 1;
        }
        target = output;
    }
    QString error;
    if (!LdxCapture::putRfSourceInPlace(target, id, json, historyEntry("rfsource", json.isEmpty() ? QString("track %1: where it comes from taken out").arg(id) : QString("track %1: where it comes from put in").arg(id)), error)) {
        err << error << "\n";
        return 1;
    }
    out << (json.isEmpty() ? "Taken out for track " : "Put in for track ") << id << ", in " << target << "\n";
    return 0;
}

// ld-container rename file.ldx --id n --name text [-o output.ldx]
int renameCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Change the name of a track (the capture it names, for a video track). It is in the header of the track, changed in the file itself, in\n"
                                     "the room the list of tracks has (no copy of the file); with -o the output is a copy that has it.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track", "id"));
    parser.addOption(QCommandLineOption("name", "The new name", "name"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write a copy of the file with the new name instead of changing it", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("id") || !parser.isSet("name")) {
        err << parser.helpText();
        return 1;
    }
    QString target = parser.positionalArguments().first();
    if (parser.isSet("output")) {
        if (sameFile(target, parser.value("output")) || isStdio(parser.value("output")) || !QFile::copy(target, parser.value("output"))) {
            err << "Could not write " << parser.value("output") << " (it exists already, or it is the file that is read?)\n";
            return 1;
        }
        target = parser.value("output");
    }
    QString error;
    const int id = parser.value("id").toInt();
    if (!LdxCapture::updateTrackInPlace(target, id, parser.value("name"), true, QJsonObject(), QStringList(),
                                        historyEntry("rename", QString("track %1 is now %2").arg(id).arg(parser.value("name"))), error)) {
        err << error << "\n";
        if (parser.isSet("output")) QFile::remove(target);
        return 1;
    }
    out << "Track " << id << " is now called " << parser.value("name") << ", in " << target << "\n";
    return 0;
}

// ld-container index file.ldx -o output.ldx: a stream that was saved as it came (no cues), written again with its index
int indexOut(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write an LDX file again with its index (the cues), as a file: what a stream that was saved as it came (ld-container stream, or\n"
                                     "a producer writing to a pipe) needs for a program to open it without reading it all first. Nothing else changes.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output") || isStdio(parser.value("output"))) {
        err << parser.helpText();
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }
    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("index", QString("written as a file, with its index")));
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
    out << "Written with its index to " << output << "\n";
    return 0;
}

// ld-container recompress file.ldx --codec flac [--id 1] -o output.ldx
int recompress(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Keep the tracks of samples of an LDX file as flac (the TBC and the audio), ffv1 (CVBS), or as raw again, or\n"
                                     "as the best for each (auto), without changing what they hold. One track with --id, else all of them. The file is written again, to the output.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("codec", "raw, flac, ffv1 or auto", "codec"));
    parser.addOption(QCommandLineOption("id", "Only the track with this id", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("codec") || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }
    const QString asked = parser.value("codec");
    Ldx::Codec ignored;
    if (!codecFor(asked, Ldx::Kind::Tbc, ignored)) {
        err << "The codec is raw, flac, ffv1 or auto\n";
        return 1;
    }
    const QString source = parser.positionalArguments().first();
    const QString output = parser.value("output");
    if (sameFile(source, output)) {
        err << "The output must not be the file that is read\n";
        return 1;
    }

    Rewrite rewrite;
    if (!openForRewrite(rewrite, source)) return 1;
    int changed = 0;
    // The tracks of what a tool sent on its standard output are named after that output ("-", "- audio", "- metadata"): they take the name of the file they are
    // written to
    if (!isStdio(output)) {
        const QString base = QFileInfo(output).completeBaseName();
        for (Ldx::Track &track: rewrite.tracks) {
            if (!base.isEmpty() && (track.name == QLatin1String("-") || track.name.startsWith(QLatin1String("- ")))) track.name = base + track.name.mid(1);
        }
    }
    for (Ldx::Track &track: rewrite.tracks) {
        const bool samples = track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Rf || track.kind == Ldx::Kind::Pcm || track.kind == Ldx::Kind::Cvbs;
        if (!samples || (parser.isSet("id") && track.number != parser.value("id").toUInt())) continue;
        // (a codec that is for other tracks leaves this one as it is, except raw, which is for all)
        Ldx::Codec codec;
        codecFor(asked, track.kind, codec, track.depth);
        if (codec == Ldx::Codec::Raw && asked != "raw" && asked != "auto") continue;
        if (track.codec != codec) changed++;
        track.codec = codec;
    }
    if (parser.isSet("id") && changed == 0) {
        err << "There is no track " << parser.value("id") << " of samples to change, for this codec, in " << source << "\n";
        return 1;
    }

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    rewrite.history.append(historyEntry("recompress", QString("%1 as %2").arg(parser.isSet("id") ? "track " + parser.value("id") : QString("the tracks of samples"), parser.value("codec"))));
    writer.setHistory(rewrite.history);

    QString error;
    if (!openWriter(writer, output, error)) {
        err << error << "\n";
        return 1;
    }
    LdxSource old(rewrite.reader, {});
    if (!mergeInto(writer, { &old }) || !writer.finish(error)) {
        if (!error.isEmpty()) err << error << "\n";
        return 1;
    }
    if (changed == 0) out << "The tracks of samples were " << parser.value("codec") << " already (nothing to change), written to " << output;
    else out << changed << " track(s) now " << parser.value("codec") << ", written to " << output;
    if (!isStdio(output) && !isStdio(source)) out << ": " << QFileInfo(output).size() << " bytes, from " << QFileInfo(source).size();
    out << "\n";
    return 0;
}

// ld-container verify file.ldx
int verify(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Check an LDX file: every block is read in the order of the file and by its place in its\n"
                                     "track (as the tools that jump about in a capture read it), and the two must agree.");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1) {
        err << parser.helpText();
        return 1;
    }
    const QString fileName = parser.positionalArguments().first();
    if (isStdio(fileName)) {
        err << "verify reads the file by place as well as in order, and a stream cannot be read twice: give it a file\n";
        return 1;
    }

    // In the order of the file: what each track holds, as the size and a hash of its blocks
    struct Seen {
        qint64 size;
        quint64 hash;
        qint64 timeNs;
    };
    QMap<quint32, std::vector<Seen>> seen;
    {
        Ldx::Reader reader;
        QString error;
        if (!reader.open(fileName, error)) {
            err << error << "\n";
            return 1;
        }
        if (reader.profile() < 1) {
            err << fileName << " is not an LDX file\n";
            return 1;
        }
        Ldx::Block block;
        while (reader.nextBlock(block)) {
            seen[block.track].push_back({ block.data.size(), qHash(block.data, 0), block.timeNs });
        }
    }

    // By place in the track
    Ldx::Reader reader;
    QString error;
    if (!reader.open(fileName, error) || !reader.buildIndex(error)) {
        err << error << "\n";
        return 1;
    }
    int problems = 0;
    for (const Ldx::Track &track: reader.tracks()) {
        const std::vector<Seen> &blocks = seen[track.number];
        if (reader.blockCount(track.number) != static_cast<qint64>(blocks.size())) {
            err << "  track " << track.number << ": " << blocks.size() << " blocks in the file, " << reader.blockCount(track.number) << " in the index\n";
            // Where they part: the first times that are not the same
            const qint64 indexed = reader.blockCount(track.number);
            for (qint64 i = 0; i < indexed && i < static_cast<qint64>(blocks.size()); i++) {
                if (reader.blockTimeNs(track.number, i) != blocks[static_cast<size_t>(i)].timeNs) {
                    err << "    first difference at block " << i << ": the index says " << reader.blockTimeNs(track.number, i)
                        << " ns, the file " << blocks[static_cast<size_t>(i)].timeNs << " ns\n";
                    break;
                }
            }
            if (indexed > 0 && !blocks.empty()) {
                err << "    last block: the index says " << reader.blockTimeNs(track.number, indexed - 1) << " ns, the file "
                    << blocks.back().timeNs << " ns\n";
            }
            problems++;
            continue;
        }
        int bad = 0;
        for (qint64 i = 0; i < static_cast<qint64>(blocks.size()); i++) {
            Ldx::Block block;
            if (!reader.readBlock(track.number, i, block) || block.data.size() != blocks[i].size || qHash(block.data, 0) != blocks[i].hash) {
                if (bad++ < 5) err << "  track " << track.number << ": block " << i << " does not read back the same\n";
            }
        }
        if (bad > 0) problems += bad;
        out << "  track " << track.number << " (" << Ldx::kindName(track.kind) << "): " << blocks.size() << " blocks" << (bad ? QString(", %1 differ").arg(bad) : QString(", all agree")) << "\n";
    }
    out << (problems ? "The file has problems\n" : "The file is sound\n");
    return problems ? 1 : 0;
}

// ld-container unpack file.ldx -o directory
int unpack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Write the tracks and the attachments of an LDX file out as separate files");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The directory to write to", "directory"));
    parser.process(arguments);
    processStandardDebugOptions(parser);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }

    Ldx::Reader reader;
    QString error;
    if (!openReader(reader, parser.positionalArguments().first(), error)) {
        err << error << "\n";
        return 1;
    }
    const QDir directory(parser.value("output"));
    QDir().mkpath(directory.absolutePath());
    const QString base = QFileInfo(parser.positionalArguments().first()).completeBaseName();

    struct CvbsOut {
        VideoSystem system;
        Cvbs::SampleEncoding encoding;
        QString path;
        qint64 frames;
    };
    std::map<quint32, std::shared_ptr<Cvbs::SampleConverter>> converters;
    std::map<quint32, CvbsOut> cvbsTracks;
    std::map<quint32, std::unique_ptr<QFile>> files;
    // The 24-bit audio of a CVBS capture goes back to the WAV file it came from: <data file name>_audio_0.wav
    struct WavOut {
        std::unique_ptr<CvbsAudio::WavWriter> writer;
        QString path;
        quint32 about;
    };
    std::map<quint32, WavOut> wavTracks;
    for (const Ldx::Track &track: reader.tracks()) {
        if (track.kind == Ldx::Kind::Pcm && track.depth == 24) {
            const quint32 about = static_cast<quint32>(track.parameters.value("track").toInt(0));
            WavOut wavOut;
            wavOut.about = about;
            wavOut.path = directory.filePath(CvbsAudio::wavFileName(QString("%1.%2").arg(base).arg(about), 0));
            wavOut.writer = std::make_unique<CvbsAudio::WavWriter>();
            if (!wavOut.writer->open(wavOut.path, static_cast<quint32>(track.parameters.value("sampleRate").toDouble(CvbsAudio::CVBS_SAMPLE_RATE)))) {
                err << "Could not write " << wavOut.path << "\n";
                return 1;
            }
            out << "  track " << track.number << " -> " << QFileInfo(wavOut.path).fileName() << "\n";
            wavTracks[track.number] = std::move(wavOut);
        }
    }
    for (const Ldx::Track &track: reader.tracks()) {
        if (wavTracks.count(track.number)) continue;
        QString suffix = ".bin";

        if (track.kind == Ldx::Kind::Tbc) suffix = ".tbc";
        else if (track.kind == Ldx::Kind::Cvbs) suffix = ".cvbs";
        else if (track.kind == Ldx::Kind::Pcm) suffix = ".pcm";
        if (track.kind == Ldx::Kind::Tbc && track.parameters.value("role").toString() == "chroma") suffix = ".chroma.tbc";
        // A CVBS Y/C pair is named as decode-orc names it: <name>.cvbsy and <name>.cvbsc, both with the id of the luma
        quint32 fileId = track.number;
        if (track.kind == Ldx::Kind::Cvbs) {
            if (track.parameters.value("role").toString() == "chroma") {
                suffix = ".cvbsc";
                fileId = static_cast<quint32>(track.parameters.value("track").toInt(0));
            } else {
                for (const Ldx::Track &other: reader.tracks()) {
                    if (other.kind == Ldx::Kind::Cvbs && other.parameters.value("role").toString() == "chroma"
                        && other.parameters.value("track").toInt(0) == static_cast<int>(track.number)) suffix = ".cvbsy";
                }
            }
        }
        else if (track.kind == Ldx::Kind::Efm) suffix = ".efm";
        else if (track.kind == Ldx::Kind::Ac3) suffix = ".ac3";
        else if (track.kind == Ldx::Kind::Metadata) suffix = Ldx::isCaptureMetadata(track) ? ".json" : ".rfsource.json";      // (the metadata of the capture, or other metadata of a namespace of its own)
        else if (track.kind == Ldx::Kind::Annotations) suffix = ".annotations.json";
        // The files are told apart by the id of their track. What is about a CVBS capture is named as its companion: <data file>.json
        QString name = QString("%1.%2%3").arg(base).arg(fileId).arg(Ldx::isCaptureMetadata(track) ? ".tbc" + suffix : suffix);
        if (Ldx::isCaptureMetadata(track)) {
            const quint32 about = static_cast<quint32>(track.parameters.value("track").toInt(0));
            for (const Ldx::Track &other: reader.tracks()) {
                if (other.number == about && other.kind == Ldx::Kind::Cvbs) name = QString("%1.%2.cvbs.json").arg(base).arg(about);
            }
        }
        auto file = std::make_unique<QFile>(directory.filePath(name));
        if (!file->open(QIODevice::WriteOnly)) {
            err << "Could not write " << file->fileName() << "\n";
            return 1;
        }
        if (track.kind == Ldx::Kind::Cvbs) {
            // The samples go back to the words the file had, whatever the encoding
            const QString system = track.parameters.value("system").toString();
            const VideoSystem videoSystem = videoSystemOfName(system);
            Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
            Cvbs::parseSampleEncodingName(track.parameters.value("sampleEncoding").toString(), encoding);
            converters[track.number] = std::make_shared<Cvbs::SampleConverter>(encoding, videoSystem);
            cvbsTracks[track.number] = { videoSystem, encoding, directory.filePath(name), 0 };
        }
        out << "  track " << track.number << " -> " << name << "\n";
        files[track.number] = std::move(file);
    }

    // (the current metadata are the last block of their track: what was added to the file is more recent than what was there)
    std::map<quint32, LdxMetadataMerger> current;
    QSet<quint32> keepLast;
    QSet<quint32> otherMetadata;
    QMap<quint32, QByteArray> otherLast;
    for (const Ldx::Track &track: reader.tracks()) {
        if (Ldx::isCaptureMetadata(track)) keepLast.insert(track.number);
        else if (track.kind == Ldx::Kind::Metadata) otherMetadata.insert(track.number);      // (what a tool wrote of its own: the last block is the current one)
    }
    Ldx::Block block;
    while (reader.nextBlock(block)) {
        const auto wavFound = wavTracks.find(block.track);
        if (wavFound != wavTracks.end()) {
            wavFound->second.writer->write(block.data);
            continue;
        }
        const auto found = files.find(block.track);
        if (found == files.end()) continue;
        if (otherMetadata.contains(block.track)) {
            otherLast[block.track] = block.data;
        } else if (keepLast.contains(block.track)) {
            current[block.track].add(block.data);
        } else if (converters.count(block.track)) {
            // The 10-bit values of a frame, as the words of the file
            const auto converter = converters[block.track];
            QByteArray words(block.data.size(), 0);
            const quint16 *values = reinterpret_cast<const quint16 *>(block.data.constData());
            qint16 *stored = reinterpret_cast<qint16 *>(words.data());
            for (qint32 i = 0; i < block.data.size() / 2; i++) stored[i] = converter->fromWord(values[i]);
            found->second->write(words);
            cvbsTracks[block.track].frames++;
        } else {
            found->second->write(block.data);
        }
    }
    for (const auto &kept: current) files[kept.first]->write(kept.second.current());
    for (auto it = otherLast.constBegin(); it != otherLast.constEnd(); ++it) files[it.key()]->write(it.value());

    for (auto &wavOut: wavTracks) {
        if (!wavOut.second.writer->close()) {
            err << "Writing " << wavOut.second.path << " failed\n";
            return 1;
        }
    }
    // The core metadata of a CVBS file, which says how its samples are kept
    for (const auto &cvbs: cvbsTracks) {
        files[cvbs.first]->close();
        Cvbs::FileMetadata fileMetadata;
        fileMetadata.system = cvbs.second.system;
        fileMetadata.sampleEncoding = cvbs.second.encoding;
        fileMetadata.numberOfFrames = static_cast<qint32>(cvbs.second.frames);
        if (fileMetadata.write(Cvbs::metaFileName(cvbs.second.path))) out << "  core metadata of track " << cvbs.first << " -> " << QFileInfo(Cvbs::metaFileName(cvbs.second.path)).fileName() << "\n";
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
    if (arguments.size() == 2 && (arguments[1] == "-v" || arguments[1] == "--version")) {
        out << "ld-container " << QCoreApplication::applicationVersion() << "\n";
        return 0;
    }
    if (arguments.size() < 2 || arguments[1] == "-h" || arguments[1] == "--help") {
        out << "ld-container - pack, unpack and inspect LDX files\n\n"
               "Usage: ld-container <command> [options]\n\n"
               "Commands:\n"
               "  history  show what was done to a file: the tools, when and with what arguments\n"
               "  addtrack put a file (decoded EFM audio, AC3 data...) in an .ldx file as a track\n"
               "  wav      write the audio of an .ldx file as a WAV file\n"
               "  pack     pack a .tbc, its metadata and its audio into an .ldx file\n"
               "  add      add a capture to an .ldx file, as a track of its own\n"
               "  remove   remove the track with an id (and what is about it) from an .ldx file\n"
               "  addchroma add the chroma file of a Y/C capture to the capture that is in an .ldx file as its luma\n"
               "  stack    keep a stacking session in an .ldx file, its sources being tracks\n"
               "  unpack   write the tracks and attachments of an .ldx file out as separate files\n"
               "  info     show what an .ldx file holds\n"
               "  recompress  keep the tracks of samples as flac, ffv1 or raw\n"
               "  reorder  write an .ldx file with its captures in another order (the tracks are numbered again)\n"
               "  merge    put the captures of another .ldx file in this one, with their tracks\n"
               "  cover    put a picture in an .ldx file as its cover, take it out, or write it out\n"
               "  stream   write an .ldx file as a stream (no index), to a file or to the standard output\n"
               "  index    write a stream that was saved as it came again with its index\n"
               "  display  set the shape (4:3, 16:9 or none) a capture is shown in by video players, and isWidescreen with it\n"
               "  rename   change the name of a track, in the file itself\n"
               "  settrack change the name and the parameters of a track (the sample rate of the audio, the system of a video...)\n"
               "  tag      say what a capture is: medium, reference, copy, capture, face, colour standard\n"
               "  rfsource show, put in or take out where a capture comes from (the RF file it was decoded from)\n"
               "  metadata write the metadata of a capture out as JSON, or replace them with those of a .json or .db file\n"
               "  unstack  write an .ldx file again without its session (session.stack)\n"
               "  verify   check that every block of an .ldx file reads back the same, in order and by place\n"
               "  extract  write the data of one track as it is, to a file or to the standard output (to be piped)\n\n"
               "Use ld-container <command> --help for the options of a command.\n"
               "Every command also takes -v (version), -d (debug messages) and -q (quiet).\n";
        return arguments.size() < 2 ? 1 : 0;
    }

    const QString command = arguments[1];
    // The command is not an option of the parser of the command
    arguments.removeAt(1);
    if (command == "pack") return pack(arguments);
    if (command == "add") return addStream(arguments);
    if (command == "remove") return removeStream(arguments);
    if (command == "addchroma") return addChroma(arguments);
    if (command == "stack") return setStack(arguments);
    if (command == "unpack") return unpack(arguments);
    if (command == "info") return info(arguments);
    if (command == "history") return history(arguments);
    if (command == "addtrack") return addTrack(arguments);
    if (command == "wav") return wav(arguments);
    if (command == "verify") return verify(arguments);
    if (command == "recompress") return recompress(arguments);
    if (command == "stream") return streamOut(arguments);
    if (command == "index") return indexOut(arguments);
    if (command == "display") return displayCommand(arguments);
    if (command == "rename") return renameCommand(arguments);
    if (command == "settrack") return setTrackCommand(arguments);
    if (command == "tag") return tagCommand(arguments);
    if (command == "rfsource") return rfSourceCommand(arguments);
    if (command == "metadata") return metadataCommand(arguments);
    if (command == "unstack") return unstack(arguments);
    if (command == "reorder") return reorder(arguments);
    if (command == "merge") return mergeCommand(arguments);
    if (command == "cover") return coverCommand(arguments);
    if (command == "extract") return extractTrack(arguments);
    err << "Unknown command " << command << "\n";
    return 1;
}
