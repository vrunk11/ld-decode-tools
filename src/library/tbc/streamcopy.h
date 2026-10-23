/******************************************************************************
 * streamcopy.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STREAMCOPY_H
#define STREAMCOPY_H

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

class TargetVideo;

// The other streams of the sources of a stack or of a correction (EFM, AC3, RF, the 24-bit audio of a CVBS capture), copied into the LDX file the tool writes.
// They are copied as they are, block by block, with the times they have in their source: they are not realigned to the frames of the output, which is right
// when the source is the first one and is not shifted, and which the history says in any case. (The analogue audio is not one of them: it is written
// from the fields, see FieldAudio.)
class StreamCopy
{
public:
    // A stream of an LDX source that can be copied
    struct Candidate {
        qint32 source = 0;           // the source it is of, from 0
        QString file;                // its LDX file
        quint32 track = 0;           // the track in that file
        QString kind;                // efm, ac3, rf, pcm
        QString codec;               // raw, flac
        qint32 depth = 0;
        QString name;
        QJsonObject parameters;
        qint64 blocks = 0;
        bool derived = false;        // the analogue audio: written from the fields, not copied
        // What a person sees: "efm - 1 234 blocks - name"
        QString description() const;
        // What the command line and the session call it: the number of the source (from 1) and the track
        QString selector() const;
    };

    // The streams of the sources (the names of the sources the tool was given: an LDX file, or anything else, which has none)
    static QVector<Candidate> candidates(const QStringList &sources);

    // The default choice: the streams of the first source (the reference) that are not video or analogue audio
    static QVector<Candidate> defaultChoice(const QVector<Candidate> &all);

    // A choice given as text: "none", "default", or a list of "N:efm", "N:ac3", "N:rf", "N:audio" (the 24-bit audio), "N:all" or "N:#id" (the track id), where N is the
    // number of the source from 1. Returns false, with the reason, if it names what is not there.
    static bool parseChoice(const QString &text, const QVector<Candidate> &all, QVector<Candidate> &chosen, QString &error);

    // Where the frames of the output are in the stream of a source, so that a reader can find the part of a stream that goes with a frame of the video even though the stream is
    // copied as it is (with the times it has in its source): sourceFrames holds, for each frame of the output (index 0 is the frame 1), the frame of the capture the stream
    // belongs to that it is made of, or 0 for none (a padded frame, a frame the source does not have). The object is {"unit": "frame", "segments": [[output frame,
    // source frame, how many], ...]}: in a segment both go up by one; a source frame of 0 is none. A source that is not shifted at all is one segment, the same frame
    // in both. The time of a frame in a stream is (frame - 1) times the period of the video, from the start of the stream's own timeline, as in the file it comes from.
    static QJsonObject alignmentOf(const QVector<qint32> &sourceFrames);
    // In words, for the history: not shifted, shifted by so many frames, or in so many pieces
    static QString alignmentSummary(const QJsonObject &alignment);

    // A sentence for the history of the output: what was copied, from where, and that it was not realigned
    static QString historyDetail(const QVector<Candidate> &chosen, const QStringList &sources, const QMap<qint32, QJsonObject> &alignments = QMap<qint32, QJsonObject>());

    // Open the sources of the streams that were chosen, and declare their tracks in the output (before TargetVideo::open)
    bool prepare(const QVector<Candidate> &chosen, TargetVideo &target, QString &error, const QMap<qint32, QJsonObject> &alignments = QMap<qint32, QJsonObject>());
    // Write the blocks whose time is before timeNs, or all that are left (finish); true if they were all written
    bool writeUpTo(TargetVideo &target, qint64 timeNs);
    bool finish(TargetVideo &target);
    bool isEmpty() const;

    StreamCopy();
    ~StreamCopy();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // STREAMCOPY_H
