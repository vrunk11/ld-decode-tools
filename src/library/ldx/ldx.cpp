/******************************************************************************
 * ldx.cpp
 * ld-decode-tools LDX library - reading and writing the LDX container
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldx.h"
#include "ldxcodec.h"

#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <algorithm>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <deque>
#include <stdexcept>
#include <map>
#include <vector>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxAttached.h>
#include <matroska/KaxAttachments.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxTag.h>
#include <matroska/KaxTags.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackEntryData.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTracks.h>

using namespace libebml;
using namespace libmatroska;

namespace Ldx {

namespace {

// One tick of the time stamps of the file is a microsecond. A block is stamped from the start of its cluster
// in 16 bits, so a cluster covers about 30 ms, which is a field or two.
constexpr int64_t TimecodeScaleNs = 1000;
constexpr qint64 ClusterSpanNs = 30000000;

// Space after the list of tracks, so that a track can be added to a finished file without moving the data
constexpr quint64 TrackPaddingBytes = 4096;

const char *PcmCodecId = "A_PCM/INT/LIT";
constexpr double StandInSampleRate = 48000.0;   // the real rate is in the parameters of the track

struct KindName { Kind kind; const char *name; };
const KindName kindNames[] = {
    { Kind::Tbc, "tbc" }, { Kind::Cvbs, "cvbs" }, { Kind::Rf, "rf" }, { Kind::Pcm, "pcm" },
    { Kind::Efm, "efm" }, { Kind::Ac3, "ac3" }, { Kind::Metadata, "metadata" }, { Kind::Annotations, "annotations" },
};

// The samples of these are carried in an audio track, the rest are binary tracks
bool isSampleKind(Kind kind)
{
    return kind == Kind::Tbc || kind == Kind::Cvbs || kind == Kind::Rf || kind == Kind::Pcm;
}

// The frames of a cvbs track: the samples of a frame, and the lines they make (a line is a row of the image of FFV1)
struct CvbsShape {
    int width = 0;
    qint64 samples = 0;
    int height = 0;
    quint16 pad = 0;
    bool ok() const { return width > 0 && samples > 0; }
};

CvbsShape cvbsShape(const Track &track)
{
    CvbsShape shape;
    shape.width = track.parameters.value("lineWidth").toInt();
    shape.samples = static_cast<qint64>(track.parameters.value("frameSamples").toDouble());
    shape.pad = static_cast<quint16>(track.parameters.value("padValue").toInt(0));
    if (shape.ok()) shape.height = ffv1Height(shape.samples, shape.width);
    return shape;
}

QByteArray trackDescription(const Track &track)
{
    QJsonObject object;
    object["ldx"] = ProfileVersion;
    object["kind"] = kindName(track.kind);
    object["codec"] = codecName(track.codec);
    object["depth"] = track.depth;
    object["parameters"] = track.parameters;
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void setSimpleTag(KaxTag &tag, KaxTagSimple *&simple, const char *name, const QByteArray &value)
{
    simple = simple ? &GetNextChild<KaxTagSimple>(tag, *simple) : &GetChild<KaxTagSimple>(tag);
    GetChild<KaxTagName>(*simple).SetValueUTF8(name);
    GetChild<KaxTagString>(*simple).SetValueUTF8(value.constData());
}

// A pipe: written to or read from in order, and never seeked
class StreamOutput : public IOCallback
{
public:
    explicit StreamOutput(FILE *file) : file(file) {}
    uint32 read(void *, size_t) override { return 0; }
    void setFilePointer(int64 offset, seek_mode mode) override
    {
        // Only to where it already is
        const bool here = (mode == seek_current && offset == 0) || (mode == seek_beginning && static_cast<uint64>(offset) == written);
        if (!here) throw std::runtime_error("a stream cannot be written again");
    }
    size_t write(const void *buffer, size_t size) override
    {
        const size_t done = fwrite(buffer, 1, size, file);
        written += done;
        return done;
    }
    uint64 getFilePointer() override { return written; }
    void close() override { fflush(file); }

private:
    FILE *file;
    uint64 written = 0;
};

// What a stream is read from: a window of what came in, so that the little that is read again (the id of an element
// that was not the one looked for) can be, and what is passed over is read and let go. A read gives what has come, and
// does not wait for the buffer to be full, so a producer that sends in blocks is followed as it goes.
class StreamInput : public IOCallback
{
public:
    explicit StreamInput(FILE *file) : file(file) {}
    uint32 read(void *buffer, size_t size) override
    {
        char *out = static_cast<char *>(buffer);
        size_t done = 0;
        while (done < size) {
            if (position >= windowEnd() && !fill()) break;
            const size_t offset = static_cast<size_t>(position - windowStart);
            const size_t count = std::min(size - done, static_cast<size_t>(window.size()) - offset);
            std::memcpy(out + done, window.constData() + offset, count);
            done += count;
            position += count;
        }
        return static_cast<uint32>(done);
    }
    void setFilePointer(int64 offset, seek_mode mode) override
    {
        int64 target = 0;
        if (mode == seek_beginning) target = offset;
        else if (mode == seek_current) target = static_cast<int64>(position) + offset;
        else throw std::runtime_error("a stream has no end to seek from");
        if (target < static_cast<int64>(windowStart)) throw std::runtime_error("a stream cannot be read again from so far back");
        // (forward is read through)
        while (target > static_cast<int64>(windowEnd()) && fill()) {
        }
        position = static_cast<uint64>(qMin<int64>(target, static_cast<int64>(windowEnd())));
    }
    size_t write(const void *, size_t) override { return 0; }
    uint64 getFilePointer() override { return position; }
    void close() override {}

private:
    uint64 windowEnd() const { return windowStart + static_cast<uint64>(window.size()); }
    bool fill()
    {
        // What was passed over long ago is let go
        if (window.size() > MaxWindow && position - windowStart > KeepBehind) {
            const qint64 drop = static_cast<qint64>(position - windowStart - KeepBehind);
            window.remove(0, static_cast<int>(drop));
            windowStart += static_cast<uint64>(drop);
        }
        char chunk[ChunkBytes];
#ifdef Q_OS_WIN
        const int got = _read(_fileno(file), chunk, ChunkBytes);
#else
        const int got = static_cast<int>(::read(fileno(file), chunk, ChunkBytes));
#endif
        if (got <= 0) return false;
        window.append(chunk, got);
        return true;
    }

    static constexpr int ChunkBytes = 1 << 16;
    static constexpr int MaxWindow = 64 << 20;
    static constexpr int KeepBehind = 16 << 20;
    FILE *file;
    QByteArray window;
    uint64 windowStart = 0;
    uint64 position = 0;
};

// A pipe is bytes, and not text
void makeBinary(FILE *file)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(file), _O_BINARY);
#else
    Q_UNUSED(file);
#endif
}

} // namespace

QString kindName(Kind kind)
{
    for (const KindName &entry: kindNames) {
        if (entry.kind == kind) return QString::fromLatin1(entry.name);
    }
    return QString();
}

Kind kindFromName(const QString &name)
{
    for (const KindName &entry: kindNames) {
        if (name == QLatin1String(entry.name)) return entry.kind;
    }
    return Kind::Unknown;
}

QString codecName(Codec codec)
{
    switch (codec) {
    case Codec::Raw: return QStringLiteral("raw");
    case Codec::Flac: return QStringLiteral("flac");
    case Codec::Ffv1: return QStringLiteral("ffv1");
    default: return QString();
    }
}

Codec codecFromName(const QString &name)
{
    if (name == QLatin1String("raw")) return Codec::Raw;
    if (name == QLatin1String("flac")) return Codec::Flac;
    if (name == QLatin1String("ffv1")) return Codec::Ffv1;
    return Codec::Unknown;
}

// ---------------------------------------------------------------------------------------------------------------
// Writer

struct Writer::Private
{
    QVector<Track> tracks;
    QJsonArray history;

    std::unique_ptr<IOCallback> output;
    bool streaming = false;                    // written to a pipe: no size, no cues, nothing written again
    std::unique_ptr<KaxSegment> segment;
    KaxTracks *trackList = nullptr;
    std::map<quint32, KaxTrackEntry *> entries;            // by the id (number) of the track
    std::map<int, QByteArray> flacPrivates;                // what the FLAC tracks give the decoder before the data, by channels
    std::map<quint32, std::unique_ptr<Ffv1Encoder>> ffv1Encoders;   // one for each FFV1 track
    std::map<quint32, QByteArray> ffv1Privates;            // what the decoder of an FFV1 track is given before the data
    quint32 nextId = 1;                                    // the id the next track gets
    struct Attached {
        QString name;
        QString mediaType;
        QByteArray data;
    };
    std::vector<Attached> attachments;
    KaxCues cues;

    KaxCluster *cluster = nullptr;
    qint64 clusterStartNs = 0;
    std::vector<QByteArray> held;                          // the data of the blocks of the cluster, until it is written
    struct BlockCue {
        qint64 timeNs;
        quint32 track;
        KaxBlockGroup *block;           // its position is known once the cluster is written
    };
    struct CueRecord {
        qint64 timeNs;
        quint32 track;
        quint64 clusterPosition;        // from the start of the data of the segment
        quint64 relativePosition;       // of the block, from the start of the data of the cluster
    };
    std::vector<BlockCue> clusterBlocks;       // the blocks of the cluster being written
    std::vector<CueRecord> cueRecords;         // of all the blocks written, for the cues at the end
    bool finished = false;
    bool appending = false;                    // a file that was already written is being added to
    QString path;
    QString error;

    const Track *trackInfo(quint32 number) const
    {
        for (const Track &track: tracks) {
            if (track.number == number) return &track;
        }
        return nullptr;
    }

    // The channels of a track of samples: an audio track says (stereo by default), the rest are one
    static int channelsOf(const Track &track)
    {
        return track.kind == Kind::Pcm ? qBound(1, track.parameters.value("channels").toInt(2), 8) : 1;
    }

    // FLAC is for tracks of 16-bit samples, and needs ffmpeg
    bool prepareCodecs(QString &error)
    {
        for (const Track &track: tracks) {
            if (track.codec == Codec::Flac) {
                if (track.kind != Kind::Tbc && track.kind != Kind::Rf && track.kind != Kind::Pcm) {
                    error = QObject::tr("FLAC is for tracks of 16-bit samples (tbc, rf, pcm), and not for a track of %1").arg(kindName(track.kind));
                    return false;
                }
                const int channels = channelsOf(track);
                if (flacPrivates[channels].isEmpty()) {
                    flacPrivates[channels] = flacCodecPrivate(channels, error);
                    if (flacPrivates[channels].isEmpty()) return false;
                }
            } else if (track.codec == Codec::Ffv1) {
                if (track.kind != Kind::Cvbs) {
                    error = QObject::tr("FFV1 is for the frames of a cvbs track, and not for a track of %1").arg(kindName(track.kind));
                    return false;
                }
                const CvbsShape shape = cvbsShape(track);
                if (!shape.ok()) {
                    error = QObject::tr("A cvbs track needs the length of a line and the samples of a frame in its parameters (lineWidth, frameSamples)");
                    return false;
                }
                if (!ffv1Encoders.count(track.number)) {
                    auto encoder = std::make_unique<Ffv1Encoder>();
                    if (!encoder->open(shape.width, shape.height, error)) return false;
                    ffv1Privates[track.number] = encoder->extradata();
                    ffv1Encoders[track.number] = std::move(encoder);
                }
            } else if (track.codec != Codec::Raw) {
                error = QObject::tr("The %1 codec is not written by this build yet").arg(codecName(track.codec));
                return false;
            }
        }
        return true;
    }

    // The entries of the tracks, for the file to be written or, when added to, for the blocks to name
    void buildEntries()
    {
        KaxTrackEntry *previous = nullptr;
        for (const Track &track: tracks) {
            KaxTrackEntry &entry = previous ? GetNextChild<KaxTrackEntry>(*trackList, *previous) : GetChild<KaxTrackEntry>(*trackList);
            previous = &entry;
            entries[track.number] = &entry;

            GetChild<KaxTrackNumber>(entry).SetValue(track.number);
            GetChild<KaxTrackUID>(entry).SetValue(track.uid);
            GetChild<KaxTrackFlagLacing>(entry).SetValue(track.codec == Codec::Flac ? 1 : 0);
            if (!track.name.isEmpty()) GetChild<KaxTrackName>(entry).SetValueUTF8(track.name.toUtf8().constData());

            if (track.codec == Codec::Ffv1) {
                // FFV1: a video track, whose frames are the images of the frames of CVBS
                const CvbsShape shape = cvbsShape(track);
                GetChild<KaxTrackType>(entry).SetValue(track_video);
                GetChild<KaxCodecID>(entry).SetValue("V_FFV1");
                KaxTrackVideo &video = GetChild<KaxTrackVideo>(entry);
                GetChild<KaxVideoPixelWidth>(video).SetValue(shape.width);
                GetChild<KaxVideoPixelHeight>(video).SetValue(shape.height);
            } else if (isSampleKind(track.kind)) {
                GetChild<KaxTrackType>(entry).SetValue(track_audio);
                GetChild<KaxCodecID>(entry).SetValue(track.codec == Codec::Flac ? "A_FLAC" : PcmCodecId);
                KaxTrackAudio &audio = GetChild<KaxTrackAudio>(entry);
                GetChild<KaxAudioSamplingFreq>(audio).SetValue(StandInSampleRate);
                GetChild<KaxAudioChannels>(audio).SetValue(channelsOf(track));
                GetChild<KaxAudioBitDepth>(audio).SetValue(16);
            } else {
                GetChild<KaxTrackType>(entry).SetValue(33);     // metadata: the codec says what is in it
                GetChild<KaxCodecID>(entry).SetValue(QByteArray("D_LDX/" + kindName(track.kind).toUpper().toLatin1()).constData());
            }
            // (a FLAC track has what its decoder needs, and the description of every track is also in a tag)
            const QByteArray description = track.codec == Codec::Flac ? flacPrivates[channelsOf(track)]
                                           : (track.codec == Codec::Ffv1 ? ffv1Privates[track.number] : trackDescription(track));
            GetChild<KaxCodecPrivate>(entry).CopyBuffer(reinterpret_cast<const binary *>(description.constData()), description.size());
            entry.SetGlobalTimecodeScale(TimecodeScaleNs);
        }
    }

    void flushCluster()
    {
        if (!cluster) return;
        cluster->Render(*output, cues);
        const quint64 position = cluster->GetElementPosition() - (segment->GetElementPosition() + segment->HeadSize());
        // Where each block is in the data of the cluster: after the elements that come before it, whose sizes are
        // known now (the position the library gives for a child changes with the options of the cluster)
        std::map<const EbmlElement *, quint64> offsets;
        quint64 offset = 0;
        for (EbmlElement *child: cluster->GetElementList()) {
            offsets[child] = offset;
            offset += child->ElementSize(false);
        }
        for (const BlockCue &block: clusterBlocks) {
            const auto found = offsets.find(block.block);
            cueRecords.push_back({ block.timeNs, block.track, position, found != offsets.end() ? found->second : 0 });
        }
        clusterBlocks.clear();
        cluster->ReleaseFrames();
        delete cluster;
        cluster = nullptr;
        held.clear();
    }
};

Writer::Writer() : d(new Private) {}

Writer::~Writer()
{
    if (d->output) {
        // Not finished: what was written stays, without its closing
        delete d->cluster;
    }
}

void Writer::addTrack(Track &track)
{
    // The ids are the numbers of the tracks, from 1, never reused in a file. A track that comes from another
    // file keeps its id.
    for (const Track &existing: d->tracks) {
        if (existing.number == track.number) track.number = 0;      // a copy of a track of this file gets an id of its own
    }
    if (track.number == 0) track.number = d->nextId;
    if (track.number >= d->nextId) d->nextId = track.number + 1;
    if (track.uid == 0) track.uid = QRandomGenerator::global()->generate64() | 1;
    d->tracks.append(track);
}

void Writer::setNextTrackId(quint32 id)
{
    d->nextId = qMax(d->nextId, id);
}

void Writer::addAttachment(const QString &name, const QString &mediaType, const QByteArray &data)
{
    d->attachments.push_back({ name, mediaType, data });
}

void Writer::setHistory(const QJsonArray &entries)
{
    d->history = entries;
}

bool Writer::open(const QString &fileName, QString &error)
{
    return begin(fileName, error, nullptr);
}

bool Writer::openStream(FILE *file, QString &error)
{
    return begin(QString(), error, file);
}

bool Writer::begin(const QString &fileName, QString &error, FILE *stream)
{
    try {
        if (!d->prepareCodecs(error)) return false;

        if (stream) {
            makeBinary(stream);
            d->output.reset(new StreamOutput(stream));
            d->streaming = true;
        } else {
            d->output.reset(new StdIOCallback(fileName.toLocal8Bit().constData(), MODE_CREATE));
        }

        EbmlHead head;
        GetChild<EDocType>(head).SetValue("matroska");
        GetChild<EDocTypeVersion>(head).SetValue(4);
        GetChild<EDocTypeReadVersion>(head).SetValue(2);
        head.Render(*d->output, true);

        d->segment.reset(new KaxSegment);
        if (d->streaming) d->segment->SetSizeInfinite();        // a pipe has no end that is known
        d->segment->WriteHead(*d->output, 8);

        KaxInfo &info = GetChild<KaxInfo>(*d->segment);
        GetChild<KaxTimecodeScale>(info).SetValue(TimecodeScaleNs);
        GetChild<KaxMuxingApp>(info).SetValueUTF8("ld-decode-tools ldx");
        GetChild<KaxWritingApp>(info).SetValueUTF8("ld-decode-tools ldx");
        info.Render(*d->output, false);

        d->trackList = &GetChild<KaxTracks>(*d->segment);
        d->buildEntries();
        d->trackList->Render(*d->output, false);

        // Space for tracks to be added to the file later (a pipe is not added to)
        if (!d->streaming) {
            EbmlVoid padding;
            padding.SetSize(TrackPaddingBytes);
            padding.Render(*d->output);
        }

        // The profile and the history, before the data so that a reader (or a pipe) has them first
        KaxTags &tags = GetChild<KaxTags>(*d->segment);
        KaxTag &tag = GetChild<KaxTag>(tags);
        GetChild<KaxTagTargets>(tag);
        KaxTagSimple *simple = nullptr;
        setSimpleTag(tag, simple, "LDX_PROFILE", QByteArray::number(ProfileVersion));
        setSimpleTag(tag, simple, "LDX_HISTORY", QJsonDocument(d->history).toJson(QJsonDocument::Compact));
        setSimpleTag(tag, simple, "LDX_NEXT_TRACK_ID", QByteArray::number(d->nextId));

        // What each track is, as a tag of the track
        KaxTag *previousTag = &tag;
        for (const Track &track: d->tracks) {
            KaxTag &trackTag = GetNextChild<KaxTag>(tags, *previousTag);
            previousTag = &trackTag;
            KaxTagTargets &targets = GetChild<KaxTagTargets>(trackTag);
            GetChild<KaxTagTrackUID>(targets).SetValue(track.uid);
            KaxTagSimple *trackSimple = nullptr;
            setSimpleTag(trackTag, trackSimple, "LDX_TRACK", trackDescription(track));
        }
        tags.Render(*d->output, false);

        // The files kept in the file
        if (!d->attachments.empty()) {
            KaxAttachments &attachments = GetChild<KaxAttachments>(*d->segment);
            KaxAttached *previousFile = nullptr;
            for (const Private::Attached &file: d->attachments) {
                KaxAttached &attached = previousFile ? GetNextChild<KaxAttached>(attachments, *previousFile) : GetChild<KaxAttached>(attachments);
                previousFile = &attached;
                GetChild<KaxFileName>(attached).SetValueUTF8(file.name.toUtf8().constData());
                GetChild<KaxMimeType>(attached).SetValue(file.mediaType.toLatin1().constData());
                GetChild<KaxFileUID>(attached).SetValue(QRandomGenerator::global()->generate64() | 1);
                GetChild<KaxFileData>(attached).CopyBuffer(reinterpret_cast<const binary *>(file.data.constData()), file.data.size());
            }
            attachments.Render(*d->output, false);
        }

        d->cues.SetGlobalTimecodeScale(TimecodeScaleNs);
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not write %1: %2").arg(fileName, QString::fromLocal8Bit(exception.what()));
        return false;
    }
    return true;
}

void Writer::addHistory(const QJsonObject &entry)
{
    d->history.append(entry);
}

bool Writer::openAppend(const QString &fileName, QString &error)
{
    try {
        // What the file holds now
        Reader reader;
        if (!reader.open(fileName, error)) return false;
        if (reader.profile() < 1) {
            error = QObject::tr("%1 is not an LDX file").arg(fileName);
            return false;
        }
        if (!reader.buildIndex(error)) return false;
        for (const Track &track: reader.tracks()) d->tracks.append(track);
        if (!d->prepareCodecs(error)) return false;
        d->history = reader.history();
        d->nextId = reader.nextTrackId();
        for (const Reader::BlockRef &ref: reader.blockRefs()) {
            d->cueRecords.push_back({ ref.timeNs, ref.track, ref.clusterPosition, ref.relativePosition });
        }
        const quint64 cuesAt = reader.cuesPosition();
        const quint64 segmentAt = reader.segmentPosition();
        if (cuesAt == 0 || segmentAt == 0) {
            error = QObject::tr("%1 has no cues to add after").arg(fileName);
            return false;
        }

        d->path = fileName;
        d->appending = true;
        d->output.reset(new StdIOCallback(fileName.toLocal8Bit().constData(), MODE_SAFE));

        // The segment as it is in the file (its head is written again, the same, to be known by the clusters)
        d->segment.reset(new KaxSegment);
        d->output->setFilePointer(static_cast<int64>(segmentAt));
        d->segment->WriteHead(*d->output, 8);
        d->trackList = &GetChild<KaxTracks>(*d->segment);
        d->buildEntries();

        // What is added goes where the cues were
        d->output->setFilePointer(static_cast<int64>(cuesAt));
        d->cues.SetGlobalTimecodeScale(TimecodeScaleNs);
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not add to %1: %2").arg(fileName, QString::fromLocal8Bit(exception.what()));
        return false;
    }
    return true;
}

bool Writer::writeBlock(quint32 track, qint64 timeNs, const QByteArray &data)
{
    if (!d->output || !d->entries.count(track) || timeNs < 0) return false;
    try {
        // The frames of the block: itself, or, for FLAC, what the encoder gives
        std::vector<QByteArray> frames;
        const Track *info = d->trackInfo(track);
        if (info && info->kind == Kind::Cvbs) {
            // A block of a cvbs track is one frame, whatever the codec
            const CvbsShape frameShape = cvbsShape(*info);
            if (frameShape.ok() && data.size() != frameShape.samples * 2) {
                d->error = QObject::tr("A frame of this cvbs track is %1 samples").arg(frameShape.samples);
                return false;
            }
        }
        if (info && info->codec == Codec::Flac) {
            QString encodeError;
            if (!flacEncode(data, Private::channelsOf(*info), info->kind == Kind::Tbc, frames, encodeError)) {
                d->error = encodeError;
                return false;
            }
        } else if (info && info->codec == Codec::Ffv1) {
            // A frame of CVBS: one packet
            const CvbsShape shape = cvbsShape(*info);
            if (data.size() != shape.samples * 2) {
                d->error = QObject::tr("A frame of this cvbs track is %1 samples").arg(shape.samples);
                return false;
            }
            QByteArray packet;
            QString encodeError;
            if (!d->ffv1Encoders[track]->encode(ffv1Pad(data, shape.width, shape.height, shape.pad), packet, encodeError)) {
                d->error = encodeError;
                return false;
            }
            frames.push_back(packet);
        } else {
            frames.push_back(data);
        }
        if (d->cluster && timeNs - d->clusterStartNs >= ClusterSpanNs) d->flushCluster();
        if (!d->cluster) {
            d->cluster = new KaxCluster;
            d->cluster->SetParent(*d->segment);
            d->cluster->SetPreviousTimecode(0, TimecodeScaleNs);
            d->clusterStartNs = timeNs;
        }

        // All the frames go in one block of the file (laced when there are several). The block owns the buffer objects from
        // here (it deletes them when the frames are released). What the call gives back says whether more frames can go in the
        // block, and is not a failure.
        KaxBlockGroup *first = nullptr;
        for (const QByteArray &frame: frames) {
            d->held.push_back(frame);
            QByteArray &kept = d->held.back();
            auto *buffer = new DataBuffer(reinterpret_cast<binary *>(kept.data()), static_cast<uint32>(kept.size()));
            KaxBlockGroup *block = nullptr;
            d->cluster->AddFrame(*d->entries[track], static_cast<uint64>(timeNs), *buffer, block, frames.size() > 1 ? LACING_EBML : LACING_NONE);
            if (!first) first = block;
        }
        d->clusterBlocks.push_back({ timeNs, track, first });
        return true;
    } catch (const std::exception &exception) {
        d->error = QString::fromLocal8Bit(exception.what());
        return false;
    }
}

QString Writer::lastError() const
{
    return d->error;
}

bool Writer::finish(QString &error)
{
    if (!d->output || d->finished) return true;
    try {
        if (d->streaming) {
            // Nothing more than the last cluster: a pipe has no cues, and its segment has no size to give
            d->flushCluster();
            d->output->close();
            d->finished = true;
            return true;
        }
        d->flushCluster();
        // A file that was added to: its history, with what was added, as tags at the end (the last ones are the current ones)
        if (d->appending) {
            KaxTags &tags = GetChild<KaxTags>(*d->segment);
            KaxTag &tag = GetChild<KaxTag>(tags);
            GetChild<KaxTagTargets>(tag);
            KaxTagSimple *simple = nullptr;
            setSimpleTag(tag, simple, "LDX_PROFILE", QByteArray::number(ProfileVersion));
            setSimpleTag(tag, simple, "LDX_HISTORY", QJsonDocument(d->history).toJson(QJsonDocument::Compact));
            setSimpleTag(tag, simple, "LDX_NEXT_TRACK_ID", QByteArray::number(d->nextId));
            tags.Render(*d->output, false);
        }
        // The cues, at the end: where the blocks of each track are
        if (!d->cueRecords.empty()) {
            KaxCues cues;
            cues.SetGlobalTimecodeScale(TimecodeScaleNs);
            for (const Private::CueRecord &record: d->cueRecords) {
                auto *point = static_cast<KaxCuePoint *>(cues.AddNewElt(EBML_INFO(KaxCuePoint)));
                GetChild<KaxCueTime>(*point).SetValue(static_cast<uint64>(record.timeNs / TimecodeScaleNs));
                KaxCueTrackPositions &positions = GetChild<KaxCueTrackPositions>(*point);
                GetChild<KaxCueTrack>(positions).SetValue(record.track);
                GetChild<KaxCueClusterPosition>(positions).SetValue(record.clusterPosition);
                GetChild<KaxCueRelativePosition>(positions).SetValue(record.relativePosition);
            }
            cues.Render(*d->output, false);
        }
        // The size of the segment is known now
        const uint64 end = d->output->getFilePointer();
        d->segment->ForceSize(end - d->segment->GetElementPosition() - d->segment->HeadSize());
        d->segment->OverwriteHead(*d->output);
        d->output->close();
        // The cues are longer than before: nothing of the old ones is left
        if (d->appending) QFile(d->path).resize(static_cast<qint64>(end));
        d->finished = true;
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not finish the file: %1").arg(QString::fromLocal8Bit(exception.what()));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Reader

struct Reader::Private
{
    std::unique_ptr<IOCallback> input;
    bool streaming = false;                    // read from a pipe: in order, with no index
    std::unique_ptr<EbmlStream> stream;
    EbmlElement *segmentElement = nullptr;
    int upperLevel = 0;
    EbmlElement *pending = nullptr;            // the next element of the segment, read from its header only

    int profileValue = 0;
    QVector<Track> trackList;
    QJsonArray historyValue;
    quint32 nextIdValue = 0;
    QVector<Attachment> attachmentList;
    std::map<quint64, QByteArray> descriptions;       // what each track is, by the uid of the track, from the tags
    std::map<quint32, QByteArray> flacPrivates;       // what the decoder of a FLAC track is given first
    std::map<quint32, std::unique_ptr<FlacDecoder>> decoders;
    std::map<quint32, QByteArray> ffv1Privates;
    std::map<quint32, std::unique_ptr<Ffv1Decoder>> ffv1Decoders;
    uint64 timecodeScale = 1000000;            // Matroska default
    std::deque<Block> queue;

    // The index of the blocks of each track, from the cues
    struct IndexEntry {
        qint64 timeNs;
        quint64 clusterPosition;        // from the start of the data of the segment
        quint64 relativePosition;       // of the block, from the start of the data of the cluster (NoPosition if not known)
    };
    static constexpr quint64 NoPosition = ~static_cast<quint64>(0);
    std::map<quint32, std::vector<IndexEntry>> index;
    bool indexed = false;
    quint64 cuesAt = 0;

    ~Private()
    {
        delete pending;
        delete segmentElement;
    }

    void readTracks(KaxTracks &tracks)
    {
        for (EbmlElement *element: tracks.GetElementList()) {
            if (EbmlId(*element) != EBML_ID(KaxTrackEntry)) continue;
            KaxTrackEntry &entry = *static_cast<KaxTrackEntry *>(element);
            Track track;
            if (auto *number = FindChild<KaxTrackNumber>(entry)) track.number = static_cast<quint32>(number->GetValue());
            if (auto *uid = FindChild<KaxTrackUID>(entry)) track.uid = uid->GetValue();
            if (auto *name = FindChild<KaxTrackName>(entry)) track.name = QString::fromUtf8(name->GetValueUTF8().c_str());
            if (auto *enabled = FindChild<KaxTrackFlagEnabled>(entry)) {
                if (enabled->GetValue() == 0) continue;     // a removed stream
            }
            QString codecId;
            if (auto *id = FindChild<KaxCodecID>(entry)) codecId = QString::fromLatin1(id->GetValue().c_str());
            if (codecId == QLatin1String("A_FLAC")) {
                // FLAC: the private data are the decoder's, and what the track is comes from its tag
                track.codec = Codec::Flac;
                if (auto *priv = FindChild<KaxCodecPrivate>(entry)) {
                    flacPrivates[track.number] = QByteArray(reinterpret_cast<const char *>(priv->GetBuffer()), static_cast<int>(priv->GetSize()));
                }
            } else if (codecId == QLatin1String("V_FFV1")) {
                track.codec = Codec::Ffv1;
                if (auto *priv = FindChild<KaxCodecPrivate>(entry)) {
                    ffv1Privates[track.number] = QByteArray(reinterpret_cast<const char *>(priv->GetBuffer()), static_cast<int>(priv->GetSize()));
                }
            } else if (auto *priv = FindChild<KaxCodecPrivate>(entry)) {
                const QJsonObject object = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(priv->GetBuffer()),
                                                                              static_cast<int>(priv->GetSize()))).object();
                track.kind = kindFromName(object["kind"].toString());
                track.codec = codecFromName(object["codec"].toString());
                track.depth = object["depth"].toInt(16);
                track.parameters = object["parameters"].toObject();
            }
            trackList.append(track);
        }
    }

    // The length in bytes of an EBML element id or data size, from its first byte (0 if it is not valid)
    static int vintLength(unsigned char first, int limit)
    {
        for (int length = 1; length <= limit; length++) {
            if (first & (0x80 >> (length - 1))) return length;
        }
        return 0;
    }

    // What each track is: from its tag when it has one (a track written before the tags was described in its private data)
    void applyDescriptions()
    {
        for (Track &track: trackList) {
            const auto found = descriptions.find(track.uid);
            if (found == descriptions.end()) continue;
            const QJsonObject object = QJsonDocument::fromJson(found->second).object();
            track.kind = kindFromName(object["kind"].toString());
            track.codec = codecFromName(object["codec"].toString());
            track.depth = object["depth"].toInt(16);
            track.parameters = object["parameters"].toObject();
        }
    }

    const Track *trackById(quint32 number) const
    {
        for (const Track &track: trackList) {
            if (track.number == number) return &track;
        }
        return nullptr;
    }

    // The data of a block from its frames: they are decoded for a track that has a codec
    bool assemble(quint32 track, const std::vector<QByteArray> &frames, QByteArray &out, QString &error)
    {
        const Track *info = trackById(track);
        if (info && info->codec == Codec::Flac) {
            std::unique_ptr<FlacDecoder> &decoder = decoders[track];
            if (!decoder) {
                decoder = std::make_unique<FlacDecoder>();
                if (!decoder->open(flacPrivates[track], error)) {
                    decoder.reset();
                    return false;
                }
            }
            return decoder->decode(frames, info->kind == Kind::Tbc, out, error);
        }
        if (info && info->codec == Codec::Ffv1) {
            const CvbsShape shape = cvbsShape(*info);
            if (!shape.ok() || frames.size() != 1) {
                error = QObject::tr("An FFV1 block is one frame, and the track says how big it is");
                return false;
            }
            std::unique_ptr<Ffv1Decoder> &decoder = ffv1Decoders[track];
            if (!decoder) {
                decoder = std::make_unique<Ffv1Decoder>();
                if (!decoder->open(shape.width, shape.height, ffv1Privates[track], error)) {
                    decoder.reset();
                    return false;
                }
            }
            QByteArray image;
            if (!decoder->decode(frames.front(), image, error)) return false;
            out = ffv1Unpad(image, shape.samples);
            return true;
        }
        out.clear();
        for (const QByteArray &frame: frames) out.append(frame);
        return true;
    }

    static quint64 vintValue(const unsigned char *bytes, int length)
    {
        quint64 value = bytes[0] & (0xFF >> length);
        for (int i = 1; i < length; i++) value = (value << 8) | bytes[i];
        return value;
    }

    // The frames of a block that is laced (0 none, 1 Xiph, 2 fixed size, 3 EBML)
    static bool splitLaces(const QByteArray &payload, int lacing, std::vector<QByteArray> &frames)
    {
        frames.clear();
        if (lacing == 0) {
            frames.push_back(payload);
            return true;
        }
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(payload.constData());
        const qint64 total = payload.size();
        if (total < 2) return false;
        const int count = bytes[0] + 1;
        qint64 position = 1;
        std::vector<qint64> sizes;
        if (lacing == 2) {
            for (int i = 0; i < count; i++) sizes.push_back((total - 1) / count);
        } else if (lacing == 1) {
            for (int i = 0; i < count - 1; i++) {
                qint64 size = 0;
                for (;;) {
                    if (position >= total) return false;
                    const int part = bytes[position++];
                    size += part;
                    if (part != 255) break;
                }
                sizes.push_back(size);
            }
        } else {
            qint64 previous = 0;
            for (int i = 0; i < count - 1; i++) {
                if (position >= total) return false;
                const int length = vintLength(bytes[position], 8);
                if (length == 0 || position + length > total) return false;
                qint64 value = static_cast<qint64>(vintValue(bytes + position, length));
                // the first size is as it is, the others are the difference from the one before
                if (i > 0) value = previous + (value - ((Q_INT64_C(1) << (7 * length - 1)) - 1));
                sizes.push_back(value);
                previous = value;
                position += length;
            }
        }
        if (lacing != 2) {
            qint64 used = 0;
            for (const qint64 size: sizes) used += size;
            sizes.push_back(total - position - used);
        }
        for (const qint64 size: sizes) {
            if (size < 0 || position + size > total) return false;
            frames.push_back(payload.mid(static_cast<int>(position), static_cast<int>(size)));
            position += size;
        }
        return true;
    }

    void readTags(KaxTags &tags)
    {
        for (EbmlElement *tagElement: tags.GetElementList()) {
            if (EbmlId(*tagElement) != EBML_ID(KaxTag)) continue;
            // The track a tag is about, if it is about one
            quint64 aboutTrack = 0;
            if (auto *targets = FindChild<KaxTagTargets>(*static_cast<KaxTag *>(tagElement))) {
                if (auto *uid = FindChild<KaxTagTrackUID>(*targets)) aboutTrack = uid->GetValue();
            }
            for (EbmlElement *child: static_cast<KaxTag *>(tagElement)->GetElementList()) {
                if (EbmlId(*child) != EBML_ID(KaxTagSimple)) continue;
                KaxTagSimple &simple = *static_cast<KaxTagSimple *>(child);
                auto *name = FindChild<KaxTagName>(simple);
                auto *value = FindChild<KaxTagString>(simple);
                if (!name || !value) continue;
                const QString key = QString::fromUtf8(name->GetValueUTF8().c_str());
                const QByteArray text = QByteArray(value->GetValueUTF8().c_str());
                if (aboutTrack != 0) {
                    if (key == QLatin1String("LDX_TRACK")) descriptions[aboutTrack] = text;
                    continue;
                }
                if (key == QLatin1String("LDX_PROFILE")) profileValue = text.toInt();
                if (key == QLatin1String("LDX_HISTORY")) historyValue = QJsonDocument::fromJson(text).array();
                if (key == QLatin1String("LDX_NEXT_TRACK_ID")) nextIdValue = static_cast<quint32>(text.toUInt());
            }
        }
    }

    void readAttachments(KaxAttachments &attachments)
    {
        for (EbmlElement *element: attachments.GetElementList()) {
            if (EbmlId(*element) != EBML_ID(KaxAttached)) continue;
            KaxAttached &attached = *static_cast<KaxAttached *>(element);
            Attachment file;
            if (auto *name = FindChild<KaxFileName>(attached)) file.name = QString::fromUtf8(name->GetValueUTF8().c_str());
            if (auto *type = FindChild<KaxMimeType>(attached)) file.mediaType = QString::fromLatin1(type->GetValue().c_str());
            if (auto *data = FindChild<KaxFileData>(attached)) file.data = QByteArray(reinterpret_cast<const char *>(data->GetBuffer()), static_cast<int>(data->GetSize()));
            attachmentList.append(file);
        }
    }

    // The next top-level element of the segment, header only. Null at the end.
    EbmlElement *nextTopLevel()
    {
        return stream->FindNextElement(segmentElement->Generic().Context, upperLevel, 0xFFFFFFFFL, true, 1);
    }

    // Read the cues, which are after the clusters: the top-level elements are passed over until they are found
    bool readCues(QString &error)
    {
        input->setFilePointer(static_cast<int64>(segmentElement->GetElementPosition() + segmentElement->HeadSize()));
        EbmlStream scan(*input);
        int upper = 0;
        bool found = false;
        EbmlElement *element = nullptr;
        while ((element = scan.FindNextElement(segmentElement->Generic().Context, upper, 0xFFFFFFFFL, true, 1)) != nullptr && upper <= 0) {
            std::unique_ptr<EbmlElement> owned(element);
            if (EbmlId(*element) == EBML_ID(KaxCues)) {
                cuesAt = element->GetElementPosition();
                int up = 0;
                EbmlElement *dummy = nullptr;
                element->Read(scan, EBML_CONTEXT(element), up, dummy, true);
                for (EbmlElement *child: static_cast<KaxCues *>(element)->GetElementList()) {
                    if (EbmlId(*child) != EBML_ID(KaxCuePoint)) continue;
                    auto &point = *static_cast<KaxCuePoint *>(child);
                    auto *time = FindChild<KaxCueTime>(point);
                    if (!time) continue;
                    for (EbmlElement *positions: point.GetElementList()) {
                        if (EbmlId(*positions) != EBML_ID(KaxCueTrackPositions)) continue;
                        auto &trackPositions = *static_cast<KaxCueTrackPositions *>(positions);
                        auto *track = FindChild<KaxCueTrack>(trackPositions);
                        auto *cluster = FindChild<KaxCueClusterPosition>(trackPositions);
                        auto *relative = FindChild<KaxCueRelativePosition>(trackPositions);
                        if (!track || !cluster) continue;
                        index[static_cast<quint32>(track->GetValue())].push_back({ static_cast<qint64>(time->GetValue() * timecodeScale), cluster->GetValue(),
                                                                          relative ? relative->GetValue() : NoPosition });
                    }
                }
                found = true;
            } else if (EbmlId(*element) == EBML_ID(KaxTags)) {
                // Tags that were added at the end of the file are the current ones (the history of what was done)
                int up = 0;
                EbmlElement *dummy = nullptr;
                element->Read(scan, EBML_CONTEXT(element), up, dummy, true);
                readTags(*static_cast<KaxTags *>(element));
            } else {
                element->SkipData(scan, EBML_CONTEXT(element));
            }
        }
        if (!found) {
            error = QObject::tr("The file has no cues, which are its index");
            return false;
        }
        // In the order of time, one entry for each block
        for (auto &entry: index) {
            std::stable_sort(entry.second.begin(), entry.second.end(), [](const IndexEntry &a, const IndexEntry &b) { return a.timeNs < b.timeNs; });
            entry.second.erase(std::unique(entry.second.begin(), entry.second.end(), [](const IndexEntry &a, const IndexEntry &b) {
                return a.timeNs == b.timeNs && a.clusterPosition == b.clusterPosition; }), entry.second.end());
        }
        return true;
    }

    // Just the frame of the block at a position in a cluster, read by hand: a BlockGroup, whose first child is
    // the Block (a track number, a time and some flags, then the data). Without the rest of the cluster.
    bool readBlockAt(quint64 clusterPosition, quint64 relativePosition, quint32 &track, std::vector<QByteArray> &frames)
    {
        input->setFilePointer(static_cast<int64>(segmentElement->GetElementPosition() + segmentElement->HeadSize() + clusterPosition));
        EbmlStream scan(*input);
        std::unique_ptr<EbmlElement> clusterElement(scan.FindNextID(KaxCluster::ClassInfos, 0xFFFFFFFFL));
        if (!clusterElement) return false;
        const quint64 groupPosition = clusterElement->GetElementPosition() + clusterElement->HeadSize() + relativePosition;

        const auto readAt = [this](quint64 position, unsigned char *buffer, size_t size) {
            input->setFilePointer(static_cast<int64>(position));
            return input->read(buffer, size) == size;
        };
        const auto readSize = [](const unsigned char *bytes, int length) {
            quint64 value = bytes[0] & (0xFF >> length);
            for (int i = 1; i < length; i++) value = (value << 8) | bytes[i];
            return value;
        };

        // The BlockGroup: its id (0xA0) and the size of its data
        unsigned char head[16];
        if (!readAt(groupPosition, head, sizeof(head)) || head[0] != 0xA0) return false;
        const int groupSizeLength = vintLength(head[1], 8);
        if (groupSizeLength == 0) return false;
        const quint64 groupSize = readSize(head + 1, groupSizeLength);
        quint64 child = groupPosition + 1 + groupSizeLength;
        const quint64 groupEnd = child + groupSize;

        // Its children, up to the Block (0xA1)
        while (child < groupEnd) {
            unsigned char childHead[16];
            if (!readAt(child, childHead, sizeof(childHead))) return false;
            const int idLength = vintLength(childHead[0], 4);
            if (idLength == 0) return false;
            const int sizeLength = vintLength(childHead[idLength], 8);
            if (sizeLength == 0) return false;
            const quint64 size = readSize(childHead + idLength, sizeLength);
            const quint64 dataStart = child + idLength + sizeLength;
            if (idLength == 1 && childHead[0] == 0xA1) {
                // The track number, then 2 bytes of time and 1 of flags (no lacing: one frame)
                unsigned char blockHead[16];
                if (!readAt(dataStart, blockHead, sizeof(blockHead))) return false;
                const int trackLength = vintLength(blockHead[0], 8);
                if (trackLength == 0) return false;
                const quint64 headerSize = static_cast<quint64>(trackLength) + 3;
                if (size < headerSize) return false;
                const int lacing = (blockHead[trackLength + 2] >> 1) & 3;
                track = static_cast<quint32>(readSize(blockHead, trackLength));
                QByteArray payload(static_cast<int>(size - headerSize), 0);
                input->setFilePointer(static_cast<int64>(dataStart + headerSize));
                if (input->read(payload.data(), static_cast<size_t>(payload.size())) != static_cast<size_t>(payload.size())) return false;
                return splitLaces(payload, lacing, frames);
            }
            child = dataStart + size;
        }
        return false;
    }

    // The blocks of the cluster at a position
    bool readClusterAt(quint64 position, std::deque<Block> &target)
    {
        input->setFilePointer(static_cast<int64>(segmentElement->GetElementPosition() + segmentElement->HeadSize() + position));
        EbmlStream scan(*input);
        EbmlElement *element = scan.FindNextID(KaxCluster::ClassInfos, 0xFFFFFFFFL);
        if (!element) return false;
        std::unique_ptr<EbmlElement> owned(element);
        int up = 0;
        EbmlElement *dummy = nullptr;
        element->Read(scan, EBML_CONTEXT(element), up, dummy, true);
        readCluster(*static_cast<KaxCluster *>(element), target);
        return true;
    }

    void readCluster(KaxCluster &cluster, std::deque<Block> &target)
    {
        cluster.SetParent(*static_cast<KaxSegment *>(segmentElement));
        auto *time = FindChild<KaxClusterTimecode>(cluster);
        const qint64 clusterNs = static_cast<qint64>((time ? time->GetValue() : 0) * timecodeScale);
        cluster.InitTimecode(time ? time->GetValue() : 0, static_cast<int64>(timecodeScale));

        for (EbmlElement *element: cluster.GetElementList()) {
            if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
                auto *block = FindChild<KaxBlock>(*static_cast<KaxBlockGroup *>(element));
                if (block) pushBlock(*block, cluster, clusterNs, target);
            } else if (EbmlId(*element) == EBML_ID(KaxSimpleBlock)) {
                pushBlock(*static_cast<KaxSimpleBlock *>(element), cluster, clusterNs, target);
            }
        }
    }

    template <class BlockType>
    void pushBlock(BlockType &block, KaxCluster &cluster, qint64 clusterNs, std::deque<Block> &target)
    {
        block.SetParent(cluster);
        const Track *info = trackById(static_cast<quint32>(block.TrackNum()));
        if (info && info->codec != Codec::Raw) {
            // All the frames of the block are one block of samples
            std::vector<QByteArray> frames;
            for (unsigned int i = 0; i < block.NumberFrames(); i++) {
                DataBuffer &frame = block.GetBuffer(i);
                frames.emplace_back(reinterpret_cast<const char *>(frame.Buffer()), static_cast<int>(frame.Size()));
            }
            Block out;
            out.track = block.TrackNum();
            out.timeNs = clusterNs + static_cast<qint64>(block.GetRelativeTimestamp()) * static_cast<qint64>(timecodeScale);
            QString decodeError;
            if (!assemble(out.track, frames, out.data, decodeError)) throw std::runtime_error(decodeError.toStdString());
            target.push_back(std::move(out));
            return;
        }
        for (unsigned int i = 0; i < block.NumberFrames(); i++) {
            DataBuffer &frame = block.GetBuffer(i);
            Block out;
            out.track = block.TrackNum();
            out.timeNs = clusterNs + static_cast<qint64>(block.GetRelativeTimestamp()) * static_cast<qint64>(timecodeScale);
            out.data = QByteArray(reinterpret_cast<const char *>(frame.Buffer()), static_cast<int>(frame.Size()));
            target.push_back(std::move(out));
        }
    }
};

Reader::Reader() : d(new Private) {}
Reader::~Reader() = default;

bool Reader::open(const QString &fileName, QString &error)
{
    return begin(fileName, error, nullptr);
}

bool Reader::openStream(FILE *file, QString &error)
{
    return begin(QObject::tr("the stream"), error, file);
}

bool Reader::begin(const QString &fileName, QString &error, FILE *stream)
{
    try {
        if (stream) {
            makeBinary(stream);
            d->input.reset(new StreamInput(stream));
            d->streaming = true;
        } else {
            d->input.reset(new StdIOCallback(fileName.toLocal8Bit().constData(), MODE_READ));
        }
        d->stream.reset(new EbmlStream(*d->input));

        // The EBML header
        EbmlElement *head = d->stream->FindNextID(EbmlHead::ClassInfos, 0xFFFFFFFFL);
        if (!head) {
            error = QObject::tr("%1 is not a Matroska file").arg(fileName);
            return false;
        }
        int upper = 0;
        EbmlElement *found = nullptr;
        head->Read(*d->stream, EBML_CONTEXT(head), upper, found, true);
        delete head;

        d->segmentElement = d->stream->FindNextID(KaxSegment::ClassInfos, 0xFFFFFFFFL);
        if (!d->segmentElement) {
            error = QObject::tr("%1 has no segment").arg(fileName);
            return false;
        }

        // Everything up to the first cluster: the info, the tracks and the tags
        while ((d->pending = d->nextTopLevel()) != nullptr && d->upperLevel <= 0) {
            EbmlElement *element = d->pending;
            if (EbmlId(*element) == EBML_ID(KaxCluster)) break;

            int up = 0;
            EbmlElement *dummy = nullptr;
            if (EbmlId(*element) == EBML_ID(KaxInfo)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                if (auto *scale = FindChild<KaxTimecodeScale>(*static_cast<KaxInfo *>(element))) d->timecodeScale = scale->GetValue();
            } else if (EbmlId(*element) == EBML_ID(KaxTracks)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readTracks(*static_cast<KaxTracks *>(element));
            } else if (EbmlId(*element) == EBML_ID(KaxTags)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readTags(*static_cast<KaxTags *>(element));
            } else if (EbmlId(*element) == EBML_ID(KaxAttachments)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readAttachments(*static_cast<KaxAttachments *>(element));
            } else {
                element->SkipData(*d->stream, EBML_CONTEXT(element));
            }
            delete element;
            d->pending = nullptr;
        }
        d->applyDescriptions();
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not read %1: %2").arg(fileName, QString::fromLocal8Bit(exception.what()));
        return false;
    }
    return true;
}

int Reader::profile() const { return d->profileValue; }
QVector<Track> Reader::tracks() const { return d->trackList; }
QJsonArray Reader::history() const { return d->historyValue; }
QVector<Reader::Attachment> Reader::attachments() const { return d->attachmentList; }
quint32 Reader::nextTrackId() const
{
    quint32 next = d->nextIdValue;
    for (const Track &track: d->trackList) next = qMax(next, track.number + 1);
    return next;
}

bool Reader::nextBlock(Block &block)
{
    try {
        while (d->queue.empty()) {
            EbmlElement *element = d->pending ? d->pending : d->nextTopLevel();
            d->pending = nullptr;
            if (!element || d->upperLevel > 0) {
                delete element;
                return false;
            }
            int up = 0;
            EbmlElement *dummy = nullptr;
            if (EbmlId(*element) == EBML_ID(KaxCluster)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readCluster(*static_cast<KaxCluster *>(element), d->queue);
            } else if (EbmlId(*element) == EBML_ID(KaxTags)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readTags(*static_cast<KaxTags *>(element));
            } else if (EbmlId(*element) == EBML_ID(KaxAttachments)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readAttachments(*static_cast<KaxAttachments *>(element));
            } else {
                element->SkipData(*d->stream, EBML_CONTEXT(element));
            }
            delete element;
        }
    } catch (const std::exception &) {
        return false;
    }
    block = std::move(d->queue.front());
    d->queue.pop_front();
    return true;
}

bool Reader::buildIndex(QString &error)
{
    if (d->indexed) return true;
    if (d->streaming) {
        error = QObject::tr("A stream has no index: its blocks are read in turn");
        return false;
    }
    try {
        if (!d->readCues(error)) return false;
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not read the index: %1").arg(QString::fromLocal8Bit(exception.what()));
        return false;
    }
    d->indexed = true;
    return true;
}

QVector<Reader::BlockRef> Reader::blockRefs() const
{
    QVector<BlockRef> refs;
    for (const auto &track: d->index) {
        for (const Private::IndexEntry &entry: track.second) {
            refs.append({ track.first, entry.timeNs, entry.clusterPosition, entry.relativePosition == Private::NoPosition ? 0 : entry.relativePosition });
        }
    }
    return refs;
}

quint64 Reader::cuesPosition() const
{
    return d->cuesAt;
}

quint64 Reader::segmentPosition() const
{
    return d->segmentElement ? static_cast<quint64>(d->segmentElement->GetElementPosition()) : 0;
}

qint64 Reader::lastTimeNs() const
{
    qint64 last = 0;
    for (const auto &track: d->index) {
        if (!track.second.empty()) last = qMax(last, track.second.back().timeNs);
    }
    return last;
}

qint64 Reader::blockCount(quint32 track) const
{
    const auto found = d->index.find(track);
    return found == d->index.end() ? 0 : static_cast<qint64>(found->second.size());
}

qint64 Reader::blockTimeNs(quint32 track, qint64 index) const
{
    const auto found = d->index.find(track);
    if (found == d->index.end() || index < 0 || index >= static_cast<qint64>(found->second.size())) return -1;
    return found->second[static_cast<size_t>(index)].timeNs;
}

bool Reader::readBlock(quint32 track, qint64 index, Block &block)
{
    const auto found = d->index.find(track);
    if (found == d->index.end() || index < 0 || index >= static_cast<qint64>(found->second.size())) return false;
    const Private::IndexEntry entry = found->second[static_cast<size_t>(index)];
    try {
        std::deque<Block> blocks;
        if (entry.relativePosition != Private::NoPosition) {
            quint32 found = 0;
            std::vector<QByteArray> frames;
            QString decodeError;
            Block direct;
            if (d->readBlockAt(entry.clusterPosition, entry.relativePosition, found, frames) && found == track
                && d->assemble(track, frames, direct.data, decodeError)) {
                direct.track = track;
                direct.timeNs = entry.timeNs;
                block = std::move(direct);
                return true;
            }
        }
        if (!d->readClusterAt(entry.clusterPosition, blocks)) return false;
        for (Block &candidate: blocks) {
            // (the time of a cue is in the ticks of the file, a little less exact than the time of the block)
            if (candidate.track == track && qAbs(candidate.timeNs - entry.timeNs) <= 2 * static_cast<qint64>(d->timecodeScale)) {
                block = std::move(candidate);
                return true;
            }
        }
    } catch (const std::exception &) {
    }
    return false;
}

bool Reader::attachment(const QString &name, QByteArray &data) const
{
    for (const Attachment &candidate: d->attachmentList) {
        if (candidate.name == name) {
            data = candidate.data;
            return true;
        }
    }
    return false;
}

} // namespace Ldx
