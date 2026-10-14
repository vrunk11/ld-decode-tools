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

#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <algorithm>
#include <deque>
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

    std::unique_ptr<StdIOCallback> output;
    std::unique_ptr<KaxSegment> segment;
    KaxTracks *trackList = nullptr;
    std::map<quint32, KaxTrackEntry *> entries;            // by the id (number) of the track
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
            GetChild<KaxTrackFlagLacing>(entry).SetValue(0);
            if (!track.name.isEmpty()) GetChild<KaxTrackName>(entry).SetValueUTF8(track.name.toUtf8().constData());

            if (isSampleKind(track.kind)) {
                GetChild<KaxTrackType>(entry).SetValue(track_audio);
                GetChild<KaxCodecID>(entry).SetValue(PcmCodecId);
                KaxTrackAudio &audio = GetChild<KaxTrackAudio>(entry);
                GetChild<KaxAudioSamplingFreq>(audio).SetValue(StandInSampleRate);
                GetChild<KaxAudioChannels>(audio).SetValue(1);
                GetChild<KaxAudioBitDepth>(audio).SetValue(16);
            } else {
                GetChild<KaxTrackType>(entry).SetValue(33);     // metadata: the codec says what is in it
                GetChild<KaxCodecID>(entry).SetValue(QByteArray("D_LDX/" + kindName(track.kind).toUpper().toLatin1()).constData());
            }
            const QByteArray description = trackDescription(track);
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
    try {
        for (const Track &track: d->tracks) {
            if (track.codec != Codec::Raw) {
                error = QObject::tr("The %1 codec is not written by this build yet").arg(codecName(track.codec));
                return false;
            }
        }

        d->output.reset(new StdIOCallback(fileName.toLocal8Bit().constData(), MODE_CREATE));

        EbmlHead head;
        GetChild<EDocType>(head).SetValue("matroska");
        GetChild<EDocTypeVersion>(head).SetValue(4);
        GetChild<EDocTypeReadVersion>(head).SetValue(2);
        head.Render(*d->output, true);

        d->segment.reset(new KaxSegment);
        d->segment->WriteHead(*d->output, 8);

        KaxInfo &info = GetChild<KaxInfo>(*d->segment);
        GetChild<KaxTimecodeScale>(info).SetValue(TimecodeScaleNs);
        GetChild<KaxMuxingApp>(info).SetValueUTF8("ld-decode-tools ldx");
        GetChild<KaxWritingApp>(info).SetValueUTF8("ld-decode-tools ldx");
        info.Render(*d->output, false);

        d->trackList = &GetChild<KaxTracks>(*d->segment);
        d->buildEntries();
        d->trackList->Render(*d->output, false);

        // Space for tracks to be added to the file later
        EbmlVoid padding;
        padding.SetSize(TrackPaddingBytes);
        padding.Render(*d->output);

        // The profile and the history, before the data so that a reader (or a pipe) has them first
        KaxTags &tags = GetChild<KaxTags>(*d->segment);
        KaxTag &tag = GetChild<KaxTag>(tags);
        GetChild<KaxTagTargets>(tag);
        KaxTagSimple *simple = nullptr;
        setSimpleTag(tag, simple, "LDX_PROFILE", QByteArray::number(ProfileVersion));
        setSimpleTag(tag, simple, "LDX_HISTORY", QJsonDocument(d->history).toJson(QJsonDocument::Compact));
        setSimpleTag(tag, simple, "LDX_NEXT_TRACK_ID", QByteArray::number(d->nextId));
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
        for (const Track &track: reader.tracks()) {
            if (track.codec != Codec::Raw) {
                error = QObject::tr("The %1 codec is not written by this build yet").arg(codecName(track.codec));
                return false;
            }
            d->tracks.append(track);
        }
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
        if (d->cluster && timeNs - d->clusterStartNs >= ClusterSpanNs) d->flushCluster();
        if (!d->cluster) {
            d->cluster = new KaxCluster;
            d->cluster->SetParent(*d->segment);
            d->cluster->SetPreviousTimecode(0, TimecodeScaleNs);
            d->clusterStartNs = timeNs;
        }

        d->held.push_back(data);
        QByteArray &kept = d->held.back();
        // The block owns the buffer object from here (it deletes it when the frames are released)
        auto *buffer = new DataBuffer(reinterpret_cast<binary *>(kept.data()), static_cast<uint32>(kept.size()));
        KaxBlockGroup *block = nullptr;
        // Without lacing the block takes the one frame and says that it is full (false), which is not a failure
        d->cluster->AddFrame(*d->entries[track], static_cast<uint64>(timeNs), *buffer, block, LACING_NONE);
        d->clusterBlocks.push_back({ timeNs, track, block });
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
    std::unique_ptr<StdIOCallback> input;
    std::unique_ptr<EbmlStream> stream;
    EbmlElement *segmentElement = nullptr;
    int upperLevel = 0;
    EbmlElement *pending = nullptr;            // the next element of the segment, read from its header only

    int profileValue = 0;
    QVector<Track> trackList;
    QJsonArray historyValue;
    quint32 nextIdValue = 0;
    QVector<Attachment> attachmentList;
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
            if (auto *priv = FindChild<KaxCodecPrivate>(entry)) {
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

    void readTags(KaxTags &tags)
    {
        for (EbmlElement *tagElement: tags.GetElementList()) {
            if (EbmlId(*tagElement) != EBML_ID(KaxTag)) continue;
            for (EbmlElement *child: static_cast<KaxTag *>(tagElement)->GetElementList()) {
                if (EbmlId(*child) != EBML_ID(KaxTagSimple)) continue;
                KaxTagSimple &simple = *static_cast<KaxTagSimple *>(child);
                auto *name = FindChild<KaxTagName>(simple);
                auto *value = FindChild<KaxTagString>(simple);
                if (!name || !value) continue;
                const QString key = QString::fromUtf8(name->GetValueUTF8().c_str());
                const QByteArray text = QByteArray(value->GetValueUTF8().c_str());
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

    // The length in bytes of an EBML element id or data size, from its first byte (0 if it is not valid)
    static int vintLength(unsigned char first, int limit)
    {
        for (int length = 1; length <= limit; length++) {
            if (first & (0x80 >> (length - 1))) return length;
        }
        return 0;
    }

    // Just the frame of the block at a position in a cluster, read by hand: a BlockGroup, whose first child is
    // the Block (a track number, a time and some flags, then the data). Without the rest of the cluster.
    bool readBlockAt(quint64 clusterPosition, quint64 relativePosition, Block &out)
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
                if (trackLength == 0 || (blockHead[trackLength + 2] & 0x06) != 0) return false;
                const quint64 track = readSize(blockHead, trackLength);
                const quint64 headerSize = static_cast<quint64>(trackLength) + 3;
                if (size < headerSize) return false;
                out.track = static_cast<quint32>(track);
                out.data.resize(static_cast<int>(size - headerSize));
                input->setFilePointer(static_cast<int64>(dataStart + headerSize));
                return input->read(out.data.data(), static_cast<size_t>(out.data.size())) == static_cast<size_t>(out.data.size());
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
    try {
        d->input.reset(new StdIOCallback(fileName.toLocal8Bit().constData(), MODE_READ));
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
            Block direct;
            if (d->readBlockAt(entry.clusterPosition, entry.relativePosition, direct) && direct.track == track) {
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
