/******************************************************************************
 * ldx.h
 * ld-decode-tools LDX library - reading and writing the LDX container
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDX_H
#define LDX_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <cstdio>
#include <memory>

// The LDX container (docs/Specifications/container-format.md): a Matroska file that follows a profile. Each
// stream is a track, and the data are blocks stamped with a time in nanoseconds. This is the first slice of it:
// raw tracks, and the history, written and read from a file.
namespace Ldx {

constexpr int ProfileVersion = 1;

enum class Kind { Tbc, Cvbs, Rf, Pcm, Efm, Ac3, Metadata, Annotations, Unknown };
enum class Codec { Raw, Flac, Ffv1, Unknown };

QString kindName(Kind kind);
Kind kindFromName(const QString &name);
QString codecName(Codec codec);
Codec codecFromName(const QString &name);

struct Track {
    Kind kind = Kind::Unknown;
    Codec codec = Codec::Raw;
    int depth = 16;                 // the useful bits of a sample (16, 10, 8) or 0 for text
    QString name;
    QJsonObject parameters;         // what the tools know of the stream: system, sample rate, field size...
    double headerRate = 0.0;        // read: the sampling frequency the header of an audio track declares (what a player plays it at), 0 when it is not audio
    // read: the size of the image of a video track, and the size it is shown at (the shape) that its header declares; 0 when there is none
    quint64 headerPixelWidth = 0, headerPixelHeight = 0, headerDisplayWidth = 0, headerDisplayHeight = 0;

    // Given when the track is added (writing) or read (reading)
    quint32 number = 0;             // the number of the track in the file, from 1
    quint64 uid = 0;
};

// The size a video track is shown at when its parameters say a shape (displayAspect "4:3" or "16:9"): what the header of the track holds then. False when
// they do not say one (the image is shown whole, 1:1). `width` and `height` are the size of the image (a field of a TBC, the frame of a CVBS capture).
bool displayShape(const Track &track, int width, int height, int *displayWidth, int *displayHeight);

// Whether the system a track says has the 625 lines of PAL: PAL, and SECAM (NTSC, PAL-M and NTSC-VHD have 525)
inline bool has625Lines(const QString &system) { return system == QLatin1String("PAL") || system == QLatin1String("SECAM"); }

// Whether a track is the metadata of a capture (the ld-decode metadata, namespace "ldx.tbcmetadata", which a track with no namespace is taken to be), and not other metadata of
// a namespace of its own (section 8.4: ldx.rfsource says where the capture comes from)
inline bool isCaptureMetadata(const Track &track)
{
    if (track.kind != Kind::Metadata) return false;
    const QString space = track.parameters.value("namespace").toString();
    return space.isEmpty() || space == QLatin1String("ldx.tbcmetadata");
}

struct Block {
    quint32 track = 0;              // the number of its track
    qint64 timeNs = 0;
    QByteArray data;
};

class Writer
{
public:
    Writer();
    ~Writer();

    // The tracks and the history are given before the file is opened. The number of the track is set
    // in `track`, and is what writeBlock takes.
    void addTrack(Track &track);
    void setHistory(const QJsonArray &entries);

    // The id the next track added to a file gets. It only goes up: the id of a removed track is not given again.
    // By default it is one above the highest id of the tracks.
    void setNextTrackId(quint32 id);

    // A file kept in the LDX file (the session of a stack, for instance), written before the data
    void addAttachment(const QString &name, const QString &mediaType, const QByteArray &data);
    // (in a file that is added to, the attachments given are written at the end, and the last one of a name is the one that counts: with no data it takes the file out)

    bool open(const QString &fileName, QString &error);

    // Write to a stream that cannot be seeked (the standard output, for a pipe): the segment has no size, there are
    // no cues, and nothing is written again. A reader that reads the stream as it comes (Reader::openStream) takes it.
    bool openStream(FILE *file, QString &error);

    // Add to a file that is already written, instead of making one: its tracks are kept, the blocks written go at the
    // end, and the cues are written again. Nothing is moved. Add what is said in the history with addHistory.
    bool openAppend(const QString &fileName, QString &error);
    void addHistory(const QJsonObject &entry);
    // In a file that is added to (openAppend): change the name or the parameters of a track, or add a track, in the list of tracks of the file itself,
    // in the room the Void after it leaves (section 2). False, with the reason, when it does not fit: the file has to be written again then.
    bool replaceTrack(const Track &track, QString &error);
    bool addTrackInPlace(Track &track, QString &error);

    // Blocks are written in the order of their time, all tracks together
    bool writeBlock(quint32 track, qint64 timeNs, const QByteArray &data);
    bool finish(QString &error);

    // Why the last writeBlock failed
    QString lastError() const;

private:
    bool begin(const QString &fileName, QString &error, FILE *stream);

    struct Private;
    std::unique_ptr<Private> d;
};

class Reader
{
public:
    Reader();
    ~Reader();

    bool open(const QString &fileName, QString &error);
    // A stream that cannot be seeked (the standard input): its blocks are read in turn with nextBlock, and there is
    // no index, so no random access
    // (alreadyRead: the first bytes of the stream, if the caller has taken them to see what it is)
    bool openStream(FILE *file, QString &error, const QByteArray &alreadyRead = QByteArray());

    int profile() const;                    // the LDX_PROFILE of the file, 0 if it has none
    QVector<Track> tracks() const;
    QJsonArray history() const;
    quint32 nextTrackId() const;

    // Where the blocks are, from the cues (after buildIndex): for the writer that adds to a file
    struct BlockRef {
        quint32 track;
        qint64 timeNs;
        quint64 clusterPosition;
        quint64 relativePosition;
    };
    QVector<BlockRef> blockRefs() const;
    quint64 cuesPosition() const;                       // in the file, where the cues start
    quint64 segmentPosition() const;                    // in the file, where the segment starts
    quint64 tracksPosition() const;                     // in the file, where the list of tracks starts
    quint64 tracksSpace() const;                        // its length, with the Void that follows it (room for the tracks to change)
    qint64 lastTimeNs() const;                          // the time of the last block of any track

    struct Attachment {
        QString name;
        QString mediaType;
        QByteArray data;
    };
    QVector<Attachment> attachments() const;

    // The next block of the file, in the order it was written. False at the end.
    bool nextBlock(Block &block);

    // Random access to the blocks of a track, for the tools that jump about in a capture (the field of a
    // TBC, by its number). The index is read from the cues at the end of the file; it is built by the first
    // call. Do not mix this with nextBlock on one reader: both move in the file.
    bool buildIndex(QString &error);
    qint64 blockCount(quint32 track) const;              // 0 for a track that has no blocks
    qint64 blockTimeNs(quint32 track, qint64 index) const;
    bool readBlock(quint32 track, qint64 index, Block &block);

    // The attachment with a name, if the file has it
    bool attachment(const QString &name, QByteArray &data) const;

private:
    bool begin(const QString &fileName, QString &error, FILE *stream);

    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace Ldx

#endif // LDX_H
