/******************************************************************************
 * mainwindow.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QtDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QStyle>
#include <QMenu>
#include <QMenuBar>
#include <QActionGroup>
#include <QPushButton>
#include <QSignalBlocker>
#include <QPainter>
#include <QMap>
#include <QHash>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <memory>
#include <QIcon>

#include "oscilloscopedialog.h"
#include "vectorscopedialog.h"
#include "aboutdialog.h"
#include "vbidialog.h"
#include "dropoutanalysisdialog.h"
#include "visibledropoutanalysisdialog.h"
#include "blacksnranalysisdialog.h"
#include "whitesnranalysisdialog.h"
#include "busydialog.h"
#include "closedcaptionsdialog.h"
#include "videoparametersdialog.h"
#include "chromadecoderconfigdialog.h"
#include "configuration.h"
#include "shortcutsdialog.h"
#include "tbcsource.h"
#include "playbackcontroller.h"
#include "playbackrenderer.h"
#ifdef LDDECODE_HAVE_AUDIO
#include "audioengine.h"
#include "waveformpanel.h"
#include "audioconfigdialog.h"
#include "efmdecoding.h"
#include "sidecaraudio.h"
#endif
#include "videofilter.h"
#include "videofilterdialog.h"
#include "theme.h"
#include "themeeditordialog.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // streamInput: video piped in (inputFilenameParam is then "-") rather than a file
    explicit MainWindow(QString inputFilenameParam, const TbcSource::StreamInput *streamInput = nullptr,
                        QWidget *parent = nullptr);
	TbcSource& getTbcSource();
    ~MainWindow();

private slots:
    // Menu bar handlers
    void on_actionExit_triggered();
    void on_actionOpen_TBC_file_triggered();
    void on_actionReload_TBC_triggered();
    void on_actionSave_Metadata_triggered();
    void on_actionLine_scope_triggered();
    void on_actionVectorscope_triggered();
    void on_actionAbout_ld_analyse_triggered();
    void on_actionVBI_triggered();
    void on_actionDropout_analysis_triggered();
    void on_actionVisible_Dropout_analysis_triggered();
    void on_actionSNR_analysis_triggered();
    void on_actionWhite_SNR_analysis_triggered();
    void on_actionSave_frame_as_PNG_triggered();
    void on_actionZoom_In_triggered();
    void on_actionZoom_Out_triggered();
    void on_actionZoom_1x_triggered();
    void on_actionZoom_2x_triggered();
    void on_actionZoom_3x_triggered();
    void on_actionClosed_Captions_triggered();
    void on_actionVideo_parameters_triggered();
    void on_actionChroma_decoder_configuration_triggered();
    void on_actionToggleChromaDuringSeek_triggered();

    // Media control frame handlers
    void on_previousPushButton_clicked();
    void on_nextPushButton_clicked();
    void on_previousPushButton_pressed();
    void on_previousPushButton_released();
    void on_nextPushButton_pressed();
    void on_nextPushButton_released();
    void on_endPushButton_clicked();
    void on_startPushButton_clicked();
    void on_posNumberSpinBox_editingFinished();
    void on_posHorizontalSlider_valueChanged(int value);
    void on_posHorizontalSlider_sliderPressed();
    void on_posHorizontalSlider_sliderReleased();
    void on_videoPushButton_clicked();
    void on_aspectPushButton_clicked();
    void on_dropoutsPushButton_clicked();
    void on_sourcesPushButton_clicked();
    void on_viewPushButton_clicked();
    void on_fieldOrderPushButton_clicked();
    void on_zoomInPushButton_clicked();
    void on_zoomOutPushButton_clicked();
    void on_originalSizePushButton_clicked();
    void on_mouseModePushButton_clicked();
    //void on_autoResizeButton_clicked();
	void on_toggleAutoResize_toggled(bool checked);
	void on_actionResizeFrameWithWindow_toggled(bool checked);

    // Miscellaneous handlers
    void scopeCoordsChangedSignalHandler(qint32 xCoord, qint32 yCoord);
    void vectorscopeChangedSignalHandler();
    void onSliderDebounceTimeout();
    void onDragPauseTimeout();
    void mousePressEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void videoParametersChangedSignalHandler(const LdDecodeMetaData::VideoParameters &videoParameters);
    void chromaDecoderConfigChangedSignalHandler();

    // Tbc Source signal handlers
    void on_busy(QString infoMessage);
    void on_finishedLoading(bool success);
    void on_finishedSaving(bool success);
	
	// UI handler
	void resize_on_aspect(int passesLeft = 3);
	void resizeForZoom();

private:
    Ui::MainWindow *ui;

    // Dialogues
    OscilloscopeDialog* oscilloscopeDialog;
    VectorscopeDialog* vectorscopeDialog;
    AboutDialog* aboutDialog;
    VbiDialog* vbiDialog;
    DropoutAnalysisDialog* dropoutAnalysisDialog;
    VisibleDropOutAnalysisDialog* visibleDropoutAnalysisDialog;
    BlackSnrAnalysisDialog* blackSnrAnalysisDialog;
    WhiteSnrAnalysisDialog* whiteSnrAnalysisDialog;
    BusyDialog* busyDialog;
    ClosedCaptionsDialog *closedCaptionDialog;
    VideoParametersDialog *videoParametersDialog;
    ChromaDecoderConfigDialog *chromaDecoderConfigDialog;

    // Class globals
    Configuration configuration;
    QLabel sourceVideoStatus;
    QLabel fieldNumberStatus;
    QLabel vbiStatus;
    QLabel timeCodeStatus;
    TbcSource tbcSource;
    bool displayAspectRatio;
	bool autoResize = true;
	qint64 zoomResizeUntil = 0;
	bool resizeFrameWithWindow = true;
    qint32 lastScopeLine;
    qint32 lastScopeDot;
    qint32 currentFieldNumber, currentFrameNumber;
    double scaleFactor;
    QPalette buttonPalette;

    // Colour theme and widget style (View > Theme, View > Style)
    ThemeMode themeMode = ThemeMode::Auto;
    QString customPalette;       // custom theme colours (serialised)
    QString styleName;           // empty: Auto
    bool isDarkTheme = false;
    QAction *customThemeAction = nullptr;
    QString lastFilename;
    
    // Slider debouncing
    QTimer* sliderDebounceTimer;
    QTimer* dragPauseTimer;
    QTimer* resizeTimer;
    qint32 pendingSliderValue;
    bool sliderDragging;
    
    // Chroma toggle during seek
    bool chromaSeekMode;
    bool originalChromaState;
    QTimer* seekTimer;

    // Video playback: the controller is the clock, MainWindow shows what falls due
    PlaybackController *playback;
    PlaybackRenderer *renderer;
    VideoFilter videoFilter;
    VideoFilterSettings videoFilterSettings;
    VideoFilterDialog *videoFilterDialog = nullptr;
    qint32 lastFilteredPosition = -1;
    bool deinterlacedView = false;     // the view button's Deinterlaced mode: a frame through the chosen deinterlacer
    QString updateVideoFilterGraph();
    QPushButton *filtersPushButton = nullptr;     // the master switch of the filters
    QAction *actionFilters = nullptr;
    bool filtersEnabled = false;
    void setFiltersEnabled(bool enabled);
    void updateFiltersButton();
    void updateFieldOrderVisibility();
    QAction *actionFieldOrder = nullptr;
    void saveVideoFilterConfig();
    void setupVideoFilter();
    void applyVideoFilter(const VideoFilterSettings &settings);
    bool filterImage(QImage &image, qint32 displayWidth, qint32 displayHeight);
    // The frames around a frame, rendered as it is, for deinterlacing a picture that is not playing
    std::unique_ptr<TbcSource> contextSource;
    quint32 contextGeneration = 0;
    QMap<qint32, QImage> contextImages;
    QImage contextImage(qint32 frame);
    void resetContext();
    quint32 renderGeneration = 0;
    bool firstImageShown = false;
    qint32 playbackStartPosition = 0;
    qint32 playbackFirstPosition = 1, playbackLastPosition = 1;
    void playbackLooped();
    qint32 playbackDue = 0;
    void showRenderedImage();
    // A stream (live input): count what has arrived, and keep the controls and playback to it
    QTimer *liveTimer = nullptr;
    // The frame view at field rate while playing (a trick of the playback, see actionWeave)
    bool weaveActive = false;
    QAction *actionVideoFilter = nullptr;
    void buildConfigMenu();
    // Seeking by the keyboard (the keys of a jog and a shuttle) and the keys of the program that can be changed
    QMenu *playbackMenu = nullptr;
    QTimer *scanTimer = nullptr;
    QElapsedTimer scanClock;
    double scanSpeed = 0.0;               // times the normal speed, negative backward; 0: not scanning
    double scanRemainder = 0.0;
    bool scanChromaSuppressed = false;
    QHash<QAction *, QList<QKeySequence>> shortcutDefaults;
    QAction *actionKeyboardShortcuts = nullptr;
    void setupSeekActions();
    qint32 seekPosition();
    qint32 seekLast();
    qint32 seekFirst();
    qint32 seekUnitsPerSecond();
    void seekTo(qint32 position);
    void seekBy(qint32 units);
    void startScan(double speed);
    void stopScan();
    void scanTick();
    void shuttle(qint32 direction);
    void shuttleStop();
    QVector<ShortcutCommand> shortcutCommands();
    void captureShortcutDefaults();
    void applyStoredShortcuts();
    void showShortcuts();
    QAction *actionWeave = nullptr;
    void updateLive();
    void updateLiveControls();
    void updateLiveGraphs();
    QTimer *snrFileTimer = nullptr;        // refreshes the SNR graphs while a file is measured from its video
    void setFileSnrMeasuring(bool measuring);
    void updateFileSnrGraphs();
    qint32 liveGraphTick = 0;
    QPushButton *playPushButton;
    QAction *actionPlayPause;
    QAction *actionLoop;
    QActionGroup *speedActions;
    QAction *speedNormalAction = nullptr;
    QAction *speedSeparator = nullptr;
#ifdef LDDECODE_HAVE_AUDIO
    // The audio that plays with the video, and its waveform under it (shown when the capture has audio)
    AudioEngine *audio = nullptr;
    WaveformPanel *waveform = nullptr;
    QAction *actionWaveform = nullptr;
    bool waveformWanted = true;
    AudioConfigDialog *audioConfig = nullptr;
    QAction *actionAudioConfig = nullptr;
    void showAudioConfig();
    void updateAudioStatistics();
    QTimer *audioStatsTimer = nullptr;
    QString efmSourceName();
    void stopEfmDecoders();
    void showWaveform(bool shown);
    // A live input's other tracks: its audio (a growing file read as the waveform and the sound) and its EFM (kept, and given to the decoders)
    std::shared_ptr<AudioRing> liveAnalogue;        // (the last part of the audio of a live input: a ring)
    std::unique_ptr<EfmByteRing> liveEfm;           // (the last part of its EFM)
    QTimer *liveAudioTimer = nullptr;
    QMap<quint32, QString> liveKinds;
    quint32 livePcmTrack = 0;
    qint32 livePcmBits = 16;
    double livePcmRate = 44100.0;
    qint64 liveEfmFed[2] = { 0, 0 };
    QVector<QPair<qint64, qint64>> liveEfmIndex;      // (byte in the file of the EFM kept, time in the capture) for each piece of EFM that came
    qint64 liveEfmBytes = 0;
    bool liveEfmStartPending[2] = { false, false };
    bool livePcmStartSet = false;
    QString audioLoadedKey;      // what loadAudio loaded: a source (a file, or the live input) is loaded once
    void loadLiveAudio();
    void pollLiveTracks();
    void feedLiveEfm();
    void applyWaveformDensity(qint32 density);
    void startEfmDecoding(qint32 decoder, bool noTimecodes, bool conceal, bool pad, bool select = true);
    void startSidecars(const QStringList &files);
    void stopSidecars();
    QList<AudioFileDecoding *> sidecarDecodings;
    EfmDecoding *efmDecoders[2] = { nullptr, nullptr };
    void saveImportedAudio(const QVector<qint16> &samples, double sampleRate, const QString &name);
    void setupAudio();
    void loadAudio();
    void unloadAudio();
    bool audioStart(qint32 position);
    bool audioStartAt(double videoSeconds);
    void audioFollowVideo(double videoSeconds);
    void audioStop();
    void updateAudioPlayhead();
    void waveformSeek(double seconds);
    double positionSeconds(qint32 position);
#endif
    void setPlaybackOptionsVisible(bool visible);
    void setupPlayback();
    void togglePlayback();
    void pausePlayback();
    void playbackPositionDue(qint32 position);
    void updatePlaybackState(bool playing);
    void updatePlayButtonIcon();
    bool playbackShowsPause = false;

    // Update GUI methods
    void setGuiEnabled(bool enabled);
    void resetGui();
    void updateGuiLoaded();
    void updateGuiUnloaded();
    void updateAspectPushButton();
    void updateSourcesPushButton();
    void setViewValues();
    void setCurrentFrame(qint32 frame);
    void setCurrentField(qint32 field);
    void sanitizeCurrentPosition();

    // Analysis graphs and colour theme
    void updateGraphs();
    void setupAppearanceMenus();
    void updateAppearance();
    void refreshAfterAppearanceChange(bool updateGraphData);
    void customizeTheme();

	// Image display methods
    void showImage();
    void updateImage();
    qint32 getAspectAdjustment();
    void updateImageViewer();
    void hideImage();
    void resizeFrameToWindow();
    void enterChromaSeekMode(QPushButton* button);
    void exitChromaSeekMode(QPushButton* button);

    // TBC source signal handlers
    void loadTbcFile(QString inputFileName);
    void updateOscilloscopeDialogue();
    void updateVectorscopeDialogue();
    void mouseScanLineSelect(qint32 oX, qint32 oY);
	void resizeEvent(QResizeEvent *event);
};

#endif // MAINWINDOW_H
