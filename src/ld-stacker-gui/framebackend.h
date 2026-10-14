/******************************************************************************
 * framebackend.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FRAMEBACKEND_H
#define FRAMEBACKEND_H

#include <QImage>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

#include "lddecodemetadata.h"
#include "comb.h"
#include "outputwriter.h"
#include "palcolour.h"
#include "sourcefield.h"
#include "sourcevideo.h"
#include "stackproject.h"

// What the interface is told about a source once it is loaded
struct SourceInfo {
    QString id;
    bool ok = false;
    QString error;
    QString file;
    QString metadataFile;
    qint32 frames = 0;
    VideoSystem system = NTSC;
    qint32 fieldWidth = 0;
    qint32 fieldHeight = 0;
    // Per frame (index 0 is frame 1): the SNR of the metadata, NaN where there is none
    QVector<float> whiteSnr;
    QVector<float> blackSnr;
};

// A frame to show: the stack of the sources, or one of them
struct FrameRequest {
    struct Entry {
        QString id;
        bool enabled = true;
        qint32 offset = 0;
    };

    quint64 serial = 0;
    qint32 channel = 0;           // 0 the main picture, 1 the stack output window; each has its own newest request
    qint32 frame = 1;
    QString view = "stack";       // "stack" or the id of a source
    bool chroma = false;          // the picture is chroma decoded
    QVector<Entry> entries;       // in the order of the list
    StackSettings settings;
};

struct FrameResult {
    quint64 serial = 0;
    qint32 channel = 0;
    qint32 frame = 1;
    QString view;
    QImage image;
    QString message;              // what to tell the user when there is no image
    qint32 contributing = 0;      // sources that gave a frame to the stack
    qint32 inStack = 0;           // sources that are on
    qint32 modeUsed = -1;
    double stackWhiteSnr = 0;     // of the stacked frame, NaN if it was not measured
    double stackBlackSnr = 0;
};

Q_DECLARE_METATYPE(PalColour::Configuration)
Q_DECLARE_METATYPE(Comb::Configuration)
Q_DECLARE_METATYPE(SourceInfo)
Q_DECLARE_METATYPE(FrameRequest)
Q_DECLARE_METATYPE(FrameResult)

// Reads the sources and works out the frames, in a thread of its own: the interface only
// sends requests and gets images back. Only the newest request is worked on.
class FrameBackend : public QObject
{
    Q_OBJECT

public:
    explicit FrameBackend(QObject *parent = nullptr);

    // The serial of the newest request, set by the sender before it posts it, so that requests
    // that have been overtaken are skipped
    std::atomic<quint64> latest[2] = { 0, 0 };

public slots:
    void addSource(const QString &id, const QString &file, bool reverseFields);
    void removeSource(const QString &id);
    void setReverseFields(bool reverseFields);
    void setChromaConfiguration(const PalColour::Configuration &pal, const Comb::Configuration &ntsc);
    void render(const FrameRequest &request);

signals:
    void sourceLoaded(const SourceInfo &info);
    void frameReady(const FrameResult &result);

private:
    struct Loaded {
        LdDecodeMetaData metadata;
        SourceVideo video;
        bool ok = false;
    };

    // Frame number of a source for a frame of the stack, or -1 if the source has none there
    qint32 sourceFrame(Loaded &source, qint32 frame, qint32 offset);
    bool isPadded(Loaded &source, qint32 ownFrame);
    static QImage weave(const SourceVideo::Data &first, const SourceVideo::Data &second, qint32 width, qint32 fieldHeight);
    // The picture of a frame decoded to colour. For the stack, the two fields stacked are given and
    // replace those of the source the frame is read from.
    struct StackedFrame {
        bool ok = false;
        QVector<qint32> usable;       // the entries that gave a frame
        qint32 ownFrame = -1;         // the frame, of the first of them, that this is the stack of
        QString sourceId;             // that first one
        SourceVideo::Data first, second;
        LdDecodeMetaData::VideoParameters videoParameters;
        qint32 mode = 0;
    };
    // Stack a frame of the timeline from the sources that have it; false if none does
    bool stackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, StackedFrame &out);
    QImage chromaPicture(Loaded &source, qint32 ownFrame, const QVector<StackedFrame> *around, qint32 lookBehind);
    void lookRange(VideoSystem system, qint32 &lookBehind, qint32 &lookAhead) const;
    void configureDecoders(LdDecodeMetaData::VideoParameters videoParameters);

    QMap<QString, std::shared_ptr<Loaded>> sources;

    // The chroma decoders, set up for the video parameters of the last picture
    PalColour palColour;
    Comb ntscColour;
    OutputWriter outputWriter;
    PalColour::Configuration palConfiguration;
    Comb::Configuration ntscConfiguration;
    OutputWriter::Configuration outputConfiguration;
    QString decoderKey;
};

#endif // FRAMEBACKEND_H
