/******************************************************************************
 * efmdecoding.h
 * ld-analyse - the EFM of a capture decoded to audio by a program, as it is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef EFMDECODING_H
#define EFMDECODING_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryFile>
#include <memory>
#include <vector>

// Two programs decode EFM, and both are offered: the suite (efm-decoder-f2 | efm-decoder-d24 | efm-decoder-audio, piped) and
// ld-process-efm (one program). Either runs on the EFM of a capture, and what it decodes is written, as it comes, to a file
// of raw 16-bit stereo audio (44.1 kHz) that the player and the waveform read while it grows.
class EfmDecoding : public QObject
{
    Q_OBJECT
public:
    enum class Backend { Suite, ProcessEfm };

    struct Options {
        bool noTimecodes = true;        // the EFM of a LaserDisc has none
        bool conceal = true;            // conceal the errors of the audio
        bool pad = false;               // begin the audio at disc time 00:00
    };

    explicit EfmDecoding(Backend backend, QObject *parent = nullptr);
    ~EfmDecoding() override;

    Backend backend() const { return which; }
    static QString backendName(Backend backend);

    // Start on the EFM of a file: an LDX file (capture.ldx, or capture.ldx#4) or an .efm file. False, with the reason, if a
    // program is not there.
    bool start(const QString &efmFile, const Options &options, QString &error);
    void stop();
    bool isRunning() const { return running; }

    QString cacheFile() const { return cache ? cache->fileName() : QString(); }
    // Seconds of audio decoded so far
    double seconds() const { return static_cast<double>(written) / 4.0 / 44100.0; }

signals:
    void grew();                                    // more audio is in the file
    void finished(bool success, const QString &message);

private:
    void readOutput();
    void processFinished();
    QString program(const QString &name) const;

    Backend which;
    std::vector<std::unique_ptr<QProcess>> processes;
    std::unique_ptr<QTemporaryFile> cache;
    qint64 written = 0;
    bool running = false;
    bool stopping = false;
    QString lastError;
    qint32 remaining = 0;
};

#endif // EFMDECODING_H
