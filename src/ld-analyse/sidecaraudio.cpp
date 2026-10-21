/******************************************************************************
 * sidecaraudio.cpp
 * ld-analyse - the audio files beside a capture, with its name, read through ffmpeg
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "sidecaraudio.h"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>

#include "ldxcapture.h"
#include "tbcfiles.h"

namespace SidecarAudio {

// What a capture keeps in files of its own name that are not audio to play (or are read in another way)
static const QStringList notAudio {
    "lds", "ldf", "rf", "u8", "s16", "tbc", "tbcy", "tbcc", "cvbs", "cvbsy", "cvbsc", "json", "db", "ldx", "stack", "efm", "pcm", "txt", "log", "png", "jpg", "jpeg", "gif",
    "bmp", "csv", "xml", "md", "sqlite", "orc", "orcprj", "ini", "bak", "tmp", "part", "lock", "ac3x", "rf", "lds", "ldf", "raw", "rgb", "yuv", "y4m",
};

bool ffmpegAvailable()
{
    QProcess probe;
    probe.start("ffmpeg", { "-version" });
    return probe.waitForFinished(5000) && probe.exitCode() == 0;
}

// ffmpeg is asked to read a tenth of a second of the audio of the file: it succeeds if there is an audio stream. The RF samples of a
// capture (a FLAC of the Domesday Duplicator or ld-compress, which ffmpeg reads as audio) are not audio to play: they are told by their
// rate, which is no rate of sound (40000 Hz, 20000...), the usual ones only are taken
static bool hasAudio(const QString &fileName)
{
    QProcess probe;
    probe.start("ffmpeg", { "-hide_banner", "-i", fileName, "-vn", "-t", "0.1", "-f", "null", "-" });
    if (!probe.waitForFinished(15000) || probe.exitCode() != 0) return false;
    const QString text = QString::fromLocal8Bit(probe.readAllStandardError());
    static const QRegularExpression stream(QStringLiteral("Audio: [^\n]*?([0-9]+) Hz"));
    const QRegularExpressionMatch match = stream.match(text);
    if (!match.hasMatch()) return false;
    static const QList<qint32> soundRates { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    return soundRates.contains(match.captured(1).toInt());
}

QStringList find(const QString &captureName)
{
    QStringList found;
    if (captureName.isEmpty()) return found;
    QString path;
    qint32 track;
    LdxCapture::split(captureName, path, track);        // (a track of an LDX file)
    const QFileInfo capture(path);
    // A Y/C capture's second file (.tbcc, or _chroma.tbc) has the name of the first
    QString base = TbcFiles::baseName(capture.fileName());
    if (base == capture.fileName()) base = capture.completeBaseName();
    QStringList candidates;
    const QFileInfoList entries = capture.absoluteDir().entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &entry: entries) {
        if (entry.completeBaseName().compare(base, Qt::CaseInsensitive) != 0) continue;
        if (notAudio.contains(entry.suffix().toLower())) continue;
        candidates.append(entry.absoluteFilePath());
    }
    if (candidates.isEmpty() || !ffmpegAvailable()) return found;
    for (const QString &candidate: candidates) {
        if (hasAudio(candidate)) found.append(candidate);
    }
    return found;
}

} // namespace SidecarAudio

AudioFileDecoding::AudioFileDecoding(QObject *parent) : QObject(parent) {}

AudioFileDecoding::~AudioFileDecoding()
{
    stop();
}

bool AudioFileDecoding::start(const QString &file, QString &error)
{
    stop();
    fileName = file;
    cache = std::make_unique<QTemporaryFile>();
    if (!cache->open()) {
        error = tr("Could not make a file for the audio of %1").arg(QFileInfo(file).fileName());
        return false;
    }
    written = 0;
    stopping = false;
    messages.clear();
    process = std::make_unique<QProcess>();
    connect(process.get(), &QProcess::readyReadStandardOutput, this, &AudioFileDecoding::readOutput);
    connect(process.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        running = false;
        messages += QString::fromLocal8Bit(process->readAllStandardError());
        const bool ok = !stopping && status == QProcess::NormalExit && code == 0 && written > 0;
        if (!stopping) {
            emit finished(ok, ok ? tr("%1: %2 seconds").arg(QFileInfo(fileName).fileName()).arg(seconds(), 0, 'f', 1)
                                 : tr("ffmpeg could not read %1: %2").arg(QFileInfo(fileName).fileName(), messages.trimmed()));
        }
    });
    running = true;
    process->start("ffmpeg", { "-v", "error", "-i", file, "-vn", "-ac", "2", "-ar", "44100", "-f", "s16le", "-" });
    if (!process->waitForStarted(5000)) {
        running = false;
        error = tr("ffmpeg could not be started: it must be installed and in the PATH");
        return false;
    }
    return true;
}

void AudioFileDecoding::readOutput()
{
    if (!process || !cache) return;
    const QByteArray data = process->readAllStandardOutput();
    if (data.isEmpty()) return;
    cache->write(data);
    cache->flush();
    written += data.size();
    emit grew();
}

void AudioFileDecoding::stop()
{
    if (!process) return;
    stopping = true;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(2000);
    process.reset();
    running = false;
}
