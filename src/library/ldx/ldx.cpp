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

#include <QJsonDocument>
#include <QRandomGenerator>

#include <deque>
#include <vector>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
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
    std::vector<KaxTrackEntry *> entries;
    KaxCues cues;

    KaxCluster *cluster = nullptr;
    qint64 clusterStartNs = 0;
    std::vector<QByteArray> held;                          // the data of the blocks of the cluster, until it is written
    bool finished = false;
    QString error;

    void flushCluster()
    {
        if (!cluster) return;
        cluster->Render(*output, cues);
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
    // The ids are the numbers of the tracks, from 1, never reused in a file
    track.number = d->tracks.size() + 1;
    if (track.uid == 0) track.uid = QRandomGenerator::global()->generate64() | 1;
    d->tracks.append(track);
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
        KaxTrackEntry *previous = nullptr;
        for (const Track &track: d->tracks) {
            KaxTrackEntry &entry = previous ? GetNextChild<KaxTrackEntry>(*d->trackList, *previous) : GetChild<KaxTrackEntry>(*d->trackList);
            previous = &entry;
            d->entries.push_back(&entry);

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
        tags.Render(*d->output, false);

        d->cues.SetGlobalTimecodeScale(TimecodeScaleNs);
    } catch (const std::exception &exception) {
        error = QObject::tr("Could not write %1: %2").arg(fileName, QString::fromLocal8Bit(exception.what()));
        return false;
    }
    return true;
}

bool Writer::writeBlock(quint32 track, qint64 timeNs, const QByteArray &data)
{
    if (!d->output || track < 1 || track > d->entries.size() || timeNs < 0) return false;
    try {
        if (d->cluster && timeNs - d->clusterStartNs >= ClusterSpanNs) d->flushCluster();
        if (!d->cluster) {
            d->cluster = new KaxCluster;
            d->cluster->SetParent(*d->segment);
            d->cluster->SetPreviousTimecode(0, TimecodeScaleNs);
            d->cluster->EnableChecksum();
            d->clusterStartNs = timeNs;
        }

        d->held.push_back(data);
        QByteArray &kept = d->held.back();
        // The block owns the buffer object from here (it deletes it when the frames are released)
        auto *buffer = new DataBuffer(reinterpret_cast<binary *>(kept.data()), static_cast<uint32>(kept.size()));
        KaxBlockGroup *block = nullptr;
        // Without lacing the block takes the one frame and says that it is full (false), which is not a failure
        d->cluster->AddFrame(*d->entries[track - 1], static_cast<uint64>(timeNs), *buffer, block, LACING_NONE);
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
        // The size of the segment is known now
        const uint64 end = d->output->getFilePointer();
        d->segment->ForceSize(end - d->segment->GetElementPosition() - d->segment->HeadSize());
        d->segment->OverwriteHead(*d->output);
        d->output->close();
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
    uint64 timecodeScale = 1000000;            // Matroska default
    std::deque<Block> queue;

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
            }
        }
    }

    // The next top-level element of the segment, header only. Null at the end.
    EbmlElement *nextTopLevel()
    {
        return stream->FindNextElement(segmentElement->Generic().Context, upperLevel, 0xFFFFFFFFL, true, 1);
    }

    void readCluster(KaxCluster &cluster)
    {
        cluster.SetParent(*static_cast<KaxSegment *>(segmentElement));
        auto *time = FindChild<KaxClusterTimecode>(cluster);
        const qint64 clusterNs = static_cast<qint64>((time ? time->GetValue() : 0) * timecodeScale);
        cluster.InitTimecode(time ? time->GetValue() : 0, static_cast<int64>(timecodeScale));

        for (EbmlElement *element: cluster.GetElementList()) {
            if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
                auto *block = FindChild<KaxBlock>(*static_cast<KaxBlockGroup *>(element));
                if (block) pushBlock(*block, cluster, clusterNs);
            } else if (EbmlId(*element) == EBML_ID(KaxSimpleBlock)) {
                pushBlock(*static_cast<KaxSimpleBlock *>(element), cluster, clusterNs);
            }
        }
    }

    template <class BlockType>
    void pushBlock(BlockType &block, KaxCluster &cluster, qint64 clusterNs)
    {
        block.SetParent(cluster);
        for (unsigned int i = 0; i < block.NumberFrames(); i++) {
            DataBuffer &frame = block.GetBuffer(i);
            Block out;
            out.track = block.TrackNum();
            out.timeNs = clusterNs + static_cast<qint64>(block.GetRelativeTimestamp()) * static_cast<qint64>(timecodeScale);
            out.data = QByteArray(reinterpret_cast<const char *>(frame.Buffer()), static_cast<int>(frame.Size()));
            queue.push_back(std::move(out));
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
                d->readCluster(*static_cast<KaxCluster *>(element));
            } else if (EbmlId(*element) == EBML_ID(KaxTags)) {
                element->Read(*d->stream, EBML_CONTEXT(element), up, dummy, true);
                d->readTags(*static_cast<KaxTags *>(element));
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

} // namespace Ldx
