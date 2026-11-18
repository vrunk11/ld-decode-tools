/******************************************************************************
 * stackaudio.h
 * ld-analyse - the audio of a stack: the audio of its sources, one after the other
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STACKAUDIO_H
#define STACKAUDIO_H

#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWaitCondition>
#include <atomic>
#include <functional>
#include <map>
#include <memory>

#include "audioreader.h"
#include "stackedvideo.h"

class QTemporaryFile;
class QThread;

// A stack has no audio of its own: the sounds of its sources cannot be stacked (they are not on the same clock), so the audio of a stack is, for each stretch of its
// timeline, the audio of one of its sources. Where several sources have audio, the one that goes on the longest without a break is the one that is heard, and it
// is heard until it ends: the audio changes source as seldom as can be. Where none has audio there is silence.
//
// The audio of a source begins at the frame where the source begins on the timeline (the numbers of the disc of its first frame, and its offset) and goes on from there
// at its own pace: nothing in it says where it is on the disc (the EFM of a LaserDisc has no time), so it is not looked for. If it drifts a little, it does.
// It is made in a thread of its own into a file of raw 16-bit stereo audio (44.1 kHz) that grows as it is made, which the player and the waveform read.
//
// There are four of them, by what the sources give: the analogue audio, the digital audio (decoded from the EFM: that of the file when it has some, else the EFM
// itself, which the window decodes when it is asked to, source after source), the mix (the analogue audio of a source, else its digital audio, else what was imported
// to it: what a source that has only one of them gives) and what was imported.
class StackAudio : public QObject
{
    Q_OBJECT
public:
    enum class Kind { Analogue, Digital, Mixed, Imported };

    // A stretch of the timeline (frames from 0, `from` to `to` not included) heard from a source (an index of the spans; -1: silence)
    struct Segment {
        qint32 from = 0;
        qint32 to = 0;
        qint32 source = -1;
    };

    // What a source gives for a kind of audio
    struct Pick {
        bool has = false;
        AudioTrackInfo track;           // the audio it has, when it has it as a track
        QString efm;                    // else the EFM it has, to be decoded (the window decodes it, see efmNeeded)
    };

    // The EFM a source has to be decoded (the file that has it); empty if it has none
    using EfmOf = std::function<QString(const StackAudioSpan &)>;

    explicit StackAudio(QObject *parent = nullptr);
    ~StackAudio() override;

    // Which source is heard where: the sources that have audio are those that `hasAudio` says (one for each span); `start` and `end` are the frames each of them
    // covers (end not included). Apart from the audio, so that it can be tried.
    static QVector<Segment> choose(const QVector<qint32> &start, const QVector<qint32> &end, const QVector<bool> &hasAudio, qint32 timelineFrames);
    // What a source gives
    static Pick pick(const StackAudioSpan &span, Kind kind, const EfmOf &efmOf);
    // Whether any of the sources gives audio of that kind
    static bool available(const QVector<StackAudioSpan> &spans, Kind kind, const EfmOf &efmOf);

    // Work out the audio of the stack of these sources, and make the file it goes to (empty for now). False, with the reason, when no source gives any.
    bool prepare(const QVector<StackAudioSpan> &spans, qint32 timelineFrames, double framesPerSecond, Kind kind, const EfmOf &efmOf, QString &error);
    // Begin making it (once)
    void begin();
    // prepare and begin
    bool start(const QVector<StackAudioSpan> &spans, qint32 timelineFrames, double framesPerSecond, Kind kind, const EfmOf &efmOf, QString &error);
    // Stop making it
    void stop();

    // The EFM of a source that efmNeeded asked for is decoded: the file of its audio (raw 16-bit stereo, 44.1 kHz), empty if it could not be
    void efmDecoded(qint32 source, const QString &pcmFile);

    QString cacheFile() const;
    static constexpr double sampleRate() { return 44100.0; }
    Kind kind() const { return audioKind; }
    // Which source is heard where, as a text for a person
    QString description() const { return text; }
    const QVector<Segment> &segments() const { return plan; }
    bool isStarted() const { return started; }
    bool isRunning() const { return running; }
    // Whether some of what is heard has to be decoded from the EFM first (it takes a while to come)
    bool needsDecoding() const;

signals:
    void grew();
    void finished(bool success, const QString &message);
    // The audio of this source (an index of the spans) is wanted, and it is EFM: decode it, and say when it is done (efmDecoded). They are asked for in the order they are heard.
    void efmNeeded(qint32 source, const QString &efmFile);

private:
    std::unique_ptr<QTemporaryFile> cache;
    QThread *worker = nullptr;
    std::atomic_bool cancel{false};
    std::atomic_bool running{false};
    bool started = false;
    Kind audioKind = Kind::Analogue;
    QVector<StackAudioSpan> spans;
    QVector<Pick> picks;
    double fps = 25.0;
    QVector<Segment> plan;
    QString text;

    QMutex decodedMutex;
    QWaitCondition decodedWake;
    std::map<qint32, QString> decoded;      // the audio of the EFM of a source, when it is decoded (empty: it could not be)
};

#endif // STACKAUDIO_H
