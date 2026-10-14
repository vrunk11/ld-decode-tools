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

#include "ldxcapture.h"
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
    quint32 video = 0, audio = 0;
    qint64 fields = 0;
};

namespace {
qint64 fieldTimeNs(qint64 field, VideoSystem system)
{
    if (system == PAL) return field * 1000000000LL / 50;
    return field * 1000000000LL * 1001 / 60000;
}
}

void TargetVideo::setLdxVideo(qint32 fieldWidth, qint32 fieldHeight, double sampleRate, const QString &name, const QJsonObject &historyEntry)
{
    ldxFieldWidth = fieldWidth;
    ldxFieldHeight = fieldHeight;
    ldxSampleRate = sampleRate;
    ldxName = name;
    ldxHistory = historyEntry;
}

void TargetVideo::setLdxAudio(double sampleRate) { ldxAudioRate = sampleRate; }
bool TargetVideo::isLdx() const { return ldx != nullptr; }

bool TargetVideo::writeAudio(const QByteArray &samples)
{
    if (!ldx || ldx->audio == 0) return false;
    // (stamped with the time of the next field: the audio of the fields written so far)
    return ldx->writer.writeBlock(ldx->audio, fieldTimeNs(ldx->fields, ldxSystem), samples);
}

#else

void TargetVideo::setLdxVideo(qint32, qint32, double, const QString &, const QJsonObject &) {}
void TargetVideo::setLdxAudio(double) {}
bool TargetVideo::isLdx() const { return false; }
bool TargetVideo::writeAudio(const QByteArray &) { return false; }

#endif

bool TargetVideo::open(const QString &fileName, VideoSystem system, Cvbs::SampleEncoding encoding)
{
    cvbs = fileName != "-" && Cvbs::isDataFileName(fileName);
    hasPendingField = false;
    ldxSystem = system;

    if (fileName != "-" && LdxCapture::isLdxName(fileName)) {
#ifdef LDDECODE_WITH_LDX
        if (fileName.contains('#')) {
            qCritical().noquote() << "The output" << fileName << "is a track of an LDX file: name the file";
            return false;
        }
        ldx = std::make_shared<LdxOutput>();
        Ldx::Track video;
        video.kind = Ldx::Kind::Tbc;
        video.depth = 16;
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
        if (ldxAudioRate > 0) {
            Ldx::Track audio;
            audio.kind = Ldx::Kind::Pcm;
            audio.depth = 16;
            audio.name = video.name + " audio";
            audio.parameters["sampleRate"] = ldxAudioRate;
            audio.parameters["channels"] = 2;
            audio.parameters["track"] = static_cast<int>(video.number);
            ldx->writer.addTrack(audio);
            ldx->audio = audio.number;
        }
        if (!ldxHistory.isEmpty()) ldx->writer.setHistory(QJsonArray { ldxHistory });
        QString error;
        if (!ldx->writer.open(fileName, error)) {
            qCritical().noquote() << "Unable to open output LDX file" << fileName << "-" << error;
            ldx.reset();
            return false;
        }
        if (cvbs) {
            qCritical() << "CVBS video cannot be written to an LDX file here";
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

bool TargetVideo::writeField(const QVector<quint16> &fieldData)
{
#ifdef LDDECODE_WITH_LDX
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
    // stdout carries whatever the input is
    if (outputFileName == "-") return true;

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
