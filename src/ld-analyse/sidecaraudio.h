/******************************************************************************
 * sidecaraudio.h
 * ld-analyse - the audio files beside a capture, with its name, read through ffmpeg
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SIDECARAUDIO_H
#define SIDECARAUDIO_H

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <memory>

// The audio files that are beside a capture and have its name (capture.tbc, capture.wav, capture.flac, capture.mp3, ...): a
// reference recording, the audio of another decoder. Any file that ffmpeg reads and finds audio in is one, whatever its extension.
namespace SidecarAudio {

// The files in the folder of a capture (a .tbc, .cvbs or .ldx, or any other name) that have its base name, are not data of the
// capture (tbc, json, efm, pcm...) and that ffmpeg finds audio in. Asks ffmpeg, so it takes a moment for each candidate.
QStringList find(const QString &captureName);

// Whether ffmpeg is there to be run
bool ffmpegAvailable();

} // namespace SidecarAudio

// One audio file, decoded by ffmpeg to 16-bit stereo at 44.1 kHz into a file that grows (the player and the waveform read it as it
// does), so that a long recording does not wait for the end of its decoding, nor sit in memory.
class AudioFileDecoding : public QObject
{
    Q_OBJECT
public:
    explicit AudioFileDecoding(QObject *parent = nullptr);
    ~AudioFileDecoding() override;

    bool start(const QString &fileName, QString &error);
    void stop();
    bool isRunning() const { return running; }

    QString cacheFile() const { return cache ? cache->fileName() : QString(); }
    double seconds() const { return static_cast<double>(written) / 4.0 / 44100.0; }

signals:
    void grew();
    void finished(bool success, const QString &message);

private:
    void readOutput();

    std::unique_ptr<QProcess> process;
    std::unique_ptr<QTemporaryFile> cache;
    qint64 written = 0;
    bool running = false;
    bool stopping = false;
    QString fileName;
    QString messages;
};

#endif // SIDECARAUDIO_H
