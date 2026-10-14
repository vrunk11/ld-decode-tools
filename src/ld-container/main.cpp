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
#include <QTemporaryDir>
#include <QTextStream>

#include "ldx.h"
#include "lddecodemetadata.h"
#include "tbc/logging.h"

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

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

// ld-container pack input.tbc [--pcm file] -o output.ldx
int pack(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Pack a TBC (or CVBS) capture, its metadata and its audio into an LDX file");
    parser.addHelpOption();
    parser.addPositionalArgument("input", "The .tbc or .cvbs file");
    parser.addOption(QCommandLineOption({ "o", "output" }, "The LDX file to write", "file"));
    parser.addOption(QCommandLineOption("pcm", "Analogue audio (16-bit stereo PCM) to add", "file"));
    parser.addOption(QCommandLineOption("pcm-rate", "Sample rate of the audio (default 44100)", "hertz", "44100"));
    parser.addOption(QCommandLineOption("start", "First field to pack (default 0)", "field", "0"));
    parser.addOption(QCommandLineOption("fields", "Number of fields to pack (default all)", "count", "0"));
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1 || !parser.isSet("output")) {
        err << parser.helpText();
        return 1;
    }

    const QString input = parser.positionalArguments().first();
    const QString output = parser.value("output");

    // The metadata of the capture say what the fields are
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(input);
    LdDecodeMetaData metadata;
    if (metadataFile.isEmpty() || !metadata.read(metadataFile)) {
        err << "Could not read the metadata of " << input << "\n";
        return 1;
    }
    const LdDecodeMetaData::VideoParameters &video = metadata.getVideoParameters();
    const qint64 fieldBytes = static_cast<qint64>(video.fieldWidth) * video.fieldHeight * 2;

    QTemporaryDir temporary;
    const QString jsonPath = temporary.filePath("metadata.json");
    if (!metadata.write(jsonPath)) {
        err << "Could not write the metadata out as JSON\n";
        return 1;
    }
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        err << "Could not read back the metadata\n";
        return 1;
    }
    const QByteArray metadataJson = jsonFile.readAll();

    QFile tbc(input);
    if (!tbc.open(QIODevice::ReadOnly)) {
        err << "Could not open " << input << "\n";
        return 1;
    }
    const qint64 start = parser.value("start").toLongLong();
    qint64 fields = tbc.size() / fieldBytes - start;
    if (parser.value("fields").toLongLong() > 0) fields = qMin(fields, parser.value("fields").toLongLong());
    if (fields < 1) {
        err << "There is nothing to pack\n";
        return 1;
    }

    // The tracks
    Ldx::Writer writer;
    Ldx::Track videoTrack;
    videoTrack.kind = Ldx::Kind::Tbc;
    videoTrack.depth = 16;
    videoTrack.name = QFileInfo(input).fileName();
    videoTrack.parameters["system"] = systemName(video.system);
    videoTrack.parameters["sampleRate"] = video.sampleRate;
    videoTrack.parameters["fieldWidth"] = video.fieldWidth;
    videoTrack.parameters["fieldHeight"] = video.fieldHeight;
    writer.addTrack(videoTrack);

    Ldx::Track metadataTrack;
    metadataTrack.kind = Ldx::Kind::Metadata;
    metadataTrack.depth = 0;
    metadataTrack.name = "ld-decode metadata";
    metadataTrack.parameters["namespace"] = "ldx.tbcmetadata";
    metadataTrack.parameters["format"] = "ld-decode-json";
    writer.addTrack(metadataTrack);

    Ldx::Track audioTrack;
    QFile audio(parser.value("pcm"));
    const bool hasAudio = parser.isSet("pcm");
    const double pcmRate = parser.value("pcm-rate").toDouble();
    constexpr int PcmFrameBytes = 4;                    // 16-bit stereo
    if (hasAudio) {
        if (!audio.open(QIODevice::ReadOnly)) {
            err << "Could not open " << parser.value("pcm") << "\n";
            return 1;
        }
        audioTrack.kind = Ldx::Kind::Pcm;
        audioTrack.depth = 16;
        audioTrack.name = QFileInfo(audio.fileName()).fileName();
        audioTrack.parameters["sampleRate"] = pcmRate;
        audioTrack.parameters["channels"] = 2;
        writer.addTrack(audioTrack);
    }

    QJsonObject entry;
    entry["tool"] = "ld-container";
    entry["version"] = APP_COMMIT;
    entry["command"] = "pack";
    entry["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry["input"] = QFileInfo(input).fileName();
    writer.setHistory(QJsonArray { entry });

    QString error;
    if (!writer.open(output, error)) {
        err << error << "\n";
        return 1;
    }
    if (!writer.writeBlock(metadataTrack.number, 0, metadataJson)) {
        err << "Could not write the metadata: " << writer.lastError() << "\n";
        return 1;
    }

    // The fields and the audio, in the order of their time
    tbc.seek(start * fieldBytes);
    const qint64 firstTime = fieldTimeNs(start, video.system);
    qint64 field = 0;
    qint64 audioSample = 0;
    constexpr int AudioChunkSamples = 1470;
    bool audioDone = !hasAudio;
    while (field < fields || !audioDone) {
        const qint64 videoTime = field < fields ? fieldTimeNs(start + field, video.system) - firstTime : Q_INT64_C(0x7fffffffffffffff);
        const qint64 audioTime = audioDone ? Q_INT64_C(0x7fffffffffffffff) : static_cast<qint64>(audioSample * 1.0e9 / pcmRate);
        if (field < fields && videoTime <= audioTime) {
            const QByteArray data = tbc.read(fieldBytes);
            if (data.size() != fieldBytes) { fields = field; continue; }
            if (!writer.writeBlock(videoTrack.number, videoTime, data)) {
                err << "Could not write a field\n";
                return 1;
            }
            field++;
        } else {
            const QByteArray data = audio.read(AudioChunkSamples * PcmFrameBytes);
            if (data.isEmpty()) { audioDone = true; continue; }
            if (!writer.writeBlock(audioTrack.number, audioTime, data)) {
                err << "Could not write audio\n";
                return 1;
            }
            audioSample += data.size() / PcmFrameBytes;
            // The audio stops where the video does
            if (field >= fields) audioDone = true;
        }
    }
    if (!writer.finish(error)) {
        err << error << "\n";
        return 1;
    }
    out << "Packed " << field << " fields";
    if (hasAudio) out << " and " << audioSample << " audio samples";
    out << " into " << output << "\n";
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

    out << "Tracks:\n";
    for (const Ldx::Track &track: reader.tracks()) {
        out << trackLine(track) << "\n";
        out << "       " << blocks.value(track.number) << " blocks, " << bytes.value(track.number) << " bytes";
        if (blocks.value(track.number) > 1) {
            out << ", from " << firstTime.value(track.number) / 1000000.0 << " ms to " << lastTime.value(track.number) / 1000000.0 << " ms";
        }
        out << "\n       " << QJsonDocument(track.parameters).toJson(QJsonDocument::Compact) << "\n";
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
    parser.setApplicationDescription("Write the tracks of an LDX file out as separate files");
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

    QMap<quint32, QFile *> files;
    QMap<quint32, Ldx::Track> byNumber;
    for (const Ldx::Track &track: reader.tracks()) {
        QString suffix = ".bin";
        if (track.kind == Ldx::Kind::Tbc) suffix = ".tbc";
        else if (track.kind == Ldx::Kind::Cvbs) suffix = ".cvbs";
        else if (track.kind == Ldx::Kind::Pcm) suffix = ".pcm";
        else if (track.kind == Ldx::Kind::Efm) suffix = ".efm";
        else if (track.kind == Ldx::Kind::Ac3) suffix = ".ac3";
        else if (track.kind == Ldx::Kind::Metadata) suffix = ".json";
        else if (track.kind == Ldx::Kind::Annotations) suffix = ".annotations.json";
        // Several tracks of a kind are told apart by their id
        QString name = base + (track.kind == Ldx::Kind::Metadata ? ".tbc" : "") + suffix;
        if (QFile::exists(directory.filePath(name))) name = QString("%1.%2%3").arg(base).arg(track.number).arg(suffix);
        auto *file = new QFile(directory.filePath(name));
        if (!file->open(QIODevice::WriteOnly)) {
            err << "Could not write " << file->fileName() << "\n";
            return 1;
        }
        files[track.number] = file;
        byNumber[track.number] = track;
        out << "  track " << track.number << " -> " << name << "\n";
    }

    Ldx::Block block;
    while (reader.nextBlock(block)) {
        if (files.contains(block.track)) files[block.track]->write(block.data);
    }
    qDeleteAll(files);
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
               "  unpack   write the tracks of an .ldx file out as separate files\n"
               "  info     show what an .ldx file holds\n\n"
               "Use ld-container <command> --help for the options of a command.\n";
        return arguments.size() < 2 ? 1 : 0;
    }

    const QString command = arguments[1];
    // The command is not an option of the parser of the command
    arguments.removeAt(1);
    if (command == "pack") return pack(arguments);
    if (command == "unpack") return unpack(arguments);
    if (command == "info") return info(arguments);
    err << "Unknown command " << command << "\n";
    return 1;
}
