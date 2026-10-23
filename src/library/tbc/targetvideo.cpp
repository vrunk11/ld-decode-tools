/******************************************************************************
 * targetvideo.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "targetvideo.h"
#include "stackproject.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include "ldxcapture.h"
#include "ldxstream.h"
#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

TargetVideo::~TargetVideo()
{
    close();
}

#ifdef LDDECODE_WITH_LDX

struct TargetVideo::LdxOutput {
    Ldx::Writer writer;
    quint32 video = 0, audio = 0, metadata = 0;
    qint64 fields = 0;
    bool cvbs = false;
};

namespace {
qint64 fieldTimeNs(qint64 field, VideoSystem system)
{
    if (system == PAL) return field * 1000000000LL / 50;
    return field * 1000000000LL * 1001 / 60000;
}
}

void TargetVideo::setLdxStream(const QByteArray &parametersJson)
{
    ldxStreamOutput = true;
    ldxParameters = parametersJson;
}

void TargetVideo::setLdxVideo(qint32 fieldWidth, qint32 fieldHeight, double sampleRate, const QString &name, const QJsonArray &history)
{
    ldxFieldWidth = fieldWidth;
    ldxFieldHeight = fieldHeight;
    ldxSampleRate = sampleRate;
    ldxName = name;
    ldxHistory = history;
}

void TargetVideo::setLdxAudio(double sampleRate, qint32 bits) { ldxAudioRate = sampleRate; ldxAudioBits = bits; }

qint32 TargetVideo::addLdxTrack(const QString &kind, const QString &codec, qint32 depth, const QString &name, const QJsonObject &parameters)
{
    ExtraTrack track;
    track.kind = kind;
    track.codec = codec;
    track.depth = depth;
    track.name = name;
    track.parameters = parameters;
    ldxExtraTracks.append(track);
    return static_cast<qint32>(ldxExtraTracks.size()) - 1;
}

bool TargetVideo::writeLdxTrack(qint32 index, qint64 timeNs, const QByteArray &data)
{
    if (!ldx || index < 0 || index >= ldxExtraTracks.size() || ldxExtraTracks[index].number == 0) return false;
    return ldx->writer.writeBlock(ldxExtraTracks[index].number, timeNs, data);
}

qint64 TargetVideo::ldxTimeNs() const
{
    return ldx ? fieldTimeNs(ldx->fields, ldxSystem) : 0;
}
bool TargetVideo::isLdx() const { return ldx != nullptr; }

bool TargetVideo::writeAudio(const QByteArray &samples)
{
    if (!ldx || ldx->audio == 0) return false;
    // (stamped with the time of the next field: the audio of the fields written so far)
    return ldx->writer.writeBlock(ldx->audio, fieldTimeNs(ldx->fields, ldxSystem), samples);
}

#else

void TargetVideo::setLdxVideo(qint32, qint32, double, const QString &, const QJsonArray &) {}
void TargetVideo::setLdxStream(const QByteArray &) {}
void TargetVideo::setLdxAudio(double, qint32) {}
bool TargetVideo::isLdx() const { return false; }
bool TargetVideo::writeAudio(const QByteArray &) { return false; }
qint32 TargetVideo::addLdxTrack(const QString &, const QString &, qint32, const QString &, const QJsonObject &) { return -1; }
bool TargetVideo::writeLdxTrack(qint32, qint64, const QByteArray &) { return false; }
qint64 TargetVideo::ldxTimeNs() const { return 0; }

#endif

bool TargetVideo::sourceIsCvbs(const QString &inputName)
{
    if (inputName == "-") return LdxStream::instance() && LdxStream::instance()->isCvbs();
    const QString reference = StackProject::referenceFile(inputName);
    if (LdxCapture::isLdxName(reference)) return LdxCapture::isCvbs(reference);
    return Cvbs::isDataFileName(reference);
}

bool TargetVideo::open(const QString &fileName, VideoSystem system, Cvbs::SampleEncoding encoding)
{
    cvbs = fileName != "-" && Cvbs::isDataFileName(fileName);
    hasPendingField = false;
    ldxSystem = system;

    if ((fileName == "-" && ldxStreamOutput) || (fileName != "-" && LdxCapture::isLdxName(fileName))) {
#ifdef LDDECODE_WITH_LDX
        if (fileName != "-" && fileName.contains('#')) {
            qCritical().noquote() << "The output" << fileName << "is a track of an LDX file: name the file";
            return false;
        }
        ldx = std::make_shared<LdxOutput>();
        Ldx::Track video;
        video.kind = ldxCvbs ? Ldx::Kind::Cvbs : Ldx::Kind::Tbc;
        video.depth = ldxCvbs ? 10 : 16;
        ldx->cvbs = ldxCvbs;
        if (ldxCvbs) {
            // What a frame is (the length of a line, and its samples) is what the codec needs and what gives the file back
            geometry = Cvbs::geometry(system);
            converter = std::make_unique<Cvbs::SampleConverter>(encoding, system);
            frameData.resize(static_cast<qint32>(geometry.frameSamples));
            video.parameters["lineWidth"] = geometry.lineWidth;
            video.parameters["frameSamples"] = static_cast<double>(geometry.frameSamples);
            video.parameters["sampleEncoding"] = Cvbs::sampleEncodingName(encoding);
            video.parameters["padValue"] = Cvbs::standardLevels(system).blanking;
        }
        video.name = ldxName.isEmpty() ? QFileInfo(fileName).completeBaseName() : ldxName;
        video.parameters["system"] = system == PAL ? "PAL" : (system == NTSC ? "NTSC" : "PAL-M");
        video.parameters["sampleRate"] = ldxSampleRate;
        video.parameters["fieldWidth"] = ldxFieldWidth;
        video.parameters["fieldHeight"] = ldxFieldHeight;
        ldx->writer.addTrack(video);
        ldx->video = video.number;
        Ldx::Track metadata;
        metadata.kind = Ldx::Kind::Metadata;
        metadata.depth = 0;
        metadata.name = video.name + " metadata";
        metadata.parameters["namespace"] = "ldx.tbcmetadata";
        metadata.parameters["format"] = "ld-decode-json";
        metadata.parameters["track"] = static_cast<int>(video.number);
        ldx->writer.addTrack(metadata);
        ldx->metadata = metadata.number;
        if (ldxAudioRate > 0) {
            Ldx::Track audio;
            audio.kind = Ldx::Kind::Pcm;
            audio.depth = ldxAudioBits;
            audio.name = video.name + " audio";
            audio.parameters["sampleRate"] = ldxAudioRate;
            audio.parameters["channels"] = 2;
            if (ldxAudioBits != 16) audio.parameters["bits"] = ldxAudioBits;
            audio.parameters["track"] = static_cast<int>(video.number);
            ldx->writer.addTrack(audio);
            ldx->audio = audio.number;
        }
        for (ExtraTrack &extra : ldxExtraTracks) {
            Ldx::Track track;
            track.kind = Ldx::kindFromName(extra.kind);
            track.codec = Ldx::codecFromName(extra.codec);
            track.depth = extra.depth;
            track.name = extra.name;
            track.parameters = extra.parameters;
            track.parameters["track"] = static_cast<int>(video.number);
            ldx->writer.addTrack(track);
            extra.number = track.number;
        }
        if (!ldxHistory.isEmpty()) ldx->writer.setHistory(ldxHistory);
        QString error;
        if (fileName == "-") {
#ifdef Q_OS_WIN
            _setmode(_fileno(stdout), _O_BINARY);
#endif
        }
        const bool opened = fileName == "-" ? ldx->writer.openStream(stdout, error) : ldx->writer.open(fileName, error);
        if (opened && ldxStreamOutput && !ldxParameters.isEmpty() && !ldx->writer.writeBlock(ldx->metadata, 0, ldxParameters)) {
            error = ldx->writer.lastError();
            qCritical().noquote() << "Could not write the parameters of the video to" << fileName << "-" << error;
            ldx.reset();
            return false;
        }
        if (!opened) {
            qCritical().noquote() << "Unable to open output LDX file" << fileName << "-" << error;
            ldx.reset();
            return false;
        }
        return true;
#else
        qCritical() << "This program was built without support for LDX files";
        return false;
#endif
    }

    if (cvbs) {
        geometry = Cvbs::geometry(system);
        converter = std::make_unique<Cvbs::SampleConverter>(encoding, system);
        frameData.resize(static_cast<qint32>(geometry.frameSamples));
    }

    if (fileName == "-") {
        if (!file.open(stdout, QIODevice::WriteOnly)) {
            qCritical() << "Unable to open stdout for the output video";
            return false;
        }
    } else {
        file.setFileName(fileName);
        if (!file.open(QIODevice::WriteOnly)) {
            qCritical().noquote() << "Unable to open output video file" << fileName;
            return false;
        }
    }

    return true;
}

bool TargetVideo::writeField(const QVector<quint16> &fieldData, const QByteArray &metadataJson)
{
#ifdef LDDECODE_WITH_LDX
    if (ldx && ldx->metadata != 0 && !metadataJson.isEmpty()) {
        // (the metadata of a field go just before its video)
        // (the fields of a CVBS frame have the time of the frame)
        const qint64 at = ldx->cvbs ? ldx->fields - (ldx->fields % 2) : ldx->fields;
        if (!ldx->writer.writeBlock(ldx->metadata, fieldTimeNs(at, ldxSystem), metadataJson)) {
            qCritical().noquote() << "Could not write to the output LDX file:" << ldx->writer.lastError();
            return false;
        }
    }
#else
    Q_UNUSED(metadataJson);
#endif
    return writeField(fieldData);
}

bool TargetVideo::writeField(const QVector<quint16> &fieldData)
{
#ifdef LDDECODE_WITH_LDX
    if (ldx && ldx->cvbs) {
        if (fieldData.size() != geometry.fieldLength()) {
            qCritical() << "Field of" << fieldData.size() << "samples does not fit a CVBS frame of" << geometry.fieldLength() << "sample fields";
            return false;
        }
        ldx->fields++;
        // Hold the first field of each frame until the second arrives
        if (!hasPendingField) {
            pendingField = fieldData;
            hasPendingField = true;
            return true;
        }
        Cvbs::fieldsToFrame(pendingField.constData(), fieldData.constData(), geometry, *converter, frameData.data());
        hasPendingField = false;
        // The samples of a cvbs track are 10-bit values in words
        QByteArray words(frameData.size() * 2, 0);
        quint16 *values = reinterpret_cast<quint16 *>(words.data());
        for (qint32 i = 0; i < frameData.size(); i++) values[i] = converter->toWord(frameData[i]);
        if (!ldx->writer.writeBlock(ldx->video, fieldTimeNs(ldx->fields - 2, ldxSystem), words)) {
            qCritical().noquote() << "Could not write to the output LDX file:" << ldx->writer.lastError();
            return false;
        }
        return true;
    }
    if (ldx) {
        const QByteArray bytes(reinterpret_cast<const char *>(fieldData.constData()), fieldData.size() * 2);
        if (!ldx->writer.writeBlock(ldx->video, fieldTimeNs(ldx->fields, ldxSystem), bytes)) {
            qCritical().noquote() << "Could not write to the output LDX file:" << ldx->writer.lastError();
            return false;
        }
        ldx->fields++;
        return true;
    }
#endif
    if (!cvbs) {
        const qint64 bytes = 2 * static_cast<qint64>(fieldData.size());
        return file.write(reinterpret_cast<const char *>(fieldData.constData()), bytes) == bytes;
    }

    if (fieldData.size() != geometry.fieldLength()) {
        qCritical() << "Field of" << fieldData.size() << "samples does not fit a CVBS frame of"
                    << geometry.fieldLength() << "sample fields";
        return false;
    }

    // Hold the first field of each frame until the second arrives
    if (!hasPendingField) {
        pendingField = fieldData;
        hasPendingField = true;
        return true;
    }

    Cvbs::fieldsToFrame(pendingField.constData(), fieldData.constData(), geometry, *converter, frameData.data());
    hasPendingField = false;

    const qint64 bytes = 2 * static_cast<qint64>(frameData.size());
    return file.write(reinterpret_cast<const char *>(frameData.constData()), bytes) == bytes;
}

void TargetVideo::setLevelMap(double scale, double offset)
{
    if (converter) converter->setLevelMap(scale, offset);
}

bool TargetVideo::close()
{
#ifdef LDDECODE_WITH_LDX
    if (ldx) {
        QString error;
        if (hasPendingField) {
            qWarning() << "The output ended with an unpaired field, which is not written: CVBS holds whole frames";
            hasPendingField = false;
        }
        const bool ok = ldx->writer.finish(error);
        if (!ok) qCritical().noquote() << "Could not finish the output LDX file:" << error;
        ldx.reset();
        return ok;
    }
#endif
    if (file.isOpen()) file.close();

    if (hasPendingField) {
        qWarning() << "The output ended with an unpaired field, which is not written: CVBS files hold whole frames";
        hasPendingField = false;
        return false;
    }
    return true;
}

bool checkSameVideoContainer(const QString &inputFileName, const QString &outputFileName)
{
    // stdout carries whatever the input is, and an LDX file holds TBC or CVBS
    if (outputFileName == "-" || LdxCapture::isLdxName(outputFileName)) return true;

    const bool inputIsCvbs = Cvbs::isDataFileName(StackProject::referenceFile(inputFileName));
    const bool outputIsCvbs = Cvbs::isDataFileName(outputFileName);
    if (inputIsCvbs == outputIsCvbs) return true;

    if (inputIsCvbs) {
        qCritical().noquote() << "The input is CVBS but" << outputFileName
                              << "is not a .cvbs file - use ld-cvbs-to-tbc to convert to TBC";
    } else {
        qCritical().noquote() << "The input is TBC but" << outputFileName
                              << "is a CVBS file - use ld-tbc-to-cvbs to convert to CVBS";
    }
    return false;
}
