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
#include <cmath>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <QJsonArray>
#include <memory>

#include "lddecodemetadata.h"
#include "discnumbering.h"
#include "dropouts.h"
#include "colourconfiguration.h"
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
    bool widescreen = false;
    bool cvbs = false;            // a CVBS capture, not a TBC one
    qint32 fieldWidth = 0;
    qint32 fieldHeight = 0;
    // Per frame (index 0 is frame 1): the SNR of the metadata, NaN where there is none
    QVector<float> whiteSnr;
    QVector<float> blackSnr;
    // Per frame: the total length of the dropouts of the metadata (both fields, in samples), NaN where the frame is padded or marked as a dropout as a whole
    QVector<float> dropoutLength;
    // What the information panel of the source shows
    QString metadataFormat;       // "SQLite", "JSON" or "CVBS"
    bool isMapped = false;        // the metadata say the source was mapped (ld-discmap)
    double sampleRate = 0;
    double fsc = 0;
    qint32 blackIre = -1, whiteIre = -1;
    QString tapeFormat;
    QString gitCommit;
    qint32 fields = 0;
    qint32 paddedFrames = 0;      // frames that have a field that is not there (padded)
    qint32 vbiFrames = 0;         // frames that have a picture number or a time code
    qint32 discType = 0;          // 0 not known, 1 CLV, 2 CAV
    qint32 originDisc = DiscNumbering::NoNumber;       // the number of the disc (picture, or time in frames) of the first frame of the capture, which can be 0 or negative: NoNumber if the VBI gives none
    qint32 firstPicture = -1, lastPicture = -1;
    qint32 firstSeconds = -1, lastSeconds = -1;
    qint32 chapters = 0;
    qint32 jumps = 0;             // where the numbering of the pictures (or the times) does not go on by one
    qint32 duplicates = 0;        // a picture number that comes twice in a row
    QJsonArray history;           // of an LDX file
};

// A frame to show: the stack of the sources, or one of them
struct FrameRequest {
    struct Entry {
        QString id;
        bool enabled = true;
        qint32 offset = 0;
        bool showDropouts = false;    // the dropouts of the capture are drawn on the picture, in this colour (a field in a lighter shade than the other)
        QRgb color = 0;
    };

    quint64 serial = 0;
    qint32 channel = 0;           // 0 the main picture, 1 the stack output window; each has its own newest request
    qint32 frame = 1;
    QString view = "stack";       // "stack" or the id of a source
    bool chroma = false;          // the picture is chroma decoded
    qint32 viewMode = 0;          // 0 the frame, 1 one field with its lines doubled, 2 both fields, one above the other
    qint32 fieldIndex = 0;        // in mode 1, which field: 0 the first of the frame, 1 the second
    QVector<Entry> entries;       // in the order of the list
    StackSettings settings;
    bool residualDropouts = false;   // in the stack, the dropouts that the stacking left are drawn too
    QRgb residualColor = 0;
    bool measureResidual = false;    // the measure of the stack reads whole fields and measures the dropouts the stack is left with, as well as its SNR
    qint32 sampleStep = 1;           // the measure of the stack takes one frame in this many (1: all of them; the rough measure: one in five seconds of video)
};

// The SNR of the stack at a frame, measured from the test signal lines of the stacked fields (NaN: none)
struct StackSnrPoint {
    qint32 frame = 0;
    float white = 0;
    float black = 0;
    float residual = 0;     // the total length of the dropouts the stack is left with (NaN: not measured, or the frame is marked as a dropout as a whole)
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
    double stackResidual = std::nan("");    // the total length of the dropouts the stack is left with in this frame (NaN: none, or the frame is marked as a dropout as a whole)
    double sourceWhiteSnr = std::nan("");   // the SNR of the frame of a source that is shown (NaN: none, or it is the stack)
    double sourceBlackSnr = std::nan("");
};

Q_DECLARE_METATYPE(PalColour::Configuration)
Q_DECLARE_METATYPE(Comb::Configuration)
Q_DECLARE_METATYPE(StackSnrPoint)
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
    // When the person last asked for a picture (ms since the epoch): the measures in the background wait while the person navigates, so that the pictures are not kept waiting
    std::atomic<qint64> lastInteraction{0};
    // The generation of the stack SNR job that is wanted: a newer one makes the older stop
    std::atomic<quint64> snrGeneration{0};
    // The same for the SNR of a source that is measured from its fields (one job at a time)
    std::atomic<quint64> sourceSnrGeneration{0};

