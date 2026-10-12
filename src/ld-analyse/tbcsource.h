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

#include <QObject>
#include <QImage>
#include <QPainter>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>

// TBC library includes
#include "sourcevideo.h"
#include "lddecodemetadata.h"
#include "linenumber.h"
#include "vbidecoder.h"
#include "videoiddecoder.h"
#include "vitcdecoder.h"

// Chroma decoder includes
#include "palcolour.h"
#include "comb.h"
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
    // For a stream: take count of what has arrived. Call from the thread of the window.
    void updateLive();
    // The first frame that can be shown (the earlier ones have left the buffer of a stream)
    qint32 getFirstAvailableFrame();
    // True once a stream has ended: it has no more frames to come
    bool getIsLiveEnded();
    qint32 getLookAheadFrames();
    // A hint for the user about the input (empty if none), such as an encoding that looks wrong
    QString getStreamHint();
    // Live input, two buffers: freezing (pausing) keeps the frames around the one shown as they are,
    // while the input goes on filling the ring with newer ones; playing again carries on from the
    // frozen one into them. The number of frames and the first one available are the frozen
    // buffer's while frozen.
    void freezeLive(qint32 aroundFrame);
    void unfreezeLive();
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
    void saveSourceMetadata();
    QString getCurrentSourceFilename();
    QString getCurrentMetadataFilename();
    QString getMetadataFormatName();
    QString getLastIOError();

    void setHighlightDropouts(bool _state);
    void setChromaDecoder(bool _state);
    void setFieldView(bool _state);
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
    };

    RenderSettings getRenderSettings();
    quint32 getSettingsGeneration() const { return settingsGeneration; }
    // liveParent: for a stream, the source that reads it, which this one shares the stream of
    bool openForRendering(const RenderSettings &settings, const TbcSource *liveParent = nullptr);
    void applyRenderSettings(const RenderSettings &settings);
    // Use this image, rendered elsewhere, for the frame (field in field view) if it is the one loaded
    void setPrerenderedImage(qint32 position, const QImage &image);

    void load(qint32 frameNumber, qint32 fieldNumber);

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

signals:
    void busy(QString information);
    void finishedLoading(bool success);
    void finishedSaving(bool success);

private slots:
    void finishBackgroundLoad();
    void finishBackgroundSave();

private:
    bool sourceReady;

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

    void resetState();
    void invalidateImageCache();
    void configureChromaDecoder();
    void loadInputFields();
    void decodeFrame();
    QImage generateQImage();
    QImage generateChromaImage();
    QImage generateMonoImage();
    void generateData();
    bool startBackgroundLoad(QString sourceFilename);
    bool startBackgroundStream();
    bool startBackgroundSave(QString metadataFilename);
};

#endif // TBCSOURCE_H
