/******************************************************************************
 * ldxstream.cpp
 * ld-decode-tools TBC library - an LDX file coming in on the standard input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxstream.h"

#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QTemporaryFile>
#include <QThread>

#include <deque>
#include <mutex>
#include <QHash>

#ifdef LDDECODE_WITH_LDX
#include "ldx.h"
#endif

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#ifdef LDDECODE_WITH_LDX

namespace {

LdxStream *current = nullptr;

// The pipe the video goes through, inside the process: the read end is a FILE for the video reader
bool makePipe(int &readFd, int &writeFd)
{
    int fds[2];
#ifdef Q_OS_WIN
    if (_pipe(fds, 8 << 20, _O_BINARY) != 0) return false;
#else
    if (pipe(fds) != 0) return false;
    signal(SIGPIPE, SIG_IGN);       // a reader that has gone is seen as a failed write
#endif
    readFd = fds[0];
    writeFd = fds[1];
    return true;
}

bool writeAll(int fd, const QByteArray &data)
{
    qint64 done = 0;
    while (done < data.size()) {
#ifdef Q_OS_WIN
        const int count = _write(fd, data.constData() + done, static_cast<unsigned int>(qMin<qint64>(data.size() - done, 1 << 20)));
#else
        const ssize_t count = ::write(fd, data.constData() + done, static_cast<size_t>(data.size() - done));
#endif
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

} // namespace

struct LdxStream::Private : public QThread
{
    Ldx::Reader reader;
    Ldx::Track video;
    bool haveVideo = false;
    quint32 metadataTrack = 0;
    QByteArray metadata;
    std::deque<Ldx::Block> pending;            // video that came before the metadata
    std::mutex fieldLock;
    QHash<qint32, QByteArray> fieldJson;         // metadata of the fields that came (kept: several readers ask)
    FILE *read = nullptr;
    int writeFd = -1;
    bool started = false;

    ~Private() override
    {
        // The reader of the video may have stopped: close what it reads from, so the thread's writes fail and it ends
        if (read) fclose(read);
        read = nullptr;
        if (isRunning()) wait(5000);
        if (writeFd >= 0) {
#ifdef Q_OS_WIN
            _close(writeFd);
#else
            ::close(writeFd);
#endif
        }
    }

    void run() override
    {
        // What came before the metadata, then the rest of the stream
        for (const Ldx::Block &block: pending) {
            if (!writeAll(writeFd, block.data)) return finishPipe();
        }
        pending.clear();
        Ldx::Block block;
        while (reader.nextBlock(block)) {
            if (block.track == metadataTrack) keepField(block.data);
            else if (block.track == video.number && !writeAll(writeFd, block.data)) break;
        }
        finishPipe();
    }

    // A block of the metadata track after the first: the metadata of one field, which comes before its video
    void keepField(const QByteArray &data)
    {
        if (data.contains("videoParameters")) return;      // (a whole set of metadata is not a field)
        LdDecodeMetaData::Field field;
        if (!LdDecodeMetaData::fieldFromJson(data, field)) return;
        std::lock_guard<std::mutex> lock(fieldLock);
        fieldJson.insert(field.seqNo, data);
    }

    // The end of the video: what reads the pipe sees it end
    void finishPipe()
    {
        if (writeFd >= 0) {
#ifdef Q_OS_WIN
            _close(writeFd);
#else
            ::close(writeFd);
#endif
            writeFd = -1;
        }
    }
};

LdxStream::LdxStream() : d(new Private) {}

LdxStream::~LdxStream()
{
    if (current == this) current = nullptr;
}

LdxStream *LdxStream::instance()
{
    return current;
}

bool LdxStream::openStdin(QString &error)
{
    if (current) return true;
    std::unique_ptr<LdxStream> stream(new LdxStream);
    Private &d = *stream->d;
    if (!d.reader.openStream(stdin, error)) {
        error = QObject::tr("The standard input is not an LDX stream (%1). For raw video, say what it is: --input-format tbc or cvbs, with --system or the metadata.").arg(error);
        return false;
    }
    if (d.reader.profile() < 1) {
        error = QObject::tr("The standard input is Matroska, but not an LDX stream");
        return false;
    }

    // The first TBC or CVBS track, and the metadata about it
    for (const Ldx::Track &track: d.reader.tracks()) {
        if (track.kind == Ldx::Kind::Tbc || track.kind == Ldx::Kind::Cvbs) {
            d.video = track;
            d.haveVideo = true;
            break;
        }
    }
    if (!d.haveVideo) {
        error = QObject::tr("The LDX stream has no video (a TBC or CVBS track)");
        return false;
    }
    for (const Ldx::Track &track: d.reader.tracks()) {
        if (track.kind == Ldx::Kind::Metadata && (track.parameters.value("track").toInt(0) == static_cast<int>(d.video.number)
                                                  || (!track.parameters.contains("track") && d.metadataTrack == 0))) {
            d.metadataTrack = track.number;
        }
    }

    // The metadata come first in the stream (a producer sends them before the video); video that comes before is kept
    Ldx::Block block;
    while (d.metadataTrack != 0 && d.metadata.isEmpty() && d.reader.nextBlock(block)) {
        if (block.track == d.metadataTrack) d.metadata = block.data;
        else if (block.track == d.video.number) d.pending.push_back(std::move(block));
        if (d.pending.size() > 64) break;       // (video is not held for long)
    }

    current = stream.get();
    stream.release();
    return true;
}

bool LdxStream::isCvbs() const { return d->video.kind == Ldx::Kind::Cvbs; }

VideoSystem LdxStream::system() const
{
    const QString name = d->video.parameters.value("system").toString();
    return name == "PAL" ? PAL : (name == "NTSC" ? NTSC : PAL_M);
}

QString LdxStream::videoName() const { return d->video.name; }
QByteArray LdxStream::metadataJson() const { return d->metadata; }

bool LdxStream::readMetadata(LdDecodeMetaData &metadata, QString &error) const
{
    if (d->metadata.isEmpty()) {
        error = QObject::tr("The LDX stream sent no metadata before its video");
        return false;
    }
    QTemporaryFile temporary;
    if (!temporary.open() || temporary.write(d->metadata) != d->metadata.size() || !temporary.flush()) {
        error = QObject::tr("Could not hold the metadata of the stream");
        return false;
    }
    if (!metadata.read(temporary.fileName())) {
        error = QObject::tr("The metadata of the stream could not be read");
        return false;
    }
    return true;
}

void LdxStream::provideFields(LdDecodeMetaData &metadata)
{
    Private *p = d.get();
    metadata.setFieldProvider([p](qint32 seqNo, LdDecodeMetaData::Field &field) {
        QByteArray json;
        {
            std::lock_guard<std::mutex> lock(p->fieldLock);
            const auto it = p->fieldJson.constFind(seqNo);
            if (it == p->fieldJson.constEnd()) return false;
            json = it.value();
        }
        return LdDecodeMetaData::fieldFromJson(json, field);
    });
}

FILE *LdxStream::videoPipe()
{
    if (!d->started) {
        int readFd = -1;
        if (!makePipe(readFd, d->writeFd)) return nullptr;
        d->read = fdopen(readFd, "rb");
        d->started = true;
        d->start();
    }
    return d->read;
}

#else // no LDX

struct LdxStream::Private {};
LdxStream::LdxStream() : d(new Private) {}
LdxStream::~LdxStream() = default;
LdxStream *LdxStream::instance() { return nullptr; }
bool LdxStream::openStdin(QString &error) { error = QObject::tr("This program was built without support for LDX files"); return false; }
bool LdxStream::isCvbs() const { return false; }
VideoSystem LdxStream::system() const { return NTSC; }
QString LdxStream::videoName() const { return QString(); }
QByteArray LdxStream::metadataJson() const { return QByteArray(); }
bool LdxStream::readMetadata(LdDecodeMetaData &, QString &error) const { error = QObject::tr("No LDX support"); return false; }
FILE *LdxStream::videoPipe() { return nullptr; }
void LdxStream::provideFields(LdDecodeMetaData &) {}

#endif