public slots:
    void addSource(const QString &id, const QString &file, bool reverseFields);
    void removeSource(const QString &id);
    void setReverseFields(bool reverseFields);
    void setChromaConfiguration(const PalColour::Configuration &pal, const Comb::Configuration &ntsc);
    void render(const FrameRequest &request);
    // Measure the SNR of the stack over the whole timeline, a chunk of frames at a time so that pictures asked
    // for in between are not kept waiting
    void computeStackSnr(const FrameRequest &request, qint32 startFrame, quint64 generation);
    // Measure the SNR of one source over its own frames, from the test signal lines of its fields (instead of reading it from its metadata), in the same way
    void computeSourceSnr(const QString &id, qint32 startFrame, quint64 generation, qint32 step);

signals:
    void sourceLoaded(const SourceInfo &info);
    void frameReady(const FrameResult &result);
    void stackSnrReady(quint64 generation, const QVector<StackSnrPoint> &points, bool finished);
    void sourceSnrReady(const QString &id, quint64 generation, const QVector<StackSnrPoint> &points, bool finished);

private:
    struct Loaded {
        LdDecodeMetaData metadata;
        SourceVideo video;
        QString file;                 // what the video was opened from (a thread that measures the SNR opens it again)
        bool ok = false;
    };

    // Frame number of a source for a frame of the stack, or -1 if the source has none there
    qint32 sourceFrame(Loaded &source, qint32 frame, qint32 offset);
    bool isPadded(Loaded &source, qint32 ownFrame);
    // A picture of a frame as one of its fields (lines doubled), or as both of them, one above the other
    static QImage applyView(const QImage &frame, qint32 viewMode, qint32 fieldIndex);
    // Dropouts drawn on the picture of a frame (before the view is applied): the lines of the first field are the even rows, those of the second the odd ones
    static void drawDropouts(QImage &image, const DropOuts &dropOuts, bool secondField, const QColor &color);
    static bool isWholeDropout(const DropOuts &dropOuts, qint32 fieldWidth, qint32 fieldHeight);
    void drawSourceDropouts(QImage &image, Loaded &source, qint32 ownFrame, const QColor &color);
    // A black picture of the size of the frames, for where a source (or the stack) has no frame
    QImage blankPicture(const FrameRequest &request) const;
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
        DropOuts firstDropOuts, secondDropOuts;     // the dropouts the stack is left with
    };
    // Stack a frame of the timeline from the sources that have it; false if none does
    // lines > 0: only the first lines of the fields (the test signal lines), which is all the SNR needs
    // videos: the handles on the files of a thread that has its own (null: those of the sources)
    bool stackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, StackedFrame &out, qint32 lines = -1,
                    const QMap<QString, SourceVideo *> *videos = nullptr);
    StackSnrPoint measureStackFrame(const FrameRequest &request, qint32 frame, qint32 inStack, const QMap<QString, SourceVideo *> *videos);
    static qint32 snrThreadCount();
    void yieldToInteraction(const std::atomic<quint64> &generationNow, quint64 generation) const;
    template <typename Job>
    void runSnrJobs(qint32 threads, qint32 count, const std::atomic<quint64> &generationNow, quint64 generation, Job job) const;
    StackSnrPoint measureSourceFrame(Loaded &source, SourceVideo &video, qint32 frame);
    void openSnrReaders(const FrameRequest &request, qint32 threads, quint64 generation);
    QImage chromaPicture(Loaded &source, qint32 ownFrame, const QVector<StackedFrame> *around, qint32 lookBehind);
    void lookRange(VideoSystem system, qint32 &lookBehind, qint32 &lookAhead) const;
    void configureDecoders(LdDecodeMetaData::VideoParameters videoParameters);

    QMap<QString, std::shared_ptr<Loaded>> sources;
    // The handles on the files of the threads that measure the SNR of the stack (one map of source id to handle for each thread), for one job
    QVector<QMap<QString, std::shared_ptr<SourceVideo>>> snrReaders;
    quint64 snrReadersGeneration = 0;
    QVector<std::shared_ptr<SourceVideo>> sourceReaders;
    quint64 sourceReadersGeneration = 0;

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
