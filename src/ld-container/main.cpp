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
#include "ldxcapture.h"
#include "cvbs.h"
#include "cvbsaudio.h"
#include "tbcfiles.h"
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
    parser.addOption(QCommandLineOption("codec", "How the video and the audio are kept: raw (default), flac (TBC and audio), ffv1 (CVBS), or auto (the best for each)", "codec", "raw"));
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
};

CaptureOptions captureOptions(const QCommandLineParser &parser)
{
    CaptureOptions options;
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
            for (qint64 i = 0; i < samples; i++) target[i] = static_cast<quint16>(converter->toTbc(source[i]) >> 6);
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
    capture.video.parameters["system"] = systemName(video.system);
    capture.video.parameters["sampleRate"] = video.sampleRate;
    capture.video.parameters["fieldWidth"] = video.fieldWidth;
    capture.video.parameters["fieldHeight"] = video.fieldHeight;
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
    parser.addPositionalArgument("input", "The .tbc file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("efm", "The EFM t-values of the capture (.efm) to add", "file"));
    parser.addOption(QCommandLineOption("ac3", "The AC3 data of the capture to add", "file"));
    parser.addOption(QCommandLineOption("live-metadata", "Send the metadata field by field, each just before its video, as a producer does that does not have them all at the start"));
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
    CaptureOptions captured = captureOptions(parser);
    captured.live = parser.isSet("live-metadata");
    if (!readCapture(captured, input, capture)) return 1;
    ChromaPart chroma;
    if (!readChroma(captured, input, chroma)) return 1;

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
    const CaptureOptions addOptions = captureOptions(parser);
    if (!readCapture(addOptions, input, capture)) return 1;
    ChromaPart chroma;
    if (!readChroma(addOptions, input, chroma)) return 1;

    Ldx::Writer writer;
    prepareWriter(rewrite, writer, {});
    addCaptureTracks(writer, capture);
    addChromaTrack(writer, chroma, capture.video.number);
    rewrite.history.append(historyEntry("add", QString("track %1: %2").arg(capture.video.number).arg(QFileInfo(input).fileName())));
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.addPositionalArgument("session", "The .stack file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("embed", "Bring the sources that are .tbc files into the LDX file, as tracks (the others stay links)"));
    parser.addOption(QCommandLineOption("codec", "With --embed, how the video and the audio brought in are kept: raw (default), flac, ffv1 or auto", "codec", "raw"));
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.process(arguments);
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("kind", "pcm, efm, ac3 or annotations", "kind"));
    parser.addOption(QCommandLineOption("input", "The file to put in", "file"));
    parser.addOption(QCommandLineOption("about", "The id of the video track it goes with (default: the first)", "id", "0"));
    parser.addOption(QCommandLineOption("name", "The name of the track (default: the name of the file)", "name"));
    parser.addOption(QCommandLineOption("rate", "Sample rate of pcm audio (default 44100)", "hertz", "44100"));
    parser.addOption(QCommandLineOption("source", "For pcm: where the audio comes from, efm for the digital audio decoded from EFM (default: the analogue audio)", "source"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "Write to another file instead of in place", "file"));
    parser.process(arguments);
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
                                      historyEntry("addtrack", QString("%1 as a %2 track").arg(QFileInfo(input).fileName(), parser.value("kind"))), error)) {
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The WAV file (default: the name of the LDX file, .wav)", "file"));
    parser.addOption(QCommandLineOption("id", "The id of the pcm track (default: the first that fits)", "id", "0"));
    parser.addOption(QCommandLineOption("source", "analogue (default) or efm", "source", "analogue"));
    parser.process(arguments);
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
    parser.addPositionalArgument("file", "The LDX file (- for a stream)");
    parser.addOption(QCommandLineOption("json", "Write the history as a JSON array, for another program"));
    parser.process(arguments);
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("id", "The id of the track", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The file to write (the standard output if omitted)", "file"));
    parser.process(arguments);
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

// ld-container recompress file.ldx --codec flac [--id 1] -o output.ldx
int recompress(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Keep the tracks of samples of an LDX file as flac (the TBC and the audio), ffv1 (CVBS), or as raw again, or\n"
                                     "as the best for each (auto), without changing what they hold. One track with --id, else all of them. The file is written again, to the output.");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption("codec", "raw, flac, ffv1 or auto", "codec"));
    parser.addOption(QCommandLineOption("id", "Only the track with this id", "id"));
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.process(arguments);
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
    out << changed << " track(s) now " << parser.value("codec") << ", written to " << output;
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.process(arguments);
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
    parser.addPositionalArgument("file", "The LDX file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The directory to write to", "directory"));
    parser.process(arguments);
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
        else if (track.kind == Ldx::Kind::Metadata) suffix = ".json";
        else if (track.kind == Ldx::Kind::Annotations) suffix = ".annotations.json";
        // The files are told apart by the id of their track. What is about a CVBS capture is named as its companion: <data file>.json
        QString name = QString("%1.%2%3").arg(base).arg(fileId).arg(track.kind == Ldx::Kind::Metadata ? ".tbc" + suffix : suffix);
        if (track.kind == Ldx::Kind::Metadata) {
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
            const VideoSystem videoSystem = system == "PAL" ? PAL : (system == "NTSC" ? NTSC : PAL_M);
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
    for (const Ldx::Track &track: reader.tracks()) {
        if (track.kind == Ldx::Kind::Metadata) keepLast.insert(track.number);
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
        if (keepLast.contains(block.track)) {
            current[block.track].add(block.data);
        } else if (converters.count(block.track)) {
            // The 10-bit values of a frame, as the words of the file
            const auto converter = converters[block.track];
            QByteArray words(block.data.size(), 0);
            const quint16 *values = reinterpret_cast<const quint16 *>(block.data.constData());
            qint16 *stored = reinterpret_cast<qint16 *>(words.data());
            for (qint32 i = 0; i < block.data.size() / 2; i++) stored[i] = converter->fromTbc(static_cast<quint16>(values[i] << 6));
            found->second->write(words);
            cvbsTracks[block.track].frames++;
        } else {
            found->second->write(block.data);
        }
    }
    for (const auto &kept: current) files[kept.first]->write(kept.second.current());

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
               "  stack    keep a stacking session in an .ldx file, its sources being tracks\n"
               "  unpack   write the tracks and attachments of an .ldx file out as separate files\n"
               "  info     show what an .ldx file holds\n"
               "  recompress  keep the tracks of samples as flac, ffv1 or raw\n"
               "  verify   check that every block of an .ldx file reads back the same, in order and by place\n"
               "  extract  write the data of one track as it is, to a file or to the standard output (to be piped)\n\n"
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
    if (command == "history") return history(arguments);
    if (command == "addtrack") return addTrack(arguments);
    if (command == "wav") return wav(arguments);
    if (command == "verify") return verify(arguments);
    if (command == "recompress") return recompress(arguments);
    if (command == "extract") return extractTrack(arguments);
    err << "Unknown command " << command << "\n";
    return 1;
}
