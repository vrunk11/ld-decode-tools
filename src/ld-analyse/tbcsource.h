/******************************************************************************
 * tbcsource.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2021-2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef TBCSOURCE_H
#define TBCSOURCE_H

#include <QMap>
#include <QSet>
#include <QObject>
#include <QImage>
#include <QPainter>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>

// TBC library includes
#include "sourcevideo.h"
#include "lddecodemetadata.h"
#include "linenumber.h"
#include "fieldvbi.h"
#include "filesnr.h"
#include "livesnr.h"
#include "vitsmetrics.h"
#include "vbidecoder.h"
#include "videoiddecoder.h"
#include "vitcdecoder.h"

// Chroma decoder includes
#include "palcolour.h"
#include "comb.h"
#include "colourconfiguration.h"
#include "monodecoder.h"

class TbcSource : public QObject
{
    Q_OBJECT
public:
    explicit TbcSource(QObject *parent = nullptr);

    struct ScanLineData {
        QString systemDescription;
        LineNumber lineNumber;
        QVector<qint32> composite;
        QVector<qint32> luma;
        QVector<qint32> chroma;
        QVector<bool> isDropout;
        qint32 blackIre;
        qint32 whiteIre;
        qint32 fieldWidth;
        qint32 colourBurstStart;
        qint32 colourBurstEnd;
        qint32 activeVideoStart;
        qint32 activeVideoEnd;
        bool isActiveLine;
    };

    // Video read from a stream (standard input, a named pipe) that may never end, with no
    // metadata: the video system is given, the rest has defaults. See loadStream.
    struct StreamInput {
        bool cvbs = false;
        VideoSystem system = NTSC;
        Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
        qint32 blackLevel = -1, whiteLevel = -1;      // TBC only; -1 for the standard level
        bool isSubcarrierLocked = false;
        // The size of each of the two buffers (the frozen one and the live one), in frames
        qint32 bufferFrames = 32;
        // Hold the producer back to the pace of the viewer instead of overwriting old video: for
        // video that comes faster than it plays (a file), not for a live input
        bool throttle = false;
    };

    void loadSource(QString inputFileName);
    void loadStream(const StreamInput &input);
    bool getIsLive();
    // The audio samples before each field (one more entry than there are fields), from the metadata; empty if it does not say
    QVector<qint64> getAudioSamplesBeforeFields();
    // The frames of the metadata that the video holds whole (a file cut short has fewer)
    qint32 numberOfWholeFrames();
    // For a stream: take count of what has arrived. Call from the thread of the window.
    void updateLive();
    // The first frame that can be shown (the earlier ones have left the buffer of a stream)
    qint32 getFirstAvailableFrame();
    // True once a stream has ended: it has no more frames to come
    bool getIsLiveEnded();
    qint32 getLookAheadFrames();
    // A hint for the user about the input (empty if none), such as an encoding that looks wrong
    QString getStreamHint();
    // Live input: the VBI and the SNR are worked out from the fields as they are shown (a pipe has no
    // metadata). The SNR of each frame analysed so far, by frame number, for the graphs
    // The history of the SNR of what has been received (every frame, if it is measured on a thread of
    // its own, as it is unless the input is throttled; else the frames that were shown). Averaged to
    // at most maxPoints points.
    QVector<QPointF> getLiveSnrHistory(bool black, qint32 maxPoints);
    bool getLiveSnrIsWholeHistory();
    bool getLiveSnrEnabled();
    void setLiveSnrEnabled(bool enabled);
    // Forget the SNR history so far
    void clearLiveSnr();

    // A file: measure the SNR from the video instead of using the metadata, on a thread of its own
    void startFileSnr();
    void stopFileSnr();
    bool getFileSnrActive();
    qint32 getFileSnrProgressPercent();
    QVector<QPointF> getFileSnrHistory(bool black, qint32 maxPoints);
    // Live input, two buffers: freezing (pausing) keeps the frames around the one shown as they are,
    // while the input goes on filling the ring with newer ones; playing again carries on from the
    // frozen one into them. The number of frames and the first one available are the frozen
    // buffer's while frozen.
    void freezeLive(qint32 aroundFrame);
    void unfreezeLive();
    // Let go of what was frozen at once, rather than once it has been played through
    void dropLiveProtection();
    bool getIsLiveFrozen();
    // Once the viewer is past what was frozen, let it go back to being part of the ring
    void releaseLiveProtection(qint32 currentFrame);
    // Frames the input has delivered (whatever is frozen), and whether a frame is still held
    qint32 getFramesReceived();
    bool isFrameHeld(qint32 frame);
    // The first held frame at or after this one (-1 if none), with what the decoder looks behind to
    qint32 nextHeldFrame(qint32 frame);
    qint32 getLookBehindFrames();
    void unloadSource();
    bool getIsSourceLoaded();
    // A stacking session (.stack): the stack of its sources, whose metadata has no SNR
    bool getIsStack();
    // "N sources, TBC" or "N sources, CVBS" for a stacking session, empty for another source
    QString getStackDescription();
    void saveSourceMetadata();
    QString getCurrentSourceFilename();
    QString getCurrentMetadataFilename();
    QString getMetadataFormatName();
    QString getLastIOError();

    void setHighlightDropouts(bool _state);
    void setChromaDecoder(bool _state);
    void setFieldOrder(bool _state);
	void setCombine(bool _state);
    bool getHighlightDropouts();
    bool getChromaDecoder();
    bool getFieldOrder();

    enum ViewMode {
        FRAME_VIEW,
        SPLIT_VIEW,
        FIELD_VIEW,
    };
    void setViewMode(ViewMode viewMode);
    void setStretchField(bool _stretch);

    ViewMode getViewMode();
    bool getFrameViewEnabled();
    bool getFieldViewEnabled();
    bool getSplitViewEnabled();
    bool getStretchField();

    enum SourceMode {
        ONE_SOURCE,
        LUMA_SOURCE,
        CHROMA_SOURCE,
        BOTH_SOURCES,
    };
    SourceMode getSourceMode();
    void setSourceMode(SourceMode sourceMode);

    // Everything a second TbcSource needs to show the same images as this one. Playback decodes
    // frames ahead in other threads, each with a TbcSource of its own opened this way.
    struct RenderSettings {
        QString sourceFilename, chromaFilename, metadataFilename;
        PalColour::Configuration palConfiguration;
        Comb::Configuration ntscConfiguration;
        MonoDecoder::MonoConfiguration monoConfiguration;
        OutputWriter::Configuration outputConfiguration;
        LdDecodeMetaData::VideoParameters videoParameters;
        bool isLive = false;
        StreamInput streamInput;
        bool chromaOn, dropoutsOn, reverseFoOn, stretchFieldOn, combine;
        ViewMode viewMode;
        SourceMode sourceMode;
        bool weave = false;         // images are weaves of the fields p and p+1 (see loadWeave)
    };

    RenderSettings getRenderSettings();
    quint32 getSettingsGeneration() const { return settingsGeneration; }
    // liveParent: for a stream, the source that reads it, which this one shares the stream of
    bool openForRendering(const RenderSettings &settings, const TbcSource *liveParent = nullptr);
    void applyRenderSettings(const RenderSettings &settings);
    // Use this image, rendered elsewhere, for the frame (field in field view) if it is the one loaded
    void setPrerenderedImage(qint32 position, const QImage &image);

    void load(qint32 frameNumber, qint32 fieldNumber);
    // A playback trick: an image of the frame view that is the weave of the fields p and p+1, whichever frames they belong to (the
    // fields are put top field first, so the lines are where they belong). The next load() goes back to real frames.
    void loadWeave(qint32 p);
    void setWeavePlayback(bool on) { weaveOn = on; }
    // Forget the image given by setPrerenderedImage and the cache (after a playback of weaves)
    void clearPrerendered();

    QImage getImage();
    qint32 getNumberOfFrames();
    qint32 getNumberOfFields();
    bool getIsWidescreen();
    VideoSystem getSystem();
    QString getSystemDescription();
    qint32 getFrameHeight();
    qint32 getFrameWidth();

    VbiDecoder::Vbi getFrameVbi();
    bool getIsFrameVbiValid();
    VideoIdDecoder::VideoId getFrameVideoId();
    bool getIsFrameVideoIdValid();
    VitcDecoder::Vitc getFrameVitc();
    bool getIsFrameVitcValid();

    QVector<double> getBlackSnrGraphData();
    QVector<double> getWhiteSnrGraphData();
    QVector<double> getDropOutGraphData();
    QVector<double> getVisibleDropOutGraphData();
    qint32 getGraphDataSize();

    bool getIsDropoutPresent();

    const LdDecodeMetaData::VideoParameters &getVideoParameters();
    void setVideoParameters(const LdDecodeMetaData::VideoParameters &videoParameters);

    const ComponentFrame &getComponentFrame();
    ScanLineData getScanLineData(qint32 scanLine);

    qint32 getFirstFieldNumber();
    qint32 getSecondFieldNumber();

    qint32 getCcData0();
    qint32 getCcData1();

    void setChromaConfiguration(const PalColour::Configuration &palConfiguration, const Comb::Configuration &ntscConfiguration,
                                const OutputWriter::Configuration &outputConfiguration);
    const PalColour::Configuration &getPalConfiguration();
    const Comb::Configuration &getNtscConfiguration();
    const MonoDecoder::MonoConfiguration &getMonoConfiguration();
    const OutputWriter::Configuration &getOutputConfiguration();

    qint32 startOfNextChapter(qint32 currentFrameNumber);
    qint32 startOfChapter(qint32 currentFrameNumber);
    // What a search by number finds (a frame from 1, or -1): the chapter, the picture number (CAV), the time in seconds (CLV)
    qint32 frameOfChapter(qint32 chapter) const;
    qint32 frameOfPicture(qint32 picture) const;
    qint32 frameOfTime(qint32 seconds) const;
    bool hasChapters() const { return !chapterStarts.isEmpty(); }
    bool hasPictureNumbers() const { return !pictureFrames.isEmpty(); }
    bool hasTimes() const { return !timeFrames.isEmpty(); }
    // The start of the chapter that comes after a frame, and of the one before it (-1 if there is none)
    qint32 chapterStartAfter(qint32 frame) const;
    qint32 chapterStartBefore(qint32 frame) const;
    // The first frame of every chapter, in order (empty if the capture has no chapters)
    QVector<qint32> chapterStartFrames() const;
    // A chapter of the disc as the VBI gives it: its number, the frame of the capture where it begins, and what the disc says there (the picture number
    // of a CAV disc, the time in seconds of a CLV disc; -1 if it does not)
    struct ChapterEntry {
        qint32 chapter = -1;
        qint32 frame = -1;
        qint32 picture = -1;
        qint32 seconds = -1;
    };
    QVector<ChapterEntry> chapterList() const;

signals:
    void busy(QString information);
    void finishedLoading(bool success);
    void finishedSaving(bool success);

private slots:
    void finishBackgroundLoad();
    void finishBackgroundSave();

private:
    void applyFieldOrder();
    bool sourceReady;
    bool weaveOn = false;
    qint32 weaveStart = 0;      // the first field of the weave that is loaded, 0 if the loaded frame is a real one

    // Frame data
    QVector<double> blackSnrGraphData;
    QVector<double> whiteSnrGraphData;
    QVector<double> dropoutGraphData;
    QVector<double> visibleDropoutGraphData;

    // Image options
    bool chromaOn;
    bool dropoutsOn;
    bool reverseFoOn;
    bool stretchFieldOn;
    ViewMode viewMode;

    // Source globals
    SourceVideo sourceVideo;
    SourceVideo chromaSourceVideo;
    SourceMode sourceMode;
    LdDecodeMetaData ldDecodeMetaData;
    QString currentSourceFilename;
    QString currentMetadataFilename;
    QString lastIOError;
    QString streamHint;
    QString currentChromaFilename;
    // A stream: what has arrived
    bool isLive = false;
    StreamInput streamInput;
    // The levels the SNR is measured against: those of the capture when it was opened (its metadata, or
    // the standard ones and --black-level and --white-level for a live input). Adjusting the levels in the
    // video parameters, which is for the picture, does not move the SNR.
    LdDecodeMetaData::VideoParameters snrParameters;
    bool liveAnalyse = false;
    QSet<qint32> liveAnalysedFrames;
    QMap<qint32, double> liveBlackSnr, liveWhiteSnr;
    std::unique_ptr<LiveSnr> liveSnr;       // after sourceVideo: stopped before it is closed
    std::unique_ptr<FileSnr> fileSnr;
    void analyseLiveFrame(qint32 frameNumber);
    bool liveFrozen = false;
    bool liveProtected = false;
    qint32 frozenFirstFrame = 1, frozenLastFrame = 0;
    qint32 liveFrames = 0;
    qint32 liveFirstFrame = 1;
    bool liveEnded = false;
    quint32 settingsGeneration = 0;
    QImage prerenderedImage;
    qint32 prerenderedPosition = -1;
    quint32 prerenderedGeneration = 0;

    // Chroma decoder objects
    PalColour palColour;
    Comb ntscColour;
	MonoDecoder monoDecoder;
    OutputWriter outputWriter;

    // VBI decoders
    VbiDecoder vbiDecoder;
    VideoIdDecoder videoIdDecoder;
    VitcDecoder vitcDecoder;

    // Background loader globals
    QFutureWatcher<bool> watcher;
    QFuture<bool> future;

    // Metadata for the loaded frame
    qint32 firstFieldNumber, secondFieldNumber;
    LdDecodeMetaData::Field firstField, secondField;
    qint32 loadedFieldNumber, loadedFrameNumber;

    // Source fields needed to decode the loaded frame
    QVector<SourceField> inputFields;
    QVector<SourceField> chromaInputFields;
    qint32 inputStartIndex, inputEndIndex;
    bool inputFieldsValid;

    // Chroma decoder output for the loaded frame
    QVector<ComponentFrame> componentFrames;
    QVector<ComponentFrame> yFrames;
    QVector<ComponentFrame> cFrames;
    bool decodedFrameValid;

    // RGB image data for the loaded frame
    QImage cache;
    bool cacheValid;

    // Chroma decoder configuration
    PalColour::Configuration palConfiguration;
    Comb::Configuration ntscConfiguration;
	MonoDecoder::MonoConfiguration monoConfiguration;
    OutputWriter::Configuration outputConfiguration;
	bool combine = false;

    // Chapter map
    QVector<qint32> chapterMap;
    // The first frame of each chapter (by its number), of each picture number and of each second of the time of the disc
    QMap<qint32, qint32> chapterStarts, pictureFrames, timeFrames;
    QMap<qint32, ChapterEntry> chapterDetails;

    void resetState();
    void invalidateImageCache();
    void configureChromaDecoder();
    void loadInputFields();
    void decodeFrame();
    QImage generateQImage();
    void generateData();
    bool startBackgroundLoad(QString sourceFilename);
    bool startBackgroundStream();
    bool startBackgroundSave(QString metadataFilename);
};

#endif // TBCSOURCE_H
