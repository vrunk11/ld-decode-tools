/******************************************************************************
 * ldxcapture.h
 * ld-decode-tools TBC library - a capture kept in an LDX file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXCAPTURE_H
#define LDXCAPTURE_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <map>
#include <memory>

// A capture kept in an LDX file (docs/Specifications/container-format.md), as the tools see it: a name that
// says which one, the fields of its TBC track by number, and its metadata. A name is the path of the file,
// and, for one of the tracks of a file that holds several, "#" and the id of the track:
//
//     capture.ldx        the stack of the session the file holds, or, if it has none, its first TBC track
//     capture.ldx#3      the track 3
//
// (When the library is built without LDX support, every call fails and says so.)
// The current metadata of a capture, from the blocks of its metadata track: a block with the parameters of the video
// is the metadata (the last one is current), and the blocks of single fields (a stream whose metadata came field by
// field) are put in it, the last block of a field being its current metadata.
class LdxMetadataMerger
{
public:
    void add(const QByteArray &data);
    QByteArray current() const;

private:
    QByteArray base;
    std::map<int, QByteArray> fieldJson;
};

class LdxCapture
{
public:
    LdxCapture();
    ~LdxCapture();

    // A line of the history of an LDX file, which says who did what, when and with what, to be used later (to know how
    // a capture was made, to do it again, to tell a tool what it has to do): the tool and its version, the command, the
    // time (UTC), the arguments of the program, and whatever the tool adds (the files it read and wrote, the options it
    // took, what it counted) in extra, as members of the entry.
    static QJsonObject historyEntry(const QString &command, const QString &detail = QString(), const QJsonObject &extra = QJsonObject());

    // The history of the capture a name gives, to which a tool adds its own entry when it writes a capture made from it:
    // that of the LDX file or stream ("-"), empty for anything else
    static QJsonArray historyOf(const QString &name);

    // Put the output of a tool in an LDX file as a track of its own: the decoded audio of EFM (kind "pcm", with the parameters
    // "source": "efm", "sampleRate": 44100, "channels": 2), AC3 data ("ac3"), EFM t-values ("efm"), or a text of annotations
    // ("annotations"). The data are the file dataFile (raw, for audio 16-bit stereo), put in blocks and stamped over the time of
    // the capture: audio by its sample rate, the rest in proportion to where it is in the file. `about` is the id of the video
    // track it goes with (0: the first). The LDX file is written again, in its place, with the track and an entry of the
    // history added to it.
    static bool addTrackFromFile(const QString &ldxPath, const QString &kind, const QString &dataFile, const QJsonObject &parameters,
                                 const QString &trackName, qint32 about, const QJsonObject &historyEntry, QString &error);

    // Whether a name is that of an LDX file or of one of its tracks
    static bool isLdxName(const QString &name);
    // The path and the track (0 when none is named) of a name
    static void split(const QString &name, QString &path, qint32 &track);
    // The name of a track of a file
    static QString trackName(const QString &path, qint32 track);

    // A Y/C capture is two tracks of a file: the luma (or composite) track, and a chroma track, a `tbc` track whose parameters
    // say "role": "chroma" and "track": the id of the luma. These give the name of one from the other (empty if there is
    // none), and whether a name is a chroma track.
    static bool isChromaTrack(const QString &name);
    static QString lumaOfChroma(const QString &name);
    static QString chromaOfLuma(const QString &name);

    // Whether the capture a name gives is CVBS (a cvbs track) and not a TBC
    static bool isCvbs(const QString &name);

    // The TBC tracks of an LDX file, with their names: the captures it holds
    static bool listCaptures(const QString &path, QVector<qint32> &tracks, QStringList &names, QString &error);

    // Whether the file keeps a stacking session, and the session
    static bool hasSession(const QString &path);
    static bool readSession(const QString &path, QByteArray &session, QString &error);

    // Open the capture a name gives (a track; the session of the file is not a capture)
    bool open(const QString &name, QString &error);
    qint32 track() const;
    qint32 numberOfFields() const;
    QJsonObject parameters() const;                        // of the track
    // The whole field, from 1. Empty if it is not there.
    QVector<quint16> getField(qint32 fieldNumber);
    // The ld-decode metadata of the capture, as JSON
    bool readMetadata(QByteArray &json, QString &error);

    // Keep new metadata for a capture in its LDX file: a block added at the end of the file, which is the current one from
    // then on (what was there stays, and the history says what was done)
    static bool writeMetadata(const QString &name, const QByteArray &json, QString &error);

    // The analogue audio of the capture: the PCM track that is about it (16-bit stereo), by samples (a sample is a pair)
    // wide: the 24-bit audio of a CVBS capture (a pair of samples is 6 bytes), read with getAudioBytes, instead of the 16-bit
    // audio of a TBC capture (4 bytes, read with getAudio)
    // audioTrack: the id of a pcm track to open (whatever its source and width), 0 for the analogue audio of the capture
    bool openAudio(const QString &name, QString &error, bool wide = false, qint32 audioTrack = 0);
    // The pcm tracks about the capture a name gives: the analogue audio, the audio decoded from EFM, imported audio
    struct AudioTrack {
        qint32 id = 0;
        QString name;
        QString source;         // "analogue", "efm" or the source the track says ("import")
        double sampleRate = 0;
        qint32 bits = 16;
    };
    static QVector<AudioTrack> audioTracks(const QString &name);
    qint64 audioSamples() const;
    double audioSampleRate() const;
    qint32 audioBytesPerSample() const;                  // 4 for 16-bit stereo, 6 for 24-bit
    // Left and right in turn, or less if the audio ends
    QVector<qint16> getAudio(qint64 startSample, qint64 numberOfSamples);
    // The samples as they are in the track, a pair after a pair (4 bytes each for 16 bits, 6 for 24)
    QByteArray getAudioBytes(qint64 startSample, qint64 numberOfSamples);

private:
    // The metadata track that is about this capture (0 if none)
    quint32 metadataTrackNumber() const;

    struct Private;
    std::unique_ptr<Private> d;
};

#endif // LDXCAPTURE_H
