/******************************************************************************
 * mainwindow.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QDateTime>
#include "mainwindow.h"
#include <QJsonObject>
#include <QSet>
#include <functional>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QTextStream>
#include "ldxcapture.h"
#include "ldxbytetrack.h"
#include "ldxstream.h"
#include "ui_mainwindow.h"
#include "tbc/logging.h"
#include "cvbs.h"

#include <QActionGroup>
#include <QDialog>
#include <QVBoxLayout>

MainWindow::MainWindow(QString inputFilenameParam, const TbcSource::StreamInput *streamInput, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Set up dialogues
    oscilloscopeDialog = new OscilloscopeDialog(this);
    vectorscopeDialog = new VectorscopeDialog(this);
    aboutDialog = new AboutDialog(this);
    vbiDialog = new VbiDialog(this);
    dropoutAnalysisDialog = new DropoutAnalysisDialog(this);
    visibleDropoutAnalysisDialog = new VisibleDropOutAnalysisDialog(this);
    blackSnrAnalysisDialog = new BlackSnrAnalysisDialog(this);
    whiteSnrAnalysisDialog = new WhiteSnrAnalysisDialog(this);
    busyDialog = new BusyDialog(this);
    closedCaptionDialog = new ClosedCaptionsDialog(this);
    videoParametersDialog = new VideoParametersDialog(this);
    chromaDecoderConfigDialog = new ChromaDecoderConfigDialog(this);

    // Add a status bar to show the state of the source video file
    ui->statusBar->addWidget(&sourceVideoStatus);
    ui->statusBar->addWidget(&fieldNumberStatus);
    ui->statusBar->addWidget(&vbiStatus);
    ui->statusBar->addWidget(&timeCodeStatus);
    sourceVideoStatus.setText(tr("No source video file loaded"));
    fieldNumberStatus.setText(tr(" -  Fields: ./."));
    vbiStatus.hide();
    timeCodeStatus.hide();

    // Set the initial field/frame number
    setCurrentFrame(1);

    // Connect to the scan line changed signal from the oscilloscope dialogue
    connect(oscilloscopeDialog, &OscilloscopeDialog::scopeCoordsChanged, this, &MainWindow::scopeCoordsChangedSignalHandler);
    lastScopeLine = 1;
    lastScopeDot = 1;

    // Make shift-clicking on the oscilloscope change the black/white level
    connect(oscilloscopeDialog, &OscilloscopeDialog::scopeLevelSelect, videoParametersDialog, &VideoParametersDialog::levelSelected);

    // Connect to the changed signal from the vectorscope dialogue
    connect(vectorscopeDialog, &VectorscopeDialog::scopeChanged, this, &MainWindow::vectorscopeChangedSignalHandler);

    // Connect to the video parameters changed signal
    connect(videoParametersDialog, &VideoParametersDialog::videoParametersChanged, this, &MainWindow::videoParametersChangedSignalHandler);

    // Connect to the chroma decoder configuration changed signal
    connect(chromaDecoderConfigDialog, &ChromaDecoderConfigDialog::chromaDecoderConfigChanged, this, &MainWindow::chromaDecoderConfigChangedSignalHandler);

    // Connect to the TbcSource signals (busy and finished loading)
    connect(&tbcSource, &TbcSource::busy, this, &MainWindow::on_busy);
    connect(&tbcSource, &TbcSource::finishedLoading, this, &MainWindow::on_finishedLoading);
    connect(&tbcSource, &TbcSource::finishedSaving, this, &MainWindow::on_finishedSaving);

    // Load the window geometry and settings from the configuration
    restoreGeometry(configuration.getMainWindowGeometry());
    scaleFactor = 1.0; // the zoom is not remembered: every input opens at its original size

    vbiDialog->restoreGeometry(configuration.getVbiDialogGeometry());
    oscilloscopeDialog->restoreGeometry(configuration.getOscilloscopeDialogGeometry());
    vectorscopeDialog->restoreGeometry(configuration.getVectorscopeDialogGeometry());
    dropoutAnalysisDialog->restoreGeometry(configuration.getDropoutAnalysisDialogGeometry());
    visibleDropoutAnalysisDialog->restoreGeometry(configuration.getVisibleDropoutAnalysisDialogGeometry());
    blackSnrAnalysisDialog->restoreGeometry(configuration.getBlackSnrAnalysisDialogGeometry());
    whiteSnrAnalysisDialog->restoreGeometry(configuration.getWhiteSnrAnalysisDialogGeometry());
    closedCaptionDialog->restoreGeometry(configuration.getClosedCaptionDialogGeometry());
    videoParametersDialog->restoreGeometry(configuration.getVideoParametersDialogGeometry());
    chromaDecoderConfigDialog->restoreGeometry(configuration.getChromaDecoderConfigDialogGeometry());

    // Load view options from configuration
    resizeFrameWithWindow = configuration.getResizeFrameWithWindow();
    ui->actionResizeFrameWithWindow->setChecked(resizeFrameWithWindow);

    // Colour theme and widget style: View > Theme and View > Style, saved in the
    // configuration. --force-dark-theme overrides the theme for this session
    // without changing the setting.
    customPalette = configuration.getCustomPalette();
    const qint32 savedTheme = configuration.getThemeMode();
    themeMode = (savedTheme >= static_cast<qint32>(ThemeMode::Auto) && savedTheme <= static_cast<qint32>(ThemeMode::DarkGrey))
        ? static_cast<ThemeMode>(savedTheme) : ThemeMode::Auto;
    if (themeMode == ThemeMode::Custom && customPalette.isEmpty()) themeMode = ThemeMode::Auto;
    if (qApp->property("forceDarkTheme").toBool()) themeMode = ThemeMode::Dark;

    // A saved style that is not installed here falls back to Auto
    styleName = configuration.getStyleName();
    if (!styleName.isEmpty() && !availableStyles().contains(styleName, Qt::CaseInsensitive)) styleName.clear();

    isDarkTheme = applyAppearance(styleName, themeMode, customPalette);
    setupAppearanceMenus();

    // Store the current button palette for the show dropouts button
    // Use application palette to ensure it respects theme settings
    buttonPalette = QApplication::palette();

    // Initialize slider debouncing
    sliderDebounceTimer = new QTimer(this);
    sliderDebounceTimer->setSingleShot(true);
    sliderDebounceTimer->setInterval(100); // 100ms debounce
    connect(sliderDebounceTimer, &QTimer::timeout, this, &MainWindow::onSliderDebounceTimeout);
    
    // Initialize drag pause timer for visual feedback during long drags
    dragPauseTimer = new QTimer(this);
    dragPauseTimer->setSingleShot(true);
    dragPauseTimer->setInterval(150); // 150ms pause before updating during drag
    connect(dragPauseTimer, &QTimer::timeout, this, &MainWindow::onDragPauseTimeout);
    
    // Initialize resize timer for delayed frame resizing
    resizeTimer = new QTimer(this);
    resizeTimer->setSingleShot(true);
    resizeTimer->setInterval(100); // 100ms delay for resize calculations
    connect(resizeTimer, &QTimer::timeout, this, &MainWindow::resizeFrameToWindow);
    
    sliderDragging = false;
    
    // Initialize chroma seek mode tracking
    chromaSeekMode = false;
    originalChromaState = false;
    
    // Set up button hold detection timer
    seekTimer = new QTimer(this);
    seekTimer->setSingleShot(true);
    seekTimer->setInterval(200); // 200ms to distinguish click from hold
    connect(seekTimer, &QTimer::timeout, this, [this]() {
        // Timer expired - enter chroma seek mode
        if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
            chromaSeekMode = true;
            originalChromaState = true;
            tbcSource.setChromaDecoder(false);
            ui->videoPushButton->setText(tr("Source"));
        }
    });
    
    // Button press/release signals for chroma seek mode are auto-connected by Qt's auto-connection mechanism
    pendingSliderValue = -1;

    setupPlayback();
    snrFileTimer = new QTimer(this);
    snrFileTimer->setInterval(1000);
    connect(snrFileTimer, &QTimer::timeout, this, &MainWindow::updateFileSnrGraphs);
    for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
        connect(bar, &SnrSourceBar::clearRequested, this, [this]() {
            tbcSource.clearLiveSnr();
            updateLiveGraphs();
        });
        connect(bar, &SnrSourceBar::measuringToggled, this, [this](bool measuring) {
            if (!tbcSource.getIsLive()) {
                setFileSnrMeasuring(measuring);
                return;
            }
            tbcSource.setLiveSnrEnabled(measuring);
            for (SnrSourceBar *other: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
                other->setLiveSource(measuring, tbcSource.getLiveSnrIsWholeHistory());
            }
        });
    }
    setupVideoFilter();
    buildConfigMenu();
    setupSeekActions();
    captureShortcutDefaults();
    applyStoredShortcuts();

    // Field order: the button is left out when the window is narrow, so the menu has it too
    actionFieldOrder = new QAction(tr("Reverse field order"), this);
    actionFieldOrder->setCheckable(true);
    connect(actionFieldOrder, &QAction::triggered, this, &MainWindow::on_fieldOrderPushButton_clicked);
    ui->menuView->addAction(actionFieldOrder);

    // Set the GUI to unloaded
    updateGuiUnloaded();
    
    // Load configuration settings
    ui->actionToggleChromaDuringSeek->setChecked(configuration.getToggleChromaDuringSeek());

    // Video piped in, or a filename specified on the command line?
    if (streamInput) {
        lastFilename.clear();
        // Not a file: opening one is blocked from the start, not only once the input is up
        ui->actionOpen_TBC_file->setEnabled(false);
        ui->actionReload_TBC->setEnabled(false);
        tbcSource.loadStream(*streamInput);
    } else if (!inputFilenameParam.isEmpty()) {
        lastFilename = inputFilenameParam;
        tbcSource.loadSource(inputFilenameParam);
    } else {
        lastFilename.clear();
    }
}

MainWindow::~MainWindow()
{
    resetContext();
    // The decoding threads read the same files (or stream) as the source
    renderer->close();

    // Save the window geometry and settings to the configuration
    configuration.setMainWindowGeometry(saveGeometry());
    configuration.setVbiDialogGeometry(vbiDialog->saveGeometry());
    configuration.setOscilloscopeDialogGeometry(oscilloscopeDialog->saveGeometry());
    configuration.setVectorscopeDialogGeometry(vectorscopeDialog->saveGeometry());
    configuration.setDropoutAnalysisDialogGeometry(dropoutAnalysisDialog->saveGeometry());
    configuration.setVisibleDropoutAnalysisDialogGeometry(visibleDropoutAnalysisDialog->saveGeometry());
    configuration.setBlackSnrAnalysisDialogGeometry(blackSnrAnalysisDialog->saveGeometry());
    configuration.setWhiteSnrAnalysisDialogGeometry(whiteSnrAnalysisDialog->saveGeometry());
    configuration.setClosedCaptionDialogGeometry(closedCaptionDialog->saveGeometry());
    configuration.setVideoParametersDialogGeometry(videoParametersDialog->saveGeometry());
    configuration.setChromaDecoderConfigDialogGeometry(chromaDecoderConfigDialog->saveGeometry());
    if (remoteWindow) configuration.setPlaybackValue("remoteGeometry", remoteWindow->saveGeometry());
    if (infoPanel) configuration.setPlaybackValue("infoGeometry", infoPanel->saveGeometry());
    configuration.writeConfiguration();

    // Close the source video if open
    if (tbcSource.getIsSourceLoaded()) {
        tbcSource.unloadSource();
    }

    delete ui;
}

// The Config menu: what is set up (rather than a window of analysis, which is in the Window menu)
void MainWindow::buildConfigMenu()
{
    QMenu *menu = new QMenu(tr("&Config"), this);
    ui->menuBar->insertMenu(ui->menuHelp->menuAction(), menu);
    for (QAction *action: { ui->actionVideo_parameters, ui->actionChroma_decoder_configuration }) {
        ui->menuWindow->removeAction(action);
        menu->addAction(action);
    }
    if (actionVideoFilter) menu->addAction(actionVideoFilter);
#ifdef LDDECODE_HAVE_AUDIO
    menu->addSeparator();
    if (actionAudioConfig) menu->addAction(actionAudioConfig);
#endif
    menu->addSeparator();
    actionKeyboardShortcuts = menu->addAction(tr("&Keyboard shortcuts..."));
    actionKeyboardShortcuts->setObjectName("actionKeyboardShortcuts");
    connect(actionKeyboardShortcuts, &QAction::triggered, this, &MainWindow::showShortcuts);
    // (the separator that was before them is of no use at the end of the Window menu)
    const QList<QAction *> left = ui->menuWindow->actions();
    if (!left.isEmpty() && left.last()->isSeparator()) ui->menuWindow->removeAction(left.last());
}

// The ffmpeg video filters: Window > Video filter...
// Give the filter the graph of the filters on: the deinterlacer in the Deinterlaced view, the scaler
// if it is ticked, and the other filters (the command line) if the Filters button is on. Returns
// ffmpeg's complaint, if any.
QString MainWindow::updateVideoFilterGraph()
{
    // Deinterlacing is the Deinterlaced view, scaling has its own switch, and the Filters button is
    // the other filters (the command line)
    VideoFilterSettings settings = videoFilterSettings;
    settings.deinterlacerOn = deinterlacedView;
    settings.customOn = filtersEnabled;
    lastFilteredPosition = -1;
    return videoFilter.setGraph(settings.toGraph());
}

void MainWindow::saveVideoFilterConfig()
{
    configuration.setVideoFilter({
        { "deinterlacer", videoFilterSettings.deinterlacer },
        { "scalerOn", videoFilterSettings.scalerOn }, { "scaler", videoFilterSettings.scaler },
        { "custom", videoFilterSettings.custom },
    });
}

// The Filters button shows whether the video filters are on
void MainWindow::updateFiltersButton()
{
    if (!filtersPushButton) return;
    const bool wide = this->width() >= 930;
    filtersPushButton->setText(filtersEnabled ? (wide ? tr("Filters On") : tr("Filt Y"))
                                              : (wide ? tr("Filters Off") : tr("Filt N")));
}

// Switch all the video filters on or off, keeping their settings
void MainWindow::setFiltersEnabled(bool enabled)
{
    filtersEnabled = enabled;
    {
        const QSignalBlocker blocker(actionFilters);
        actionFilters->setChecked(enabled);
    }
    updateFiltersButton();
    videoFilterDialog->setFiltersOn(enabled);
    updateRemote();

    const QString problem = updateVideoFilterGraph();
    if (!problem.isEmpty()) {
        filtersEnabled = false;
        videoFilterDialog->setFiltersOn(false);
        updateVideoFilterGraph();
        const QSignalBlocker blocker(actionFilters);
        actionFilters->setChecked(false);
        updateFiltersButton();
        statusBar()->showMessage(tr("The video filters could not be used: %1").arg(problem), 10000);
    }
    saveVideoFilterConfig();

    // With no command line to switch on, ask what it is to be
    if (filtersEnabled && videoFilterSettings.custom.trimmed().isEmpty()) {
        videoFilterDialog->setFiltersOn(true);
        videoFilterDialog->setSettings(videoFilterSettings);
        videoFilterDialog->show();
        videoFilterDialog->raise();
    }

    if (tbcSource.getIsSourceLoaded() && !playback->isPlaying()) updateImageViewer();
}

// The ffmpeg video filters: Window > Video filter..., and the Filters button to switch them
void MainWindow::setupVideoFilter()
{
    videoFilterDialog = new VideoFilterDialog(this);

    QAction *action = new QAction(tr("Video filter..."), this);
    actionFilters = new QAction(tr("Other filters on"), this);
    actionFilters->setCheckable(true);
    // With the other windows and configuration dialogues
    actionVideoFilter = action;        // (it is in the Config menu: buildConfigMenu)

    // The button beside the other options
    filtersPushButton = new QPushButton(this);
    filtersPushButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    filtersPushButton->setMaximumWidth(100);
    filtersPushButton->setToolTip(tr("The ffmpeg filters typed in Window > Video filter... (the command line) on or off. "
                                     "Deinterlacing (the Deinterlaced view) and scaling are separate"));
    ui->horizontalLayout_3->insertWidget(ui->horizontalLayout_3->indexOf(ui->fieldOrderPushButton) + 1, filtersPushButton);

    if (!VideoFilter::isAvailable()) {
        action->setEnabled(false);
        action->setToolTip(tr("This build of ld-analyse has no video filters (libavfilter was not found)"));
        actionFilters->setEnabled(false);
        filtersPushButton->setEnabled(false);
        filtersEnabled = false;
        updateFiltersButton();
        return;
    }
    connect(action, &QAction::triggered, this, [this]() {
        videoFilterDialog->setSettings(videoFilterSettings);
        videoFilterDialog->setFiltersOn(filtersEnabled);
        videoFilterDialog->show();
        videoFilterDialog->raise();
    });
    connect(videoFilterDialog, &VideoFilterDialog::settingsChosen, this, &MainWindow::applyVideoFilter);
    connect(videoFilterDialog, &VideoFilterDialog::filtersToggled, this, &MainWindow::setFiltersEnabled);
    connect(actionFilters, &QAction::toggled, this, &MainWindow::setFiltersEnabled);
    connect(filtersPushButton, &QPushButton::clicked, this, [this]() { setFiltersEnabled(!filtersEnabled); });

    // The filters of the last session
    const QVariantMap saved = configuration.getVideoFilter();
    VideoFilterSettings settings;
    settings.deinterlacer = saved.value("deinterlacer", settings.deinterlacer).toString();
    settings.scalerOn = saved.value("scalerOn", false).toBool();
    settings.scaler = saved.value("scaler", settings.scaler).toString();
    settings.custom = saved.value("custom").toString();
    videoFilterSettings = settings;
    // The command line is remembered, but the Filters button starts off: a filter left on from
    // another session must not alter the picture without it being asked for
    filtersEnabled = false;
    if (!updateVideoFilterGraph().isEmpty()) {
        // They no longer work (an ffmpeg without a filter): start without
        videoFilterSettings = VideoFilterSettings();
        updateVideoFilterGraph();
    }
    {
        const QSignalBlocker blocker(actionFilters);
        actionFilters->setChecked(filtersEnabled);
    }
    updateFiltersButton();
}

// Show the picture through these filters (chosen in the dialogue; typing a new command line there also
// switches the Filters button on)
void MainWindow::applyVideoFilter(const VideoFilterSettings &settings)
{
    const VideoFilterSettings previous = videoFilterSettings;
    const bool wasEnabled = filtersEnabled;
    videoFilterSettings = settings;
    // (typing a command line and applying it is asking for it)
    if (!settings.custom.trimmed().isEmpty() && settings.custom != previous.custom) filtersEnabled = true;
    const QString problem = updateVideoFilterGraph();
    videoFilterDialog->showResult(problem);
    if (!problem.isEmpty()) {
        videoFilterSettings = previous;
        filtersEnabled = wasEnabled;
        updateVideoFilterGraph();
        return;
    }

    {
        const QSignalBlocker blocker(actionFilters);
        actionFilters->setChecked(filtersEnabled);
    }
    updateFiltersButton();
    videoFilterDialog->setFiltersOn(filtersEnabled);
    saveVideoFilterConfig();
    lastFilteredPosition = -1;
    if (tbcSource.getIsSourceLoaded() && !playback->isPlaying()) updateImageViewer();
}


// A frame as it is shown (decoded with the settings in use), made on a source of its own so that the
// one the window shows is left as it is. For the frames around the one deinterlaced. A few are kept.
QImage MainWindow::contextImage(qint32 frame)
{
    if (frame < 1 || frame > tbcSource.getNumberOfFrames()) return QImage();
    if (tbcSource.getIsLive() && !tbcSource.isFrameHeld(frame)) return QImage();

    // Anything that changes the picture makes what was kept of no use
    const quint32 generation = tbcSource.getSettingsGeneration();
    if (!contextSource || contextGeneration != generation) {
        const TbcSource::RenderSettings settings = tbcSource.getRenderSettings();
        if (!contextSource) {
            contextSource = std::make_unique<TbcSource>();
            if (!contextSource->openForRendering(settings, tbcSource.getIsLive() ? &tbcSource : nullptr)) {
                contextSource.reset();
                return QImage();
            }
        } else {
            contextSource->applyRenderSettings(settings);
        }
        contextGeneration = generation;
        contextImages.clear();
    }

    const auto found = contextImages.constFind(frame);
    if (found != contextImages.constEnd()) return found.value();

    contextSource->load(frame, 2 * frame - 1);
    const QImage image = contextSource->getImage();
    contextImages.insert(frame, image);
    // Keep the few nearest the one shown
    while (contextImages.size() > 6) {
        auto farthest = contextImages.begin();
        for (auto it = contextImages.begin(); it != contextImages.end(); ++it) {
            if (qAbs(it.key() - currentFrameNumber) > qAbs(farthest.key() - currentFrameNumber)) farthest = it;
        }
        contextImages.erase(farthest);
    }
    return image;
}

void MainWindow::resetContext()
{
    contextSource.reset();
    contextImages.clear();
}

// Show the image through the video filter, if there is one. Returns false if the picture
// on show should be left as it is (a filter that is still filling up gave nothing yet).
bool MainWindow::filterImage(QImage &image, qint32 displayWidth, qint32 displayHeight)
{
    if (!videoFilter.isActive() || image.isNull()) return true;

    // (the weave of fields plays at the rate of the fields: its images are numbered by the field, or two of them would have the same
    // position, and the second would not be taken for the one that follows the first, so the filter would start again for each)
    const bool isFieldView = tbcSource.getFieldViewEnabled() || weaveActive;
    const qint32 position = isFieldView ? currentFieldNumber : currentFrameNumber;

    VideoFilter::Format format;
    format.width = image.width();
    format.height = image.height();
    format.displayWidth = displayWidth;
    format.displayHeight = displayHeight;
    const bool isPal = tbcSource.getSystem() == PAL;
    format.frameRateNumerator = (isPal ? 25 : 30000) * (isFieldView ? 2 : 1);
    format.frameRateDenominator = isPal ? 1 : 1001;
    format.topFieldFirst = !tbcSource.getFieldOrder();
    format.interlaced = tbcSource.getFrameViewEnabled();

    // While playing, the images come in order and the filter can keep its history
    const bool continuous = playback->isPlaying() && lastFilteredPosition >= 0
                            && position > lastFilteredPosition;
    lastFilteredPosition = position;

    QImage filtered;
    if (!continuous && deinterlacedView && !isFieldView) {
        // A deinterlacer looks at the frames either side of the one it works on. A picture that is not
        // playing has none, and its own image twice gives it nothing to go on, so it is given the real ones
        const QImage previous = contextImage(position - 1);
        const QImage next = contextImage(position + 1);
        if (!previous.isNull() || !next.isNull()) {
            filtered = videoFilter.filterWithContext(previous.isNull() ? image : previous, image,
                                                     next.isNull() ? image : next, format);
        } else {
            filtered = videoFilter.filter(image, format, false);
        }
    } else {
        filtered = videoFilter.filter(image, format, continuous);
    }
    if (!videoFilter.lastError().isEmpty()) {
        // Do not go on failing for every image: drop the filter, and say why
        const QString problem = videoFilter.lastError();
        videoFilter.setGraph(QString());
        videoFilterSettings = VideoFilterSettings();
        configuration.setVideoFilter(QVariantMap());
        statusBar()->showMessage(tr("The video filter failed and was switched off: %1").arg(problem), 10000);
        return true;
    }
    if (filtered.isNull()) return !continuous;
    image = filtered;
    return true;
}

// A stream (live input): count what has arrived, and keep the controls and the playback to
// it. The frames that can be shown grow, and the first of them moves up as the buffer
// forgets the oldest.
void MainWindow::updateLive()
{
    if (!tbcSource.getIsLive() || !tbcSource.getIsSourceLoaded()) return;
    tbcSource.updateLive();

    const bool isFieldView = tbcSource.getFieldViewEnabled();
    const qint32 firstFrame = tbcSource.getFirstAvailableFrame();
    const bool fieldUnits = isFieldView || weaveActive;      // (the weave of fields counts fields)
    const qint32 first = fieldUnits ? 2 * firstFrame - 1 : firstFrame;
    const qint32 last = weaveActive ? tbcSource.getNumberOfFields() - 1 : (isFieldView ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames());
    const bool ended = tbcSource.getIsLiveEnded();

    updateLiveControls();

    // The SNR graphs, if they are open, grow with the video (about twice a second)
    if (++liveGraphTick % 15 == 0) updateLiveGraphs();

    if (playback->isPlaying()) {
        // Playing on from a frozen buffer: once past it, it is part of the ring again
        tbcSource.releaseLiveProtection(currentFrameNumber);

        // The ring overwrites what is not kept, and what was frozen is not contiguous with the
        // newer buffer if the input went on for longer than the ring holds: play on from the
        // first frame that is still there, rather than showing what has gone
        const qint32 probe = qMin(currentFrameNumber + 3, tbcSource.getNumberOfFrames());
        const bool currentHeld = tbcSource.isFrameHeld(currentFrameNumber);
        if (!currentHeld || !tbcSource.isFrameHeld(probe)) {
            const qint32 next = tbcSource.nextHeldFrame(currentHeld ? probe : currentFrameNumber);
            if (next > 0) {
                // (the decoder looks behind a frame, so a few before it must be there too)
                const qint32 target = qMin(next + tbcSource.getLookBehindFrames(), tbcSource.getNumberOfFrames());
                if (target > currentFrameNumber) {
                    pausePlayback();
                    setCurrentFrame(target);
                    QTimer::singleShot(0, this, [this]() { togglePlayback(); });
                    return;
                }
            }
        }
    }

    playback->setLive(true, ended);
    playback->setRange(first, last);
    renderer->setLastPosition(last);

    // How far behind the newest frame the picture is (playing from a frame of the buffer)
    const qint32 frames = tbcSource.getNumberOfFrames();
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    const qint32 behind = qMax(0, frames - currentFrameNumber);
    QString status = ended ? tr("Live input ended after %1 frames").arg(frames)
                           : tr("Live input - %1 frames received").arg(tbcSource.getFramesReceived());
    if (!ended && behind > 2) status += tr(" - %1 s behind").arg(behind / frameRate, 0, 'f', 1);
    if (!ended && !playback->isPlaying()) status += tr(" - paused: the last %1 s can be gone through")
                                                       .arg(qRound((frames - tbcSource.getFirstAvailableFrame()) / frameRate));
    if (!ended && tbcSource.getIsLiveFrozen() && tbcSource.getFramesReceived() > frames) {
        status += tr(" - the input goes on: %1 s since").arg((tbcSource.getFramesReceived() - frames) / frameRate, 0, 'f', 1);
    }
    sourceVideoStatus.setText(status);
    if (ended) liveTimer->stop();
}

// Loop and the speeds mean nothing for a live input, which plays as it arrives: hide them (and
// go back to normal speed, no loop)
void MainWindow::setPlaybackOptionsVisible(bool visible)
{
    if (!visible) {
        actionLoop->setChecked(false);
        speedNormalAction->setChecked(true);
        playback->setSpeed(1.0);
    }
    actionLoop->setVisible(visible);
    // (the weave is for a live input too)
    speedSeparator->setVisible(visible);
    for (QAction *action: speedActions->actions()) action->setVisible(visible);
}

// The SNR graphs of a live input are the history of what has been received
void MainWindow::updateLiveGraphs()
{
    if (!tbcSource.getIsLive()) return;
    const qint32 frames = tbcSource.getFramesReceived();
    static constexpr qint32 MAX_POINTS = 4000;

    if (blackSnrAnalysisDialog->isVisible()) {
        blackSnrAnalysisDialog->setLiveHistory(tbcSource.getLiveSnrHistory(true, MAX_POINTS), frames);
    }
    if (whiteSnrAnalysisDialog->isVisible()) {
        whiteSnrAnalysisDialog->setLiveHistory(tbcSource.getLiveSnrHistory(false, MAX_POINTS), frames);
    }
}

// A file: measure the SNR from the video (instead of using the metadata), or go back to the metadata
void MainWindow::setFileSnrMeasuring(bool measuring)
{
    if (measuring) {
        tbcSource.startFileSnr();
        snrFileTimer->start();
        updateFileSnrGraphs();
    } else {
        snrFileTimer->stop();
        tbcSource.stopFileSnr();
        // The graphs of the metadata again
        updateGraphs();
        for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
            bar->setFileSource(false);
        }
    }
}

// While the SNR of a file is measured from its video, the graphs show how far it has got
void MainWindow::updateFileSnrGraphs()
{
    if (!tbcSource.getFileSnrActive()) return;
    const qint32 progress = tbcSource.getFileSnrProgressPercent();
    const qint32 frames = tbcSource.getNumberOfFrames();
    static constexpr qint32 MAX_POINTS = 4000;

    for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
        bar->setFileSource(true, progress);
    }
    if (blackSnrAnalysisDialog->isVisible()) {
        blackSnrAnalysisDialog->setLiveHistory(tbcSource.getFileSnrHistory(true, MAX_POINTS), frames);
    }
    if (whiteSnrAnalysisDialog->isVisible()) {
        whiteSnrAnalysisDialog->setLiveHistory(tbcSource.getFileSnrHistory(false, MAX_POINTS), frames);
    }
}

// A live input can not be sought while it plays: the navigation controls are off then. Paused,
// they go through the buffer, which holds the last few seconds.
void MainWindow::updateLiveControls()
{
    if (!tbcSource.getIsLive() || !tbcSource.getIsSourceLoaded()) return;

    const bool isFieldView = tbcSource.getFieldViewEnabled();
    const qint32 firstFrame = tbcSource.getFirstAvailableFrame();
    const qint32 first = isFieldView ? 2 * firstFrame - 1 : firstFrame;
    const qint32 last = isFieldView ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames();
    const bool navigable = !playback->isPlaying();

    {
        // Moving the ends of the controls must not look like the user seeking
        const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
        const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
        const qint32 position = isFieldView ? currentFieldNumber : currentFrameNumber;
        if (navigable) {
            // Paused: the buffer, which is what can be reached, with the handle where the picture is
            const qint32 high = qMax(first, last);
            ui->posNumberSpinBox->setRange(first, high);
            ui->posHorizontalSlider->setRange(first, high);
            ui->posHorizontalSlider->setPageStep(qMax(1, (high - first) / 100));
            ui->posNumberSpinBox->setValue(qBound(first, position, high));
            ui->posHorizontalSlider->setValue(qBound(first, position, high));
        } else {
            // Playing: a live input has nothing to seek, so the slider does not move at all: it is
            // left at its end (a range that grew and a position that moved on, a frame apart, made the
            // handle step left and right). The frame number goes on counting.
            ui->posHorizontalSlider->setRange(0, 1);
            ui->posHorizontalSlider->setValue(1);
            ui->posNumberSpinBox->setRange(1, qMax(1, last));
        }
    }

    ui->posNumberSpinBox->setEnabled(navigable);
    ui->posHorizontalSlider->setEnabled(navigable);
    ui->previousPushButton->setEnabled(navigable);
    ui->nextPushButton->setEnabled(navigable);
    ui->startPushButton->setEnabled(navigable);
    // (the end button goes back to the live edge, which is what is wanted when behind)
    ui->endPushButton->setEnabled(true);
}

// Video playback ------------------------------------------------------------------------------------------------------

// Create the play button, the Playback menu and the clock
void MainWindow::setupPlayback()
{
    playback = new PlaybackController(this);
    renderer = new PlaybackRenderer(this);
    liveTimer = new QTimer(this);
    liveTimer->setInterval(33);
    connect(liveTimer, &QTimer::timeout, this, &MainWindow::updateLive);
    connect(renderer, &PlaybackRenderer::imageReady, this, &MainWindow::showRenderedImage);
    connect(playback, &PlaybackController::positionDue, this, &MainWindow::playbackPositionDue);
    connect(playback, &PlaybackController::stateChanged, this, &MainWindow::updatePlaybackState);
    connect(playback, &PlaybackController::looped, this, &MainWindow::playbackLooped);

    // The play button sits beside the frame stepping buttons
    playPushButton = new QPushButton(this);
    playPushButton->setMinimumSize(30, 30);
    playPushButton->setIconSize(QSize(24, 24));
    playPushButton->setFocusPolicy(Qt::NoFocus);
    playPushButton->setToolTip(tr("Play / pause (Space)"));
    connect(playPushButton, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    const qint32 nextIndex = ui->horizontalLayout_3->indexOf(ui->nextPushButton);
    ui->horizontalLayout_3->insertWidget(nextIndex, playPushButton);

    QMenu *menu = new QMenu(tr("&Playback"), this);
    playbackMenu = menu;
    ui->menuBar->insertMenu(ui->menuWindow->menuAction(), menu);

    actionPlayPause = menu->addAction(tr("&Play"));
    actionPlayPause->setShortcut(Qt::Key_Space);
    actionPlayPause->setShortcutContext(Qt::ApplicationShortcut);
    connect(actionPlayPause, &QAction::triggered, this, &MainWindow::togglePlayback);

    actionLoop = menu->addAction(tr("&Loop"));
    actionLoop->setCheckable(true);
    connect(actionLoop, &QAction::toggled, playback, &PlaybackController::setLoop);

    // A trick of the playback: in the frame view the images go at the rate of the fields, each the weave of a field and the next
    // (fields 1 and 2, then 2 and 3, then 3 and 4...). Off by default: a source with 2:2 pulldown (film) shows two fields of
    // different times in such a weave.
    actionWeave = menu->addAction(tr("Frame view at field rate (weave)"));
    actionWeave->setCheckable(true);
    actionWeave->setToolTip(tr("While playing, show a frame made of each field and the next, at the field rate. Not for sources with 2:2 pulldown."));
    // (kept from one session to the next)
    actionWeave->setChecked(configuration.playbackValue("weave", false).toBool());
    connect(actionWeave, &QAction::toggled, this, [this](bool on) { configuration.setPlaybackValue("weave", on); configuration.writeConfiguration(); });

    // The picture stop codes of a CAV disc (what a player does: it stops at one, and goes on when it is told to play)
    actionStopCodes = menu->addAction(tr("Pause at CAV &stop codes"));
    actionStopCodes->setObjectName("actionStopCodes");
    actionStopCodes->setCheckable(true);
    actionStopCodes->setToolTip(tr("A CAV disc can have a picture stop code on a frame: the playback pauses there, and goes on when it is played again."));
    actionStopCodes->setChecked(configuration.playbackValue("stopCodes", true).toBool());
    connect(actionStopCodes, &QAction::toggled, this, [this](bool on) { configuration.setPlaybackValue("stopCodes", on); configuration.writeConfiguration(); });

    // Speed relative to the real speed of the video
    speedSeparator = menu->addSeparator();
    speedActions = new QActionGroup(this);
    for (const double speed: {0.25, 0.5, 1.0, 2.0, 4.0}) {
        QAction *action = menu->addAction(tr("%1x speed").arg(speed));
        action->setCheckable(true);
        action->setChecked(speed == 1.0);
        if (speed == 1.0) speedNormalAction = action;
        speedActions->addAction(action);
        action->setData(speed);
        connect(action, &QAction::triggered, this, [this, speed]() { applyPlaybackSpeed(speed); });
    }

    updatePlaybackState(false);

#ifdef LDDECODE_HAVE_AUDIO
    setupAudio();
#endif
}

// Draw the play or pause symbol in the theme's play button colour (icons from the style are black)
void MainWindow::updatePlayButtonIcon()
{
    const qreal ratio = devicePixelRatioF();
    const qint32 size = 24;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(playButtonColour());
    if (playbackShowsPause) {
        painter.drawRoundedRect(QRectF(6, 5, 4.5, 14), 1, 1);
        painter.drawRoundedRect(QRectF(13.5, 5, 4.5, 14), 1, 1);
    } else {
        const QPointF triangle[3] = { QPointF(7, 4.5), QPointF(7, 19.5), QPointF(19.5, 12) };
        painter.drawPolygon(triangle, 3);
    }
    painter.end();

    playPushButton->setIcon(QIcon(pixmap));
}

void MainWindow::updatePlaybackState(bool playing)
{
    playbackShowsPause = playing;
    if (playing) lastFrameHadStop = tbcSource.getIsFrameVbiValid() && tbcSource.getFrameVbi().picStop;
    if (!playing && weaveActive) {
        // The weaves were a trick of the playback: what stays on show is the real frame
        weaveActive = false;
        tbcSource.setWeavePlayback(false);
        if (tbcSource.getIsSourceLoaded()) {
            tbcSource.clearPrerendered();
            showImage();
        }
    }
#ifdef LDDECODE_HAVE_AUDIO
    if (!playing) audioStop();
#endif
    updatePlayButtonIcon();
    actionPlayPause->setText(playing ? tr("&Pause") : tr("&Play"));
    // A slow or fast speed that the remote set is not kept for the next time the playback begins (the speed chosen in the Playback menu is)
    if (!playing && speedFromRemote) {
        speedFromRemote = false;
        applyPlaybackSpeed(1.0);
    }
    updateRemote();
    updateLiveControls();

    // Say if the machine could not keep up, so a jerky video is not mistaken for a bad capture
    if (!playing) renderer->stop();
    // A live input: pausing freezes the frames around the one shown (the input goes on filling the
    // other buffer), playing lets them go once they have been played through
    if (tbcSource.getIsLive()) {
        if (playing) tbcSource.unfreezeLive();
        else tbcSource.freezeLive(currentFrameNumber);
        updateLiveControls();
    }
    if (!playing && playback->getSkippedPositions() > 0) {
        statusBar()->showMessage(tr("Playback skipped %1 %2 that could not be shown in time")
                                 .arg(playback->getSkippedPositions())
                                 .arg(tbcSource.getFieldViewEnabled() ? tr("fields") : tr("frames")), 8000);
    }
}

void MainWindow::togglePlayback()
{
    if (playback->isPlaying()) {
        pausePlayback();
        return;
    }
    if (!tbcSource.getIsSourceLoaded()) return;

    // The clock counts frames, or fields in field view, where it has twice as many
    const bool isFieldView = tbcSource.getFieldViewEnabled();
    // (the weave of fields: the frame view, no deinterlacer, a file: the clock counts fields, and the images are pairs of them)
    weaveActive = actionWeave->isChecked() && tbcSource.getViewMode() == TbcSource::ViewMode::FRAME_VIEW && !deinterlacedView
                  && tbcSource.getNumberOfFields() > 2;
    tbcSource.setWeavePlayback(weaveActive);
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    playback->setRate(isFieldView || weaveActive ? 2.0 * frameRate : frameRate);
    const bool isLive = tbcSource.getIsLive();
    if (isLive) tbcSource.unfreezeLive();
    if (isLive) updateLive();
    if (isLive && !tbcSource.getIsLiveEnded()) {
        // Playing again goes back to the live edge, not on from where it was stopped: what was frozen
        // was only there to go through, and is let go of
        tbcSource.dropLiveProtection();
        setCurrentFrame(qMax(1, tbcSource.getNumberOfFrames() - 1));
        const qint32 shown = isFieldView ? currentFieldNumber : currentFrameNumber;
        const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
        const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
        ui->posNumberSpinBox->setValue(shown);
        ui->posHorizontalSlider->setValue(shown);
    }
    qint32 lastPosition = weaveActive ? tbcSource.getNumberOfFields() - 1 : (isFieldView ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames());
    qint32 firstPosition = isLive ? (isFieldView || weaveActive ? 2 * tbcSource.getFirstAvailableFrame() - 1 : tbcSource.getFirstAvailableFrame()) : 1;
    // A part that is repeated (A and B of the remote): that is the range, and it loops
    const bool repeating = repeatStartFrame > 0 && repeatEndFrame > repeatStartFrame && !isLive;
    if (repeating) {
        const bool byFields = isFieldView || weaveActive;
        firstPosition = qMax(firstPosition, byFields ? 2 * repeatStartFrame - 1 : repeatStartFrame);
        lastPosition = qMin(lastPosition, byFields ? 2 * repeatEndFrame - (weaveActive ? 1 : 0) : repeatEndFrame);
    }
    playback->setLoop(repeating || actionLoop->isChecked());
    playback->setLive(isLive, isLive && tbcSource.getIsLiveEnded());
    playback->setRange(firstPosition, lastPosition);
    playbackFirstPosition = firstPosition;
    playbackLastPosition = lastPosition;

    // Start decoding ahead; the clock starts once the first image is ready
    playbackStartPosition = isFieldView || weaveActive ? currentFieldNumber : currentFrameNumber;
    // (a live stream plays on at its end: there is more to come)
    if (playbackStartPosition >= lastPosition && !isLive) playbackStartPosition = firstPosition;
    playbackStartPosition = qMax(playbackStartPosition, firstPosition);
    playbackDue = playbackStartPosition;
    renderGeneration = tbcSource.getSettingsGeneration();
    renderer->start(tbcSource.getRenderSettings(), renderGeneration, playbackStartPosition, lastPosition,
                    isLive ? &tbcSource : nullptr);
    // The clock starts at once: the picture on show stays until the first decoded one is ready,
    // rather than the button waiting for it
    firstImageShown = false;
#ifdef LDDECODE_HAVE_AUDIO
    audioStart(playbackStartPosition);
#endif
    playback->play(playbackStartPosition);
}

// Looping: the clock went back to the first position. The decoders are told from there (they had gone to the end, and give the
// positions that are due in order), and the sound begins again with the video.
void MainWindow::playbackLooped()
{
    playbackDue = playbackFirstPosition;
    renderer->start(tbcSource.getRenderSettings(), renderGeneration, playbackFirstPosition, playbackLastPosition, nullptr);
    firstImageShown = false;
#ifdef LDDECODE_HAVE_AUDIO
    if (audio && audio->isPlaying()) {
        audioStop();
        audioStart(playbackFirstPosition);
    }
#endif
}

// Stop playing, as when the user takes over (does nothing if not playing)
void MainWindow::pausePlayback()
{
    const bool wasActive = playback->isPlaying();
    playback->pause();
    renderer->stop();
    if (wasActive) updatePlaybackState(false);
}

// The clock says a position is due: let the decoders know, and show what is ready
void MainWindow::playbackPositionDue(qint32 position)
{
    playbackDue = position;
    renderer->setDue(position);
    showRenderedImage();
}

// Show the newest image that is ready for a position that has fallen due
void MainWindow::showRenderedImage()
{
    if (!playback->isPlaying()) return;

    if (renderer->hasFailed()) {
        pausePlayback();
        QMessageBox::warning(this, tr("Playback"), tr("The video could not be opened for playback."));
        return;
    }

    // The settings were changed while playing: decode again from the present
    if (tbcSource.getSettingsGeneration() != renderGeneration) {
        renderGeneration = tbcSource.getSettingsGeneration();
        renderer->setSettings(tbcSource.getRenderSettings(), renderGeneration);
        return;
    }

    qint32 position;
    QImage image;
    if (!renderer->take(playbackDue, position, image)) return;

    // The first image can be a few frames late (the decoders were just started): that is not
    // the machine failing to keep up
    if (!firstImageShown) {
        firstImageShown = true;
        playback->clearSkipped();
    }

    if (weaveActive) {
        // A weave of the fields position and position + 1: shown for the frame they are in, which two of them share
        const qint32 frame = (position + 1) / 2;
        tbcSource.setPrerenderedImage(frame, image);
        currentFrameNumber = frame;
        currentFieldNumber = position;
        sanitizeCurrentPosition();
        showImage();
        fieldNumberStatus.setText(QString(" - Fields: %1/%2 (weave)").arg(position).arg(position + 1));
#ifdef LDDECODE_HAVE_AUDIO
        updateAudioPlayhead();
#endif
        const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
        const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
        ui->posNumberSpinBox->setValue(frame);
        ui->posHorizontalSlider->setValue(frame);
        checkStopCode();
        return;
    }
    tbcSource.setPrerenderedImage(position, image);
    const bool isFieldView = tbcSource.getFieldViewEnabled();
    if (isFieldView) setCurrentField(position); else setCurrentFrame(position);

    // Move the controls along without making them seek
    const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
    const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
    ui->posNumberSpinBox->setValue(position);
    // (a live input: the slider stays where it is, at its end, see updateLiveControls)
    if (!tbcSource.getIsLive()) ui->posHorizontalSlider->setValue(position);
    checkStopCode();
}

// A picture stop code (CAV): the playback pauses on the frame that has one, the first of a run of frames that have it. It goes on from there when it is
// played again: the frame it begins on is not a stop to be obeyed (see updatePlaybackState). Not while scanning or going backward.
void MainWindow::checkStopCode()
{
    if (!actionStopCodes || !actionStopCodes->isChecked() || !playback->isPlaying()) return;
    const bool stop = tbcSource.getIsFrameVbiValid() && tbcSource.getFrameVbi().picStop;
    const bool begins = stop && !lastFrameHadStop;
    lastFrameHadStop = stop;
    if (!begins) return;
    pausePlayback();
    const VbiDecoder::Vbi vbi = tbcSource.getFrameVbi();
    statusBar()->showMessage(vbi.picNo != -1 ? tr("Picture stop code at picture %1 (frame %2): paused").arg(vbi.picNo).arg(currentFrameNumber)
                                             : tr("Picture stop code at frame %1: paused").arg(currentFrameNumber), 8000);
}

// The row of media controls and options is crowded: when it has not the room for the field order
// button, which is the least used, that is left out (View > Reverse field order does the same)
void MainWindow::updateFieldOrderVisibility()
{
    QLayout *row = ui->horizontalLayout_3;
    const QMargins margins = ui->verticalLayout_3->contentsMargins();
    const int available = ui->mediaControl_frame->width() - margins.left() - margins.right();

    // What the row needs with the button in it: its preferred width, without ever shrinking
    // the text of the other buttons
    int needed = row->sizeHint().width();
    if (!ui->fieldOrderPushButton->isVisible()) needed += ui->fieldOrderPushButton->sizeHint().width() + row->spacing();
    ui->fieldOrderPushButton->setVisible(needed <= available);
}

// Update GUI methods for when TBC source files are loaded and unloaded -----------------------------------------------

// Enable or disable all the GUI controls
void MainWindow::setGuiEnabled(bool enabled)
{
    // Enable the field/frame controls
    ui->posNumberSpinBox->setEnabled(enabled);
    ui->previousPushButton->setEnabled(enabled);
    ui->nextPushButton->setEnabled(enabled);
    ui->startPushButton->setEnabled(enabled);
    ui->endPushButton->setEnabled(enabled);
    ui->posHorizontalSlider->setEnabled(enabled);
    ui->mediaControl_frame->setEnabled(enabled);
    playPushButton->setEnabled(enabled);
    actionPlayPause->setEnabled(enabled);
    if (!enabled) pausePlayback();

    // Enable menu options
    ui->actionLine_scope->setEnabled(enabled);
    ui->actionVectorscope->setEnabled(enabled);
    ui->actionVBI->setEnabled(enabled);
    ui->actionNTSC->setEnabled(enabled);
    ui->actionVideo_metadata->setEnabled(enabled);
    ui->actionVITS_Metrics->setEnabled(enabled);
    ui->actionZoom_In->setEnabled(enabled);
    ui->actionZoom_Out->setEnabled(enabled);
    ui->actionZoom_1x->setEnabled(enabled);
    ui->actionZoom_2x->setEnabled(enabled);
    ui->actionZoom_3x->setEnabled(enabled);
    ui->actionDropout_analysis->setEnabled(enabled);
    ui->actionVisible_Dropout_analysis->setEnabled(enabled);
    ui->actionSNR_analysis->setEnabled(enabled); // Black SNR
    ui->actionWhite_SNR_analysis->setEnabled(enabled);
    ui->actionSave_frame_as_PNG->setEnabled(enabled);
    ui->actionClosed_Captions->setEnabled(enabled);
    ui->actionVideo_parameters->setEnabled(enabled);
    ui->actionChroma_decoder_configuration->setEnabled(enabled);
    ui->actionReload_TBC->setEnabled(enabled);
    actionFieldOrder->setEnabled(enabled);

    // "Save Metadata" should be disabled by default
    ui->actionSave_Metadata->setEnabled(false);

    // Set zoom button states
    ui->zoomInPushButton->setEnabled(enabled);
    ui->zoomOutPushButton->setEnabled(enabled);
    ui->originalSizePushButton->setEnabled(enabled);
}

TbcSource& MainWindow::getTbcSource()
{
	return this->tbcSource;
}

void MainWindow::resetGui()
{
    deinterlacedView = false;
    updateVideoFilterGraph();
    ui->posNumberSpinBox->setMinimum(1);
    ui->posHorizontalSlider->setMinimum(1);

    ui->posNumberSpinBox->setValue(1);
    ui->posHorizontalSlider->setValue(1);
   (this->width() >= 930) ? ui->dropoutsPushButton->setText(tr("Dropouts Off")) : ui->dropoutsPushButton->setText(tr("Drop N"));

    setViewValues();

    // Allow the next and previous frame buttons to auto-repeat
    ui->previousPushButton->setAutoRepeat(true);
    ui->previousPushButton->setAutoRepeatDelay(500);
    ui->previousPushButton->setAutoRepeatInterval(1);
    ui->nextPushButton->setAutoRepeat(true);
    ui->nextPushButton->setAutoRepeatDelay(500);
    ui->nextPushButton->setAutoRepeatInterval(1);

    // Set option button states
    ui->videoPushButton->setText(tr("Source"));
    displayAspectRatio = true;
    updateAspectPushButton();
    updateSourcesPushButton();
    if (this->width() > 1000)
		ui->fieldOrderPushButton->setText(tr("Normal Field-order"));
	else if (this->width() >= 930)
		ui->fieldOrderPushButton->setText(tr("Normal order"));
	else
		ui->fieldOrderPushButton->setText(tr("Normal"));

    // Zoom button options
    ui->zoomInPushButton->setAutoRepeat(true);
    ui->zoomInPushButton->setAutoRepeatDelay(500);
    ui->zoomInPushButton->setAutoRepeatInterval(100);
    ui->zoomOutPushButton->setAutoRepeat(true);
    ui->zoomOutPushButton->setAutoRepeatDelay(500);
    ui->zoomOutPushButton->setAutoRepeatInterval(100);

    // Initialize field stretch to 2:1 by default
    tbcSource.setStretchField(true);

    // Update the video parameters dialogue
    videoParametersDialog->setVideoParameters(tbcSource.getVideoParameters());

    // Update the chroma decoder configuration dialogue
    chromaDecoderConfigDialog->setConfiguration(tbcSource.getSystem(), tbcSource.getPalConfiguration(),
                                                tbcSource.getNtscConfiguration(),
                                                tbcSource.getMonoConfiguration(),
                                                tbcSource.getSourceMode(),
												true,//set to true because the chroma decoder is already init
												tbcSource.getOutputConfiguration());
}

// Method to update the GUI when a file is loaded
void MainWindow::updateGuiLoaded()
{
    updateChapterMarks();
    // (what the remote kept of another capture is of no use; a change of the view keeps it)
    if (remoteSource != tbcSource.getCurrentSourceFilename()) {
        remoteSource = tbcSource.getCurrentSourceFilename();
        memoryFrame = repeatStartFrame = repeatEndFrame = -1;
    }
    keypad.clear();
    keypad.setMode(tbcSource.hasChapters() ? SearchMode::Chapter : (tbcSource.hasPictureNumbers() ? SearchMode::Picture : (tbcSource.hasTimes() ? SearchMode::Time : SearchMode::Chapter)));
    refreshChapterList();
    endStill();
    updateRemote();
    // Enable the GUI controls
    setGuiEnabled(true);
#ifdef LDDECODE_HAVE_AUDIO
    loadAudio();
#endif

    // Where the SNR in the graphs comes from
    for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
        if (tbcSource.getIsLive()) bar->setLiveSource(tbcSource.getLiveSnrEnabled(), tbcSource.getLiveSnrIsWholeHistory());
        else bar->setFileSource(tbcSource.getFileSnrActive(), tbcSource.getFileSnrProgressPercent());
    }
    {
        const QSignalBlocker blocker(actionFieldOrder);
        actionFieldOrder->setChecked(tbcSource.getFieldOrder());
    }

    // Update the status bar
    QString statusText;
	if(tbcSource.getVideoParameters().tapeFormat != "") {
		statusText += (tbcSource.getVideoParameters().tapeFormat + " ");
	}
    statusText += tbcSource.getSystemDescription();
    statusText += tbcSource.getIsStack() ? tr(" stack (%1) loaded with ").arg(tbcSource.getStackDescription()) : tr(" source loaded with ");

    if (tbcSource.getIsLive()) {
        statusText = tr("Live input (%1)").arg(tbcSource.getSystemDescription());
    } else if (tbcSource.getFieldViewEnabled()) {
        statusText += QString::number(tbcSource.getNumberOfFields());
        statusText += tr(" fields available");
    } else {
        statusText += QString::number(tbcSource.getNumberOfFrames());
        statusText += tr(" sequential frames available");
    }

    // Which metadata was loaded: SQLite (.tbc.db) or JSON (.tbc.json)
    if (!tbcSource.getIsLive()) statusText += tr(" - Metadata: ") + tbcSource.getMetadataFormatName();

    sourceVideoStatus.setText(statusText);
    sourceVideoStatus.setToolTip(tbcSource.getCurrentMetadataFilename());

    // Update source mode button
    updateSourcesPushButton();

    // Load and show the current image
    showImage();

    // Update the video parameters dialogue
    videoParametersDialog->setVideoParameters(tbcSource.getVideoParameters());

    // Update the chroma decoder configuration dialogue
        chromaDecoderConfigDialog->setConfiguration(tbcSource.getSystem(), tbcSource.getPalConfiguration(),
                                                tbcSource.getNtscConfiguration(),
                                                tbcSource.getMonoConfiguration(),
                                                tbcSource.getSourceMode(),
												false,//set to false to init the chroma decoder selection
												tbcSource.getOutputConfiguration());

    // Ensure the busy dialogue is hidden
    busyDialog->hide();

    // Disable "Save Metadata", now we've loaded the metadata into the GUI
    ui->actionSave_Metadata->setEnabled(false);

    updateLiveControls();

	//resize the windows to fit the content in full screen
	MainWindow::resize_on_aspect();
	
	// If resizeFrameWithWindow is enabled, resize frame to fit current window
	if (resizeFrameWithWindow) {
		resizeTimer->start();
	}
}

// Method to update the GUI when a file is unloaded
void MainWindow::updateGuiUnloaded()
{
    updateChapterMarks();
    memoryFrame = repeatStartFrame = repeatEndFrame = -1;
    keypad.clear();
    endStill();
    updateRemote();
#ifdef LDDECODE_HAVE_AUDIO
    unloadAudio();
#endif
    resetContext();
    snrFileTimer->stop();
    liveTimer->stop();
    setPlaybackOptionsVisible(true);
    // Disable the GUI controls
    setGuiEnabled(false);

    // Update the current field/frame number
    setCurrentFrame(1);
    ui->posNumberSpinBox->setValue(1);
    ui->posHorizontalSlider->setValue(1);

    // Set the window title
    this->setWindowTitle(tr("ld-analyse"));

    // Set the status bar text
    sourceVideoStatus.setText(tr("No source video file loaded"));
    fieldNumberStatus.setText(tr("- Fields: ./."));
    vbiStatus.hide();
    timeCodeStatus.hide();

    // Set option button states
    ui->videoPushButton->setText(tr("Source"));
    (this->width() >= 930) ? ui->dropoutsPushButton->setText(tr("Dropouts Off")) : ui->dropoutsPushButton->setText(tr("Drop N"));
    displayAspectRatio = false;
    updateAspectPushButton();
    updateSourcesPushButton();
    if (this->width() > 1000)
		ui->fieldOrderPushButton->setText(tr("Normal Field-order"));
	else if (this->width() >= 930)
		ui->fieldOrderPushButton->setText(tr("Normal order"));
	else
		ui->fieldOrderPushButton->setText(tr("Normal"));

    // Hide the displayed image
    hideImage();

    // Hide graphs
    blackSnrAnalysisDialog->hide();
    whiteSnrAnalysisDialog->hide();
    dropoutAnalysisDialog->hide();

    // Hide configuration dialogues
    videoParametersDialog->hide();
    chromaDecoderConfigDialog->hide();
}

// Update the aspect ratio button
void MainWindow::updateAspectPushButton()
{
    if (!displayAspectRatio) {
        ui->aspectPushButton->setText(tr("SAR 1:1"));
    } else if (tbcSource.getIsWidescreen()) {
        (this->width() >= 1020) ? ui->aspectPushButton->setText(tr("DAR 16:9")) : ui->aspectPushButton->setText(tr("16:9"));
    } else {
        ui->aspectPushButton->setText(tr("DAR 4:3"));
    }
}

// Update the source selection button
void MainWindow::updateSourcesPushButton()
{
	if (this->width() >= 930)
	{
		switch (tbcSource.getSourceMode()) {
		case TbcSource::ONE_SOURCE:
			ui->sourcesPushButton->setText(tr("One Source"));
			break;
		case TbcSource::LUMA_SOURCE:
			ui->sourcesPushButton->setText(tr("Y Source"));
			break;
		case TbcSource::CHROMA_SOURCE:
			ui->sourcesPushButton->setText(tr("C Source"));
			break;
		case TbcSource::BOTH_SOURCES:
			ui->sourcesPushButton->setText(tr("Y/C Sources"));
			break;
		}
	}
	else
	{
		switch (tbcSource.getSourceMode()) {
		case TbcSource::ONE_SOURCE:
			ui->sourcesPushButton->setText(Cvbs::isDataFileName(tbcSource.getCurrentSourceFilename()) ? tr(".CVBS") : tr(".TBC"));
			break;
		case TbcSource::LUMA_SOURCE:
			ui->sourcesPushButton->setText(tr("Y"));
			break;
		case TbcSource::CHROMA_SOURCE:
			ui->sourcesPushButton->setText(tr("C"));
			break;
		case TbcSource::BOTH_SOURCES:
			ui->sourcesPushButton->setText(tr("Y/C"));
			break;
		}
	}
	chromaDecoderConfigDialog->updateSourceMode(tbcSource.getSourceMode());
}

// Frame display methods ----------------------------------------------------------------------------------------------

// Update the UI and displays when currentFrameNumber or currentFieldNumber has changed
void MainWindow::showImage()
{
    tbcSource.load(currentFrameNumber, currentFieldNumber);

    // Show the field numbers
    if (tbcSource.getViewMode() == TbcSource::ViewMode::FIELD_VIEW) {
        fieldNumberStatus.setText(QString(" - Field:  %1 (%2/2) - Frame: %3")
                                  .arg(currentFieldNumber)
                                  .arg(currentFieldNumber % 2 ? 1 : 2)
                                  .arg(currentFrameNumber));
    } else {
        fieldNumberStatus.setText(QString(" - Fields: %1/%2")
                                  .arg(tbcSource.getFirstFieldNumber())
                                  .arg(tbcSource.getSecondFieldNumber()));
    }

    // Show VBI position in the status bar, if available
    if (tbcSource.getIsFrameVbiValid()) {
        VbiDecoder::Vbi vbi = tbcSource.getFrameVbi();
        if (vbi.clvHr != -1) {
            vbiStatus.setText(QString(" -  CLV time code: %1:%2:%3")
                                  .arg(vbi.clvHr, 2, 10, QChar('0'))
                                  .arg(vbi.clvMin, 2, 10, QChar('0'))
                                  .arg(vbi.clvSec, 2, 10, QChar('0')));
            vbiStatus.show();
        } else if (vbi.picNo != -1) {
            vbiStatus.setText(QString(" -  CAV picture number: %1")
                                  .arg(vbi.picNo, 5, 10, QChar('0')));
            vbiStatus.show();
        } else {
            vbiStatus.hide();
        }
    } else {
        vbiStatus.hide();
    }

    // Show timecode in the status bar, if available
    if (tbcSource.getIsFrameVitcValid()) {
        // Use ; rather than : if the drop flag is set (as ffmpeg does)
        VitcDecoder::Vitc vitc = tbcSource.getFrameVitc();
        timeCodeStatus.setText(QString(" -  VITC time code: %1:%2:%3%4%5")
                                   .arg(vitc.hour, 2, 10, QChar('0'))
                                   .arg(vitc.minute, 2, 10, QChar('0'))
                                   .arg(vitc.second, 2, 10, QChar('0'))
                                   .arg(vitc.isDropFrame ? QChar(';') : QChar(':'))
                                   .arg(vitc.frame, 2, 10, QChar('0')));
        timeCodeStatus.show();
    } else {
        timeCodeStatus.hide();
    }

    // If there are dropouts in the frame, highlight the show dropouts button
    if (tbcSource.getIsDropoutPresent()) {
        // Light grey on a light theme (as before themes existed); on a dark
        // one a lighter shade of the button, so it stands out without
        // becoming a light patch with unreadable light text
        QPalette tempPalette = buttonPalette;
        const QColor highlightColour = isDarkTheme ? buttonPalette.color(QPalette::Button).lighter(170)
                                                   : QColor(Qt::lightGray);
        tempPalette.setColor(QPalette::Button, highlightColour);
        ui->dropoutsPushButton->setAutoFillBackground(true);
        ui->dropoutsPushButton->setPalette(tempPalette);
        ui->dropoutsPushButton->update();
    } else {
        ui->dropoutsPushButton->setAutoFillBackground(true);
        ui->dropoutsPushButton->setPalette(buttonPalette);
        ui->dropoutsPushButton->update();
    }

    // Update the VBI dialogue
    if (vbiDialog->isVisible()) {
        vbiDialog->updateVbi(tbcSource.getFrameVbi(), tbcSource.getIsFrameVbiValid());
        vbiDialog->updateVideoId(tbcSource.getFrameVideoId(), tbcSource.getIsFrameVideoIdValid());
    }

    // Add the QImage to the QLabel in the dialogue
    ui->imageViewerLabel->clear();
    ui->imageViewerLabel->setScaledContents(false);
    ui->imageViewerLabel->setAlignment(Qt::AlignCenter);

    // Update the field/frame image
    updateImage();
    updateInfoPanel();
    updateSeekPanel();

    // Update the closed caption dialog
    closedCaptionDialog->addData(currentFrameNumber, tbcSource.getCcData0(), tbcSource.getCcData1());

    // QT Bug workaround for some macOS versions
    #if defined(Q_OS_MACOS)
    	repaint();
    #endif
}

// Redraw all the GUI elements that depend on the decoded field/frame
void MainWindow::updateImage()
{
    // Update the image viewer
    updateImageViewer();

    // If the scope dialogues are open, update them
    if (oscilloscopeDialog->isVisible()) {
        updateOscilloscopeDialogue();
    }
    if (vectorscopeDialog->isVisible()) {
        updateVectorscopeDialogue();
    }
}

// In the full screen the picture is its active area: what the lines and the width of the video parameters say, without the blanking
QImage MainWindow::activeAreaOf(const QImage &image)
{
    if (!fullScreenOn || image.isNull() || !tbcSource.getIsSourceLoaded()) return image;
    const LdDecodeMetaData::VideoParameters &parameters = tbcSource.getVideoParameters();
    // (the field view that is stretched, as it is by default, is the size of a frame: with its lines doubled, and the lines of the other field one lower
    // -- the bob -- it has the lines of a frame)
    const bool field = tbcSource.getFieldViewEnabled() && !tbcSource.getStretchField();
    const qint32 firstLine = field ? parameters.firstActiveFieldLine : parameters.firstActiveFrameLine;
    const qint32 lastLine = field ? parameters.lastActiveFieldLine : parameters.lastActiveFrameLine;
    const QRect area(parameters.activeVideoStart, firstLine, parameters.activeVideoEnd - parameters.activeVideoStart, lastLine - firstLine);
    // (a picture that is not of the size of the video, or parameters that are not known: it is shown whole)
    if (area.width() <= 0 || area.height() <= 0 || !image.rect().contains(area)) return image;
    return image.copy(area);
}

// The width a picture is shown at (times the scale): the aspect ratio that was chosen, the adjustment of the whole picture, or in the full
// screen the shape of the active area (4:3 or 16:9, a field being half a frame in height)
double MainWindow::displayedWidth(const QImage &image)
{
    if (!fullScreenOn) return image.width() + getAspectAdjustment();
    if (!displayAspectRatio) return image.width();
    const double ratio = tbcSource.getIsWidescreen() ? 16.0 / 9.0 : 4.0 / 3.0;
    return image.height() * (tbcSource.getFieldViewEnabled() && !tbcSource.getStretchField() ? 2 : 1) * ratio;
}

// Return the width adjustment for the current aspect mode
qint32 MainWindow::getAspectAdjustment() {
    // Using source aspect ratio? No adjustment
    if (!displayAspectRatio) return 0;

    if (tbcSource.getSystem() == PAL) {
        // 625 lines
        if (tbcSource.getIsWidescreen()) return 103; // 16:9
        else return -196; // 4:3
    } else {
        // 525 lines
        if (tbcSource.getIsWidescreen()) return 122; // 16:9
        else return -150; // 4:3
    }
}

// Redraw the viewer (for example, when scaleFactor has been changed)
void MainWindow::updateImageViewer()
{
    QImage image = activeAreaOf(tbcSource.getImage());

    if (!fullScreenOn && ui->mouseModePushButton->isChecked() && !image.isNull() && image.width() > 0 && image.height() > 0) {
        // Create a painter object
        QPainter imagePainter;
        imagePainter.begin(&image);

        // Draw lines to indicate the current scope position
        imagePainter.setPen(QColor(0, 255, 0, 127));
        imagePainter.drawLine(0, lastScopeLine - 1, tbcSource.getFrameWidth(), lastScopeLine - 1);
        imagePainter.drawLine(lastScopeDot, 0, lastScopeDot, tbcSource.getFrameHeight());

        // End the painter object
        imagePainter.end();
    }

    // The width it is shown at, for the aspect ratio that was chosen
    const int width = static_cast<int>(scaleFactor * displayedWidth(image));
    const int height = static_cast<int>(scaleFactor * image.height());

    // Through the video filters, which can size the picture to what is shown
    const bool showPicture = filterImage(image, width, height);

    QPixmap pixmap = QPixmap::fromImage(image);

    // Scale and apply the pixmap (only if it's valid)
    if (showPicture && !pixmap.isNull()) {
        ui->imageViewerLabel->setPixmap(pixmap.scaled(width, height,
                                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }

    // Update the current frame markers on the graphs
    blackSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
    whiteSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
    dropoutAnalysisDialog->updateFrameMarker(currentFrameNumber);
    visibleDropoutAnalysisDialog->updateFrameMarker(currentFrameNumber);

    // QT Bug workaround for some macOS versions
    #if defined(Q_OS_MACOS)
    	repaint();
    #endif
}

// Method to hide the current image
void MainWindow::hideImage()
{
    ui->imageViewerLabel->clear();
}

// Misc private methods -----------------------------------------------------------------------------------------------

// Load a TBC file based on the passed file name
void MainWindow::loadTbcFile(QString inputFileName)
{
    resetContext();
    pausePlayback();
    liveTimer->stop();
    renderer->close();
    // Update the GUI
    updateGuiUnloaded();

    // Close current source video (if loaded)
    if (tbcSource.getIsSourceLoaded()) tbcSource.unloadSource();

    // Load the source
    tbcSource.loadSource(inputFileName);

    // Note: loading continues in the background...
}

// Method to update the line oscilloscope based on the frame number and scan line
void MainWindow::updateOscilloscopeDialogue()
{
    // Update the oscilloscope dialogue
    oscilloscopeDialog->showTraceImage(tbcSource.getScanLineData(lastScopeLine),
                                       lastScopeDot, lastScopeLine - 1,
                                       tbcSource.getFrameWidth(), tbcSource.getFrameHeight(), tbcSource.getSourceMode() == TbcSource::SourceMode::BOTH_SOURCES);
}

// Method to update the vectorscope
void MainWindow::updateVectorscopeDialogue()
{
    // Update the vectorscope dialogue
    vectorscopeDialog->showTraceImage(tbcSource.getComponentFrame(), tbcSource.getVideoParameters(),
                                      tbcSource.getViewMode(), currentFieldNumber % 2);
}

// Method to set the view (field/frame) values
void MainWindow::setViewValues()
{
    qint32 currentNumber, maximum;
    QString buttonLabel, spinLabel;

	if (this->width() >= 930)
	{
		if (tbcSource.getFieldViewEnabled()) {
			currentNumber = currentFieldNumber;
			maximum = tbcSource.getNumberOfFields();
			spinLabel = QString("Field #:");
			buttonLabel = QString("Field View");
		} else {
			currentNumber = currentFrameNumber;
			maximum = tbcSource.getNumberOfFrames();
			spinLabel = QString("Frame #:");

			if (tbcSource.getSplitViewEnabled()) {
				buttonLabel = QString("Split View");
			} else if (deinterlacedView) {
				buttonLabel = QString("Deinterlaced View");
			} else {
				buttonLabel = QString("Frame View");
			}
		}
	}
	else
	{
		if (tbcSource.getFieldViewEnabled()) {
			currentNumber = currentFieldNumber;
			maximum = tbcSource.getNumberOfFields();
			spinLabel = QString("Field #:");
			buttonLabel = QString("Field");
		} else {
			currentNumber = currentFrameNumber;
			maximum = tbcSource.getNumberOfFrames();
			spinLabel = QString("Frame #:");

			if (tbcSource.getSplitViewEnabled()) {
				buttonLabel = QString("Split");
			} else if (deinterlacedView) {
				buttonLabel = QString("Deint");
			} else {
				buttonLabel = QString("Frame");
			}
		}
	}

    ui->posNumberSpinBox->setMaximum(maximum);
    ui->posNumberSpinBox->setValue(currentNumber);
    ui->posHorizontalSlider->setMaximum(maximum);
    ui->posHorizontalSlider->setPageStep(maximum / 100);
    ui->posHorizontalSlider->setValue(currentNumber);

    ui->viewPushButton->setText(buttonLabel);
    ui->posNumberSpinBoxLabel->setText(spinLabel);
    ui->posNumberSpinBoxLabel->repaint();
}

// Set the current frame, field is updated based on frame number
void MainWindow::setCurrentFrame(qint32 number)
{
    if (number == currentFrameNumber) return;

    currentFrameNumber = number;
    currentFieldNumber = (number * 2) - 1;

    sanitizeCurrentPosition();
    showImage();
#ifdef LDDECODE_HAVE_AUDIO
    updateAudioPlayhead();
#endif
}

// Set the current field, frame is updated based on field number
void MainWindow::setCurrentField(qint32 number)
{
    if (number == currentFieldNumber) return;

    currentFieldNumber = number;
    currentFrameNumber = std::ceil((double)number / 2);

    sanitizeCurrentPosition();
    showImage();
#ifdef LDDECODE_HAVE_AUDIO
    updateAudioPlayhead();
#endif
}

void MainWindow::sanitizeCurrentPosition()
{
    // A stream forgets its oldest frames
    if (tbcSource.getIsLive() && currentFrameNumber < tbcSource.getFirstAvailableFrame()) {
        currentFrameNumber = tbcSource.getFirstAvailableFrame();
        currentFieldNumber = 2 * currentFrameNumber - 1;
        // (the controls were moved to a frame that is gone: show where the video is)
        const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
        const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
        const qint32 shown = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
        ui->posNumberSpinBox->setValue(shown);
        ui->posHorizontalSlider->setValue(shown);
    }

    if (currentFrameNumber > tbcSource.getNumberOfFrames() || currentFieldNumber > tbcSource.getNumberOfFields()) {
        currentFrameNumber = tbcSource.getNumberOfFrames();
        currentFieldNumber = tbcSource.getNumberOfFields();
    }

    if (currentFrameNumber == 0)
    {
        currentFrameNumber = 1;
    }

    if (currentFieldNumber == 0) {
        currentFieldNumber = 1;
    }
}

// Menu bar signal handlers -------------------------------------------------------------------------------------------

void MainWindow::on_actionExit_triggered()
{
    tbcDebugStream() << "MainWindow::on_actionExit_triggered(): Called";

    // Quit the application
    qApp->quit();
}

// Load a TBC file based on the file selection from the GUI
void MainWindow::on_actionOpen_TBC_file_triggered()
{
    // A live input cannot be replaced by a file
    if (tbcSource.getIsLive()) return;
    pausePlayback();
    tbcDebugStream() << "MainWindow::on_actionOpen_TBC_file_triggered(): Called";

    QString inputFileName = QFileDialog::getOpenFileName(this,
                tr("Open TBC, CVBS, LDX or stack file"),
                configuration.getSourceDirectory()+tr("/ldsample.tbc"),
                tr("TBC, CVBS, LDX and stack files (*.tbc *.tbcy *.tbcc *.cvbs *.cvbsy *.cvbsc *.ldx *.stack);;LDX (*.ldx);;TBC output (*.tbc *.tbcy *.tbcc);;CVBS (*.cvbs *.cvbsy *.cvbsc);;Stacking session (*.stack);;All Files (*)"));

    // Was a filename specified?
    if (!inputFileName.isEmpty() && !inputFileName.isNull()) {
        lastFilename = inputFileName;
        loadTbcFile(inputFileName);
    }
}

// Reload the current TBC selection from the GUI
void MainWindow::on_actionReload_TBC_triggered()
{
    if (tbcSource.getIsLive()) return;
    // Reload the current TBC file
    if (!lastFilename.isEmpty() && !lastFilename.isNull()) {
        loadTbcFile(lastFilename);
    }
}

// Start saving the modified metadata
void MainWindow::on_actionSave_Metadata_triggered()
{
    tbcSource.saveSourceMetadata();

    // Saving continues in the background...
}

// Display the scan line oscilloscope view
void MainWindow::on_actionLine_scope_triggered()
{
    if (tbcSource.getIsSourceLoaded()) {
        // Show the oscilloscope dialogue for the selected scan-line
        updateOscilloscopeDialogue();
        oscilloscopeDialog->show();
    }
}

// Display the vectorscope view
void MainWindow::on_actionVectorscope_triggered()
{
    if (tbcSource.getIsSourceLoaded()) {
        // Show the vectorscope dialogue
        updateVectorscopeDialogue();
        vectorscopeDialog->show();
    }
}

// Show the about window
void MainWindow::on_actionAbout_ld_analyse_triggered()
{
    aboutDialog->show();
}

// Show the VBI window
void MainWindow::on_actionVBI_triggered()
{
    // Show the VBI dialogue
    vbiDialog->updateVbi(tbcSource.getFrameVbi(), tbcSource.getIsFrameVbiValid());
    vbiDialog->updateVideoId(tbcSource.getFrameVideoId(), tbcSource.getIsFrameVideoIdValid());
    vbiDialog->show();
}

// Show the drop out analysis graph
void MainWindow::on_actionDropout_analysis_triggered()
{
    // Show the dropout analysis dialogue
    dropoutAnalysisDialog->show();
}

// Show the visible drop out analysis graph
void MainWindow::on_actionVisible_Dropout_analysis_triggered()
{
    // Show the visible dropout analysis dialogue
    visibleDropoutAnalysisDialog->show();
}

// Show the Black SNR analysis graph
void MainWindow::on_actionSNR_analysis_triggered()
{
    // Show the black SNR analysis dialogue
    blackSnrAnalysisDialog->show();
}

// Show the White SNR analysis graph
void MainWindow::on_actionWhite_SNR_analysis_triggered()
{
    // Show the white SNR analysis dialogue
    whiteSnrAnalysisDialog->show();
}

// Save current frame as PNG
void MainWindow::on_actionSave_frame_as_PNG_triggered()
{
    tbcDebugStream() << "MainWindow::on_actionSave_frame_as_PNG_triggered(): Called";

    // Create a suggestion for the filename
    QString filenameSuggestion = configuration.getPngDirectory();

    switch (tbcSource.getViewMode()) {
        case TbcSource::ViewMode::FRAME_VIEW:
            filenameSuggestion += tr("/frame_");
            break;

        case TbcSource::ViewMode::SPLIT_VIEW:
            filenameSuggestion += tr("/fields_");
            break;

        case TbcSource::ViewMode::FIELD_VIEW:
            filenameSuggestion += tr("/field_");
            break;
    }

    if (tbcSource.getSystem() == PAL) filenameSuggestion += tr("pal_");
    else if (tbcSource.getSystem() == PAL_M) filenameSuggestion += tr("palm_");
    else filenameSuggestion += tr("ntsc_");

    if (!tbcSource.getChromaDecoder()) filenameSuggestion += tr("source_");
    else filenameSuggestion += tr("chroma_");

    if (displayAspectRatio) {
        if (tbcSource.getIsWidescreen()) filenameSuggestion += tr("ar169_");
        else filenameSuggestion += tr("ar43_");
    }

    if (tbcSource.getViewMode() == TbcSource::ViewMode::FIELD_VIEW) {
        filenameSuggestion += QString::number(currentFieldNumber);
    } else {
        filenameSuggestion += QString::number(currentFrameNumber);
    }

    filenameSuggestion += "_" + tbcSource.getCurrentSourceFilename().split("/").last() + tr(".png");

    QString pngFilename = QFileDialog::getSaveFileName(this,
                tr("Save PNG file"),
                filenameSuggestion,
                tr("PNG image (*.png);;All Files (*)"));

    // Was a filename specified?
    if (!pngFilename.isEmpty() && !pngFilename.isNull()) {
        // Save the current frame as a PNG
        tbcDebugStream() << "MainWindow::on_actionSave_frame_as_PNG_triggered(): Saving current frame as" << pngFilename;

        // Generate QImage for the current frame
        QImage imageToSave = tbcSource.getImage();

        // Get the aspect ratio adjustment, and scale the image if needed
        qint32 adjustment = getAspectAdjustment();
        if (adjustment != 0) {
            imageToSave = imageToSave.scaled((imageToSave.size().width() + adjustment),
                                             (imageToSave.size().height()),
                                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }

        // Save the QImage as PNG
        if (!imageToSave.save(pngFilename)) {
            tbcDebugStream() << "MainWindow::on_actionSave_frame_as_PNG_triggered(): Failed to save file as" << pngFilename;

            QMessageBox messageBox;
            messageBox.warning(this, tr("Warning"),tr("Could not save a PNG using the specified filename!"));
        }

        // Update the configuration for the PNG directory
        QFileInfo pngFileInfo(pngFilename);
        configuration.setPngDirectory(pngFileInfo.absolutePath());
        tbcDebugStream() << "MainWindow::on_actionSave_frame_as_PNG_triggered(): Setting PNG directory to:" << pngFileInfo.absolutePath();
        configuration.writeConfiguration();
    }
}

// Zoom in menu option
void MainWindow::on_actionZoom_In_triggered()
{
    on_zoomInPushButton_clicked();
	resizeForZoom();
}

// Zoom out menu option
void MainWindow::on_actionZoom_Out_triggered()
{
    on_zoomOutPushButton_clicked();
	resizeForZoom();
}

// Original size 1:1 zoom menu option
void MainWindow::on_actionZoom_1x_triggered()
{
    on_originalSizePushButton_clicked();
	resizeForZoom();
}

// 2:1 zoom menu option
void MainWindow::on_actionZoom_2x_triggered()
{
    scaleFactor = 2.0;
    updateImageViewer();
	resizeForZoom();
}

// 3:1 zoom menu option
void MainWindow::on_actionZoom_3x_triggered()
{
    scaleFactor = 3.0;
    updateImageViewer();
	resizeForZoom();
}

// Show closed captions
void MainWindow::on_actionClosed_Captions_triggered()
{
    closedCaptionDialog->show();
}

// Show video parameters dialogue
void MainWindow::on_actionVideo_parameters_triggered()
{
    videoParametersDialog->show();
}

// Show chroma decoder configuration
void MainWindow::on_actionChroma_decoder_configuration_triggered()
{
    chromaDecoderConfigDialog->show();
}

// Toggle chroma during seek option
void MainWindow::on_actionToggleChromaDuringSeek_triggered()
{
    bool enabled = ui->actionToggleChromaDuringSeek->isChecked();
    configuration.setToggleChromaDuringSeek(enabled);
    configuration.writeConfiguration();

}

// Media control frame signal handlers --------------------------------------------------------------------------------

// Previous field/frame button has been clicked
void MainWindow::on_previousPushButton_clicked()
{
    pausePlayback();
    // Enter chroma seek mode if appropriate
    enterChromaSeekMode(ui->previousPushButton);
    
    // Normal frame navigation (works the same in both Source and Chroma modes)
    qint32 currentNumber;
    if (tbcSource.getFieldViewEnabled()) {
        setCurrentField(currentFieldNumber - 1);
        currentNumber = currentFieldNumber;
    } else {
        setCurrentFrame(currentFrameNumber - 1);
        currentNumber = currentFrameNumber;
    }

    ui->posNumberSpinBox->setValue(currentNumber);
    ui->posHorizontalSlider->setValue(currentNumber);
}

// Next field/frame button has been clicked
void MainWindow::on_nextPushButton_clicked()
{
    pausePlayback();
    // Enter chroma seek mode if appropriate
    enterChromaSeekMode(ui->nextPushButton);
    
    // Normal frame navigation (works the same in both Source and Chroma modes)
    qint32 currentNumber;
    if (tbcSource.getFieldViewEnabled()) {
        setCurrentField(currentFieldNumber + 1);
        currentNumber = currentFieldNumber;
    } else {
        setCurrentFrame(currentFrameNumber + 1);
        currentNumber = currentFrameNumber;
    }

    ui->posNumberSpinBox->setValue(currentNumber);
    ui->posHorizontalSlider->setValue(currentNumber);
}

// Previous button pressed (for chroma toggle during seek)
void MainWindow::on_previousPushButton_pressed()
{
    if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
        // Start timer to detect if this is a hold (not just a click)
        seekTimer->start();
    }
}

// Previous button released (for chroma toggle during seek)
void MainWindow::on_previousPushButton_released()
{
    // Stop the hold detection timer if still running
    seekTimer->stop();
    
    exitChromaSeekMode(ui->previousPushButton);
}

// Next button pressed (for chroma toggle during seek)
void MainWindow::on_nextPushButton_pressed()
{
    if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
        // Start timer to detect if this is a hold (not just a click)
        seekTimer->start();
    }
}

// Next button released (for chroma toggle during seek)
void MainWindow::on_nextPushButton_released()
{
    // Stop the hold detection timer if still running
    seekTimer->stop();
    
    exitChromaSeekMode(ui->nextPushButton);
}

// Skip to the next chapter (note: this button was repurposed from 'end frame')
void MainWindow::on_endPushButton_clicked()
{
    pausePlayback();
    // Live input: the end is the live edge (not the end of a frozen buffer), and it plays from there
    if (tbcSource.getIsLive()) {
        tbcSource.unfreezeLive();
        updateLive();
    }
    setCurrentFrame(tbcSource.startOfNextChapter(currentFrameNumber));
    if (tbcSource.getIsLive()) QTimer::singleShot(0, this, [this]() { togglePlayback(); });
    auto uiNumber = currentFrameNumber;

    if (tbcSource.getFieldViewEnabled()) {
        uiNumber = currentFieldNumber;
    }

    ui->posNumberSpinBox->setValue(uiNumber);
    ui->posHorizontalSlider->setValue(uiNumber);
}

// Skip to the start of chapter (note: this button was repurposed from 'start frame')
void MainWindow::on_startPushButton_clicked()
{
    pausePlayback();
    setCurrentFrame(tbcSource.startOfChapter(currentFrameNumber));
    auto uiNumber = currentFrameNumber;

    if (tbcSource.getFieldViewEnabled()) {
        uiNumber = currentFieldNumber;
    }

    ui->posNumberSpinBox->setValue(uiNumber);
    ui->posHorizontalSlider->setValue(uiNumber);
}

// Field/Frame number spin box editing has finished
void MainWindow::on_posNumberSpinBox_editingFinished()
{
    pausePlayback();
    qint32 currentNumber;
    qint32 totalNumber;

    if (tbcSource.getFieldViewEnabled()) {
        currentNumber = currentFieldNumber;
        totalNumber = tbcSource.getNumberOfFields();
    } else {
        currentNumber = currentFrameNumber;
        totalNumber = tbcSource.getNumberOfFrames();
    }

    if (ui->posNumberSpinBox->value() != currentNumber) {
        if (ui->posNumberSpinBox->value() < 1) ui->posNumberSpinBox->setValue(1);
        if (ui->posNumberSpinBox->value() > totalNumber) ui->posNumberSpinBox->setValue(totalNumber);

        if (tbcSource.getFieldViewEnabled()) {
            setCurrentField(ui->posNumberSpinBox->value());
            currentNumber = currentFieldNumber;
        } else {
            setCurrentFrame(ui->posNumberSpinBox->value());
            currentNumber = currentFrameNumber;
        }

        ui->posHorizontalSlider->setValue(currentNumber);
    }
}

// Field/frame slider value has changed
void MainWindow::on_posHorizontalSlider_valueChanged(int value)
{
    if (!tbcSource.getIsSourceLoaded()) return;
    
    // Update the spinbox immediately for visual feedback
    if (ui->posNumberSpinBox->isEnabled()) {
        ui->posNumberSpinBox->setValue(value);
    }
    
    // Store the pending value
    pendingSliderValue = value;
    
    // If user is actively dragging, start/restart the drag pause timer
    if (sliderDragging) {
        dragPauseTimer->start(); // This will update frame if user pauses during drag
        return;
    }
    
    // For non-dragging updates (keyboard, clicks), use debounced updates
    sliderDebounceTimer->start(); // Restart the debounce timer
}

// User started dragging the slider
void MainWindow::on_posHorizontalSlider_sliderPressed()
{
    pausePlayback();
    sliderDragging = true;
    dragPauseTimer->stop(); // Stop any existing timer
}

// User finished dragging the slider - now update
void MainWindow::on_posHorizontalSlider_sliderReleased()
{
    sliderDragging = false;
    dragPauseTimer->stop(); // Stop the pause timer
    
    if (pendingSliderValue != -1) {
        // Update immediately when drag ends
        if (tbcSource.getFieldViewEnabled()) {
            setCurrentField(pendingSliderValue);
        } else {
            setCurrentFrame(pendingSliderValue);
        }
        pendingSliderValue = -1;
    }
}

// Debounced update for non-dragging slider changes
void MainWindow::onSliderDebounceTimeout()
{
    if (!sliderDragging && pendingSliderValue != -1) {
        if (tbcSource.getFieldViewEnabled()) {
            setCurrentField(pendingSliderValue);
        } else {
            setCurrentFrame(pendingSliderValue);
        }
        pendingSliderValue = -1;
    }
}

// Update frame when user pauses during drag (for visual hunting)
void MainWindow::onDragPauseTimeout()
{
    if (sliderDragging && pendingSliderValue != -1) {
        if (tbcSource.getFieldViewEnabled()) {
            setCurrentField(pendingSliderValue);
        } else {
            setCurrentFrame(pendingSliderValue);
        }
        // Don't clear pendingSliderValue - we still need it for final release
    }
}

// Source/Chroma select button clicked
void MainWindow::on_videoPushButton_clicked()
{
    if (tbcSource.getChromaDecoder()) {
        // Chroma decoder off
        tbcSource.setChromaDecoder(false);
        ui->videoPushButton->setText(tr("Source"));
    } else {
        // Chroma decoder on
        tbcSource.setChromaDecoder(true);
        ui->videoPushButton->setText(tr("Chroma"));
    }

    // Show the current image
    showImage();
}

// Aspect ratio button clicked
void MainWindow::on_aspectPushButton_clicked()
{
    displayAspectRatio = !displayAspectRatio;

    // Update the button text
	updateAspectPushButton();

    // Update the image viewer (the scopes don't depend on this)
    updateImageViewer();

	//resize the windows to fit the new size
	resize_on_aspect();
}

// Size the window to the zoom the user chose. With "resize frame with window" on, the window growing
// would make resizeFrameToWindow change the zoom to fit it (so 3x then 1x did not come back), so
// that is left alone while the window settles.
void MainWindow::resizeForZoom()
{
    zoomResizeUntil = QDateTime::currentMSecsSinceEpoch() + 600;
    resizeTimer->stop();
    resize_on_aspect();
}

void MainWindow::resize_on_aspect(int passesLeft)
{
	if(!this->isFullScreen() && !this->isMaximized() && autoResize)
	{
		// Size the window so that the scroll area shows the whole image without
		// scroll bars. The room taken by everything around the image (layout
		// margins, frame, buttons, menus, status bar) depends on the platform
		// style and fonts, so it is measured rather than assumed: the window
		// grows or shrinks by the difference between the scroll area's wanted
		// and current sizes. The image itself is in the content's size hint.
		// Fixed margins (+20, +140) left the window slightly too small.
		QScrollArea *scrollArea = ui->scrollArea;
		const int frame = 2 * scrollArea->frameWidth();

		// The layout may not be settled yet (a changed status bar, for one, can
		// change the room around the image by a pixel after it was measured)
		if (this->layout()) this->layout()->activate();

		const QSize contentSize = scrollArea->widget()->sizeHint();
		const QSize wantedSize(contentSize.width() + frame, contentSize.height() + frame);
		const QSize difference = wantedSize - scrollArea->size();
		if (difference.isNull()) return;
		this->resize(this->size() + difference);

		// The resize takes effect, and the layout is redone, in the event loop: look
		// again then, to correct what the first measurement missed
		if (passesLeft > 0) {
			QTimer::singleShot(0, this, [this, passesLeft]() { resize_on_aspect(passesLeft - 1); });
		}
	}
}

// Resize the frame to fit within the current window size
void MainWindow::resizeFrameToWindow()
{
	if (QDateTime::currentMSecsSinceEpoch() < zoomResizeUntil) {
		return;
	}
	if (!tbcSource.getIsSourceLoaded()) {
		return;
	}

	// Get the scroll area size (which contains the imageViewerLabel)
	QScrollArea* scrollArea = ui->scrollArea;
	QSize availableSize = scrollArea->viewport()->size();
	
	// Ensure we have a valid size - sometimes during resize events the size might be invalid
	if (availableSize.width() <= 0 || availableSize.height() <= 0) {
		// Use the central widget size as fallback, accounting for margins and toolbars
		QSize centralSize = ui->centralWidget->size();
		availableSize = QSize(centralSize.width() - 40, centralSize.height() - 200); // Account for UI elements
	}
	
	// Get the original image size
	QImage originalImage = activeAreaOf(tbcSource.getImage());
	if (originalImage.isNull()) {
		return;
	}

	// Calculate scale factor to fit image within available space while maintaining aspect ratio
	double scaleX = static_cast<double>(availableSize.width()) / displayedWidth(originalImage);
	double scaleY = static_cast<double>(availableSize.height()) / static_cast<double>(originalImage.height());
	
	// Use the smaller scale factor to maintain aspect ratio
	double newScaleFactor = qMin(scaleX, scaleY);
	
	// Apply a minimum scale factor to prevent the image from becoming too small
	if (newScaleFactor < 0.1) {
		newScaleFactor = 0.1;
	}
	
	// Only update if there's a significant change to avoid constant tiny adjustments
	if (qAbs(newScaleFactor - scaleFactor) > 0.01) {
		scaleFactor = newScaleFactor;
		updateImageViewer();
	}
}

// Helper method to enter chroma seek mode
void MainWindow::enterChromaSeekMode(QPushButton* button)
{
    if (!chromaSeekMode && !seekTimer->isActive() && configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder() && button->isDown()) {
        chromaSeekMode = true;
        originalChromaState = true;
        tbcSource.setChromaDecoder(false);
        ui->videoPushButton->setText(tr("Source"));
    }
}

// Helper method to exit chroma seek mode
void MainWindow::exitChromaSeekMode(QPushButton* button)
{
    if (chromaSeekMode) {
        // Use a shorter timer to check if button is truly released (not just auto-repeat)
        QTimer::singleShot(5, this, [this, button]() {
            if (!button->isDown()) {
                // Exit seek mode and restore chroma
                chromaSeekMode = false;
                tbcSource.setChromaDecoder(originalChromaState);
                ui->videoPushButton->setText(tr("Chroma"));
                updateImage(); // Fast refresh without reloading - frame data already loaded
            }
        });
    }
}

// Show/hide dropouts button clicked
void MainWindow::on_dropoutsPushButton_clicked()
{
	int width = this->width();

    if (tbcSource.getHighlightDropouts()) {
        tbcSource.setHighlightDropouts(false);
		if (width >= 930)
		{
			ui->dropoutsPushButton->setText(tr("Dropouts Off"));
		}
		else
		{
			ui->dropoutsPushButton->setText(tr("Drop N"));
		}
    } else {
        tbcSource.setHighlightDropouts(true);
        if (width >= 930)
		{
			ui->dropoutsPushButton->setText(tr("Dropouts On"));
		}
		else
		{
			ui->dropoutsPushButton->setText(tr("Drop Y"));
		}
    }

    // Show the current image (why isn't this option passed?)
    showImage();
}

// Source selection button clicked
void MainWindow::on_sourcesPushButton_clicked()
{
    switch (tbcSource.getSourceMode()) {
    case TbcSource::ONE_SOURCE:
        // Do nothing - the button's disabled anyway
        break;
    case TbcSource::LUMA_SOURCE:
        tbcSource.setSourceMode(TbcSource::CHROMA_SOURCE);
        break;
    case TbcSource::CHROMA_SOURCE:
        tbcSource.setSourceMode(TbcSource::BOTH_SOURCES);
        break;
    case TbcSource::BOTH_SOURCES:
        tbcSource.setSourceMode(TbcSource::LUMA_SOURCE);
        break;
    }

    // Update the button
    updateSourcesPushButton();

    // Show the current image
    showImage();
}

// Frame/Field view button clicked
void MainWindow::on_viewPushButton_clicked()
{
    // Playing goes on in the new view (the clock counts frames or fields, so it is restarted)
    const bool wasPlaying = playback->isPlaying();
    pausePlayback();
    switch (tbcSource.getViewMode()) {
        case TbcSource::ViewMode::FRAME_VIEW:
            if (deinterlacedView) {
                tbcDebugStream() << "Leaving the deinterlaced view for FRAME_VIEW mode";
                deinterlacedView = false;
                updateVideoFilterGraph();
                break;
            }
            tbcDebugStream() << "Changing to SPLIT_VIEW mode";

            // Set split mode
            tbcSource.setViewMode(TbcSource::ViewMode::SPLIT_VIEW);
            break;

        case TbcSource::ViewMode::SPLIT_VIEW:
            tbcDebugStream() << "Changing to FIELD_VIEW mode";

            // Set field mode, stretched 2:1 to frame height (the 1:1 field
            // mode was removed as it added a click without being useful)
            tbcSource.setViewMode(TbcSource::ViewMode::FIELD_VIEW);
            tbcSource.setStretchField(true);
            break;

        case TbcSource::ViewMode::FIELD_VIEW:
            // Then a frame through the chosen deinterlacer, if this build has the filters
            tbcSource.setViewMode(TbcSource::ViewMode::FRAME_VIEW);
            if (VideoFilter::isAvailable()) {
                tbcDebugStream() << "Changing to the deinterlaced view";
                deinterlacedView = true;
                const QString problem = updateVideoFilterGraph();
                if (!problem.isEmpty()) {
                    deinterlacedView = false;
                    updateVideoFilterGraph();
                    statusBar()->showMessage(tr("The deinterlacer could not be used: %1").arg(problem), 10000);
                }
            }
            if (!deinterlacedView) tbcDebugStream() << "Changing to FRAME_VIEW mode";
            break;
    }

    setViewValues();
    updateGuiLoaded();

    // Show the current image
    showImage();

    if (wasPlaying) togglePlayback();
}

// Normal/Reverse field order button clicked
void MainWindow::on_fieldOrderPushButton_clicked()
{
    // The field order changes what the decoders read: stop them first, and go on afterwards from
    // the same frame (the interface is set up again below, which would put it back to the first)
    const bool wasPlaying = playback->isPlaying();
    const qint32 frameBefore = currentFrameNumber;
    pausePlayback();

	int width = this->width();

    if (tbcSource.getFieldOrder()) {
        tbcSource.setFieldOrder(false);

        // If the TBC field order is changed, the number of available frames can change, so we need to update the GUI
        resetGui();
        updateGuiLoaded();
        if (width > 1000)
			ui->fieldOrderPushButton->setText(tr("Normal Field-order"));
		else if (width >= 930)
			ui->fieldOrderPushButton->setText(tr("Normal order"));
		else
			ui->fieldOrderPushButton->setText(tr("Normal"));
    } else {
        tbcSource.setFieldOrder(true);

        // If the TBC field order is changed, the number of available frames can change, so we need to update the GUI
        resetGui();
        updateGuiLoaded();
        if (width > 1000)
			ui->fieldOrderPushButton->setText(tr("Reverse Field-order"));
		else if (width >= 930)
			ui->fieldOrderPushButton->setText(tr("Reverse order"));
		else
			ui->fieldOrderPushButton->setText(tr("Reverse"));
    }

    // Show the current image
    showImage();

    // (the menu entry follows, however the order was changed)
    const QSignalBlocker blocker(actionFieldOrder);
    actionFieldOrder->setChecked(tbcSource.getFieldOrder());

    if (wasPlaying) {
        if (!tbcSource.getIsLive()) setCurrentFrame(qBound(1, frameBefore, qMax(1, tbcSource.getNumberOfFrames())));
        togglePlayback();
    }
}

void MainWindow::on_toggleAutoResize_toggled(bool checked)
{
	autoResize = checked;
}

void MainWindow::on_actionResizeFrameWithWindow_toggled(bool checked)
{
	resizeFrameWithWindow = checked;
	
	// Save the setting to configuration
	configuration.setResizeFrameWithWindow(checked);
	configuration.writeConfiguration();
	
	// If resizeFrameWithWindow is now enabled, resize frame to fit current window
	if (checked && tbcSource.getIsSourceLoaded()) {
		resizeTimer->start();
	}
}

// Zoom in
void MainWindow::on_zoomInPushButton_clicked()
{
    constexpr double factor = 1.1;
    if (((scaleFactor * factor) > 0.333) && ((scaleFactor * factor) < 3.0)) {
        scaleFactor *= factor;
    }

    updateImageViewer();
}

// Zoom out
void MainWindow::on_zoomOutPushButton_clicked()
{
    constexpr double factor = 0.9;
    if (((scaleFactor * factor) > 0.333) && ((scaleFactor * factor) < 3.0)) {
        scaleFactor *= factor;
    }

    updateImageViewer();
}

// Original size 1:1 zoom
void MainWindow::on_originalSizePushButton_clicked()
{
    scaleFactor = 1.0;
    updateImageViewer();
    resizeForZoom();
}



// Mouse mode button clicked
void MainWindow::on_mouseModePushButton_clicked()
{
    if (ui->mouseModePushButton->isChecked()) {
        // Show the oscilloscope view if currently hidden
        if (!oscilloscopeDialog->isVisible()) {
            updateOscilloscopeDialogue();
            oscilloscopeDialog->show();
        }
    }

    // Update the image viewer to display/hide the indicator line
    updateImageViewer();
}

// Miscellaneous handler methods --------------------------------------------------------------------------------------

// Handler called when another class changes the currently selected scan line
void MainWindow::scopeCoordsChangedSignalHandler(qint32 xCoord, qint32 yCoord)
{
    tbcDebugStream() << "MainWindow::scanLineChangedSignalHandler(): Called with xCoord =" << xCoord << "and yCoord =" << yCoord;

    if (tbcSource.getIsSourceLoaded()) {
        // Show the oscilloscope dialogue for the selected scan-line
        lastScopeDot = xCoord;
        lastScopeLine = yCoord + 1;
        updateOscilloscopeDialogue();
        oscilloscopeDialog->show();

        // Update the image viewer
        updateImageViewer();
    }
}

// Handler called when vectorscope settings are changed
void MainWindow::vectorscopeChangedSignalHandler()
{
    tbcDebugStream() << "MainWindow::vectorscopeChangedSignalHandler(): Called";

    if (tbcSource.getIsSourceLoaded()) {
        // Update the vectorscope
        updateVectorscopeDialogue();
    }
}

// Mouse press event handler
void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // (the full screen shows the active area only: its coordinates are not the scope's)
    if (!tbcSource.getIsSourceLoaded() || fullScreenOn) return;

    // Get the mouse position relative to our scene
    QPoint origin = ui->imageViewerLabel->mapFromGlobal(QCursor::pos());

    // Check that the mouse click is within bounds of the current picture
    qint32 oX = origin.x();
    qint32 oY = origin.y();

    if (oX + 1 >= 0 &&
            oY >= 0 &&
            oX + 1 <= ui->imageViewerLabel->width() &&
            oY <= ui->imageViewerLabel->height()) {

        mouseScanLineSelect(oX, oY);
        event->accept();
    }
}

// Mouse move event
void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!tbcSource.getIsSourceLoaded()) return;

    // Get the mouse position relative to our scene
    QPoint origin = ui->imageViewerLabel->mapFromGlobal(QCursor::pos());

    // Check that the mouse click is within bounds of the current picture
    qint32 oX = origin.x();
    qint32 oY = origin.y();

    if (oX + 1 >= 0 &&
            oY >= 0 &&
            oX + 1 <= ui->imageViewerLabel->width() &&
            oY <= ui->imageViewerLabel->height()) {

        mouseScanLineSelect(oX, oY);
        event->accept();
    }
}

// Perform mouse based scan line selection
void MainWindow::mouseScanLineSelect(qint32 oX, qint32 oY)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPixmap imageViewerPixmap = ui->imageViewerLabel->pixmap();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QPixmap imageViewerPixmap = ui->imageViewerLabel->pixmap(Qt::ReturnByValue);
#else
    QPixmap imageViewerPixmap = *(ui->imageViewerLabel->pixmap());
#endif

    // X calc
    double offsetX = ((static_cast<double>(ui->imageViewerLabel->width()) -
                       static_cast<double>(imageViewerPixmap.width())) / 2.0);

    double unscaledXR = (static_cast<double>(tbcSource.getFrameWidth()) /
                         static_cast<double>(imageViewerPixmap.width())) * static_cast<double>(oX - offsetX);
    qint32 unscaledX = static_cast<qint32>(unscaledXR);
    if (unscaledX > tbcSource.getFrameWidth() - 1) unscaledX = tbcSource.getFrameWidth() - 1;
    if (unscaledX < 0) unscaledX = 0;

    // Y Calc
    double offsetY = ((static_cast<double>(ui->imageViewerLabel->height()) -
                       static_cast<double>(imageViewerPixmap.height())) / 2.0);

    double unscaledYR = (static_cast<double>(tbcSource.getFrameHeight()) /
                         static_cast<double>(imageViewerPixmap.height())) * static_cast<double>(oY - offsetY);
    qint32 unscaledY = static_cast<qint32>(unscaledYR);
    if (unscaledY > tbcSource.getFrameHeight()) unscaledY = tbcSource.getFrameHeight();
    if (unscaledY < 1) unscaledY = 1;

    // Show the oscilloscope dialogue for the selected scan-line (if the right mouse mode is selected)
    if (ui->mouseModePushButton->isChecked()) {
        // Remember the last line rendered
        lastScopeLine = unscaledY;
        lastScopeDot = unscaledX;

        updateOscilloscopeDialogue();
        oscilloscopeDialog->show();

        // Update the image viewer
        updateImageViewer();
    }
}

// Handle parameters changed signal from the video parameters dialogue
void MainWindow::videoParametersChangedSignalHandler(const LdDecodeMetaData::VideoParameters &videoParameters)
{
    // Update the VideoParameters in the source
    tbcSource.setVideoParameters(videoParameters);

    // Enable the "Save Metadata" action, since the metadata has been modified (a live input has
    // no metadata file to save to)
    ui->actionSave_Metadata->setEnabled(!tbcSource.getIsLive());

    // Update the aspect button's label
    updateAspectPushButton();

    // Update the image viewer
    updateImage();
}

// Handle configuration changed signal from the chroma decoder configuration dialogue
void MainWindow::chromaDecoderConfigChangedSignalHandler()
{
    // Set the new configuration
    tbcSource.setChromaConfiguration(chromaDecoderConfigDialog->getPalConfiguration(),
                                     chromaDecoderConfigDialog->getNtscConfiguration(),
                                     chromaDecoderConfigDialog->getOutputConfiguration());

    // Update the image viewer
    updateImage();
}

// TbcSource class signal handlers ------------------------------------------------------------------------------------

// Signal handler for busy signal from TbcSource class
void MainWindow::on_busy(QString infoMessage)
{
    tbcDebugStream() << "MainWindow::on_busy(): Got signal with message" << infoMessage;
    // Set the busy message and centre the dialog in the parent window
    busyDialog->setMessage(infoMessage);
    busyDialog->move(this->geometry().center() - busyDialog->rect().center());

    if (!busyDialog->isVisible()) {
        // Disable the main window during loading
        this->setEnabled(false);
        busyDialog->setEnabled(true);

        busyDialog->show();
    }
}

// Signal handler for finishedLoading signal from TbcSource class
// Fill the analysis graphs from the loaded source
void MainWindow::updateGraphs()
{
    dropoutAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
    visibleDropoutAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
    blackSnrAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
    whiteSnrAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());

    QVector<double> doGraphData = tbcSource.getDropOutGraphData();
    QVector<double> visibleDoGraphData = tbcSource.getVisibleDropOutGraphData();
    QVector<double> blackSnrGraphData = tbcSource.getBlackSnrGraphData();
    QVector<double> whiteSnrGraphData = tbcSource.getWhiteSnrGraphData();

    for (qint32 frameNumber = 0; frameNumber < qMin(tbcSource.getNumberOfFrames(), tbcSource.getGraphDataSize()); frameNumber++) {
        dropoutAnalysisDialog->addDataPoint(frameNumber + 1, doGraphData[frameNumber]);
        visibleDropoutAnalysisDialog->addDataPoint(frameNumber + 1, visibleDoGraphData[frameNumber]);
        blackSnrAnalysisDialog->addDataPoint(frameNumber + 1, blackSnrGraphData[frameNumber]);
        whiteSnrAnalysisDialog->addDataPoint(frameNumber + 1, whiteSnrGraphData[frameNumber]);
    }

    dropoutAnalysisDialog->finishUpdate(currentFrameNumber);
    visibleDropoutAnalysisDialog->finishUpdate(currentFrameNumber);
    blackSnrAnalysisDialog->finishUpdate(currentFrameNumber);
    whiteSnrAnalysisDialog->finishUpdate(currentFrameNumber);
}

// Add View > Theme (Auto/Light/Dark) and View > Style (Auto/Windows 11/Classic/
// Fusion), with the current choices checked
void MainWindow::setupAppearanceMenus()
{
    QMenu *themeMenu = new QMenu(tr("Theme"), this);
    QActionGroup *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);

    const struct {
        ThemeMode mode;
        QString text;
    } themeEntries[] = {
        // The system's own appearance
        { ThemeMode::Auto, tr("Auto (follow system)") },
        { ThemeMode::Light, tr("Light") },
        { ThemeMode::Dark, tr("Dark") },
        // Fixed palettes
        { ThemeMode::SoftLight, tr("Soft light") },
        { ThemeMode::Dim, tr("Dim") },
        { ThemeMode::DarkGrey, tr("Dark grey") },
        { ThemeMode::Custom, tr("Custom") },
    };

    for (const auto &entry : themeEntries) {
        if (entry.mode == ThemeMode::SoftLight) themeMenu->addSeparator();
        QAction *action = themeMenu->addAction(entry.text);
        action->setCheckable(true);
        action->setChecked(entry.mode == themeMode);
        themeGroup->addAction(action);
        const ThemeMode mode = entry.mode;
        if (mode == ThemeMode::Custom) {
            // Only selectable once a custom theme has been saved
            customThemeAction = action;
            action->setEnabled(!customPalette.isEmpty());
        }
        connect(action, &QAction::triggered, this, [this, mode]() {
            themeMode = mode;
            configuration.setThemeMode(static_cast<qint32>(mode));
            updateAppearance();
        });
    }

    themeMenu->addSeparator();
    QAction *customizeAction = themeMenu->addAction(tr("Customize..."));
    connect(customizeAction, &QAction::triggered, this, &MainWindow::customizeTheme);

    QMenu *styleMenu = new QMenu(tr("Style"), this);
    QActionGroup *styleGroup = new QActionGroup(this);
    styleGroup->setExclusive(true);

    // Auto first, then every style Qt finds on this machine
    QStringList names = { QString() };
    names += availableStyles();

    for (const QString &name : names) {
        QString text;
        if (name.isEmpty()) {
#ifdef Q_OS_WIN
            text = tr("Auto (Classic look)");
#else
            text = tr("Auto (system)");
#endif
        } else {
            text = styleDisplayName(name);
        }

        QAction *action = styleMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(name.compare(styleName, Qt::CaseInsensitive) == 0);
        action->setData(name);
        styleGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name]() {
            styleName = name;
            configuration.setStyleName(name);
            updateAppearance();
        });
    }
    ui->menuView->addSeparator();
    ui->menuView->addMenu(themeMenu);
    ui->menuView->addMenu(styleMenu);
}

// Apply the theme and style now and remember the choice
void MainWindow::updateAppearance()
{
    configuration.writeConfiguration();

    isDarkTheme = applyAppearance(styleName, themeMode, customPalette);
    refreshAfterAppearanceChange(true);
}

// Update what keeps its own copy of the colours. The graphs only depend on
// whether the theme is dark, and are costly to rebuild on a long capture, so
// the live preview of the theme editor only rebuilds them when that changes.
void MainWindow::refreshAfterAppearanceChange(bool updateGraphData)
{
    buttonPalette = QApplication::palette();
    updatePlayButtonIcon();

    if (tbcSource.getIsSourceLoaded()) {
        if (updateGraphData) updateGraphs();
        showImage();
    }
}

// View > Theme > Customize...: edit the custom theme with a live preview
void MainWindow::customizeTheme()
{
    const ThemeMode previousMode = themeMode;
    const QString previousPalette = customPalette;

    // The play button colour is kept apart from the system palette, which has none
    QPalette startPalette = QApplication::palette();
    startPalette.setColor(QPalette::All, QPalette::BrightText, playButtonColour());
    startPalette.setColor(QPalette::All, QPalette::LinkVisited, waveformColour(false));
    startPalette.setColor(QPalette::Disabled, QPalette::LinkVisited, waveformColour(true));
    ThemeEditorDialog editor(startPalette, this);
    connect(&editor, &ThemeEditorDialog::previewChanged, this, [this](const QString &palette) {
        const bool wasDark = isDarkTheme;
        isDarkTheme = applyAppearance(styleName, ThemeMode::Custom, palette);
        refreshAfterAppearanceChange(isDarkTheme != wasDark);
    });

    if (editor.exec() == QDialog::Accepted) {
        customPalette = editor.palette();
        themeMode = ThemeMode::Custom;
        configuration.setCustomPalette(customPalette);
        configuration.setThemeMode(static_cast<qint32>(themeMode));
        if (customThemeAction) {
            customThemeAction->setEnabled(true);
            customThemeAction->setChecked(true);
        }
    } else {
        themeMode = previousMode;
        customPalette = previousPalette;
    }

    updateAppearance();
}

void MainWindow::on_finishedLoading(bool success)
{
    tbcDebugStream() << "MainWindow::on_finishedLoading(): Called";

    // Hide the busy dialogue
    busyDialog->hide();

    // Ensure source loaded ok
    if (success) {
        // Every input opens at its original size, whatever the zoom was before
        scaleFactor = 1.0;

        // Generate the graph data
        updateGraphs();

        // Update the GUI
        resetGui();
        updateGuiLoaded();

        // A stack has no SNR in its metadata (that of its first source is not the stack's), so it is
        // measured from the video, as with Measure ticked
        if (tbcSource.getIsStack()) setFileSnrMeasuring(true);

        ui->actionOpen_TBC_file->setEnabled(!tbcSource.getIsLive());
        // Set the main window title
        this->setWindowTitle(tbcSource.getIsLive() ? tr("ld-analyse - Live input")
                                                   : tr("ld-analyse - ") + tbcSource.getCurrentSourceFilename());

        // Update the configuration for the source directory
        if (!tbcSource.getIsLive()) {
            QFileInfo inFileInfo(tbcSource.getCurrentSourceFilename());
        configuration.setSourceDirectory(inFileInfo.absolutePath());
        tbcDebugStream() << "MainWindow::loadTbcFile(): Setting source directory to:" << inFileInfo.absolutePath();
        configuration.writeConfiguration();
            }

        if (tbcSource.getIsLive()) {
            // There is nothing to reload or save; the video plays as it arrives, and stays on
            ui->actionReload_TBC->setEnabled(false);
            ui->actionOpen_TBC_file->setEnabled(false);
            setPlaybackOptionsVisible(false);
            if (!tbcSource.getStreamHint().isEmpty()) statusBar()->showMessage(tbcSource.getStreamHint(), 30000);
            liveTimer->start();
            updateLive();
            // At the live edge, not at the first frame, which would be as late as loading took
            setCurrentFrame(qMax(1, tbcSource.getNumberOfFrames() - 1));
            {
                const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
                const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
                ui->posNumberSpinBox->setValue(currentFrameNumber);
                ui->posHorizontalSlider->setValue(currentFrameNumber);
            }
            togglePlayback();
        }
    } else {
        // Load failed
        updateGuiUnloaded();
        ui->actionOpen_TBC_file->setEnabled(true);

        // Show the error to the user
        QMessageBox messageBox;
        messageBox.warning(this, "Error", tbcSource.getLastIOError());
    }

    // Enable the main window
    this->setEnabled(true);
}

// Signal handler for finishedSaving signal from TbcSource class
void MainWindow::on_finishedSaving(bool success)
{
    tbcDebugStream() << "MainWindow::on_finishedSaving(): Called";

    // Hide the busy dialogue
    busyDialog->hide();

    if (success) {
        // Disable the "Save Metadata" action until the metadata is modified again
        ui->actionSave_Metadata->setEnabled(false);
    } else {
        // Show the error to the user
        QMessageBox messageBox;
        messageBox.warning(this, tr("Error"), tbcSource.getLastIOError());
    }

    // Enable the main window
    this->setEnabled(true);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    int width = this->width();

	//field order rename depending on size
	if (!tbcSource.getFieldOrder())
	{
		if (width > 1000)
			ui->fieldOrderPushButton->setText(tr("Normal Field-order"));
		else if (width >= 930)
			ui->fieldOrderPushButton->setText(tr("Normal order"));
		else
			ui->fieldOrderPushButton->setText(tr("Normal"));
	}
	else
	{
		if (width > 1000)
			ui->fieldOrderPushButton->setText(tr("Reverse Field-order"));
		else if (width >= 930)
			ui->fieldOrderPushButton->setText(tr("Reverse order"));
		else
			ui->fieldOrderPushButton->setText(tr("Reverse"));
	}

	//source label depending on size
	updateSourcesPushButton();

	//filters label
	updateFiltersButton();

	//dropout label
	if (!tbcSource.getHighlightDropouts())
	{
		if (width >= 930)
		{
			ui->dropoutsPushButton->setText(tr("Dropouts Off"));
		}
		else
		{
			ui->dropoutsPushButton->setText(tr("Drop N"));
		}

	}
	else
	{
		if (width >= 930)
		{
			ui->dropoutsPushButton->setText(tr("Dropouts On"));
		}
		else
		{
			ui->dropoutsPushButton->setText(tr("Drop Y"));
		}
	}

	//view label
	if (this->width() >= 930)
	{
		if (tbcSource.getFieldViewEnabled()) {
			ui->viewPushButton->setText(tr("Field View"));
		} else {
			if (tbcSource.getSplitViewEnabled()) {
				ui->viewPushButton->setText(tr("Split View"));
			} else {
				ui->viewPushButton->setText(tr("Frame View"));
			}
		}
	}
	else
	{
		if (tbcSource.getFieldViewEnabled()) {
			ui->viewPushButton->setText(tr("Field"));
		} else {
			if (tbcSource.getSplitViewEnabled()) {
				ui->viewPushButton->setText(tr("Split"));
			} else {
				ui->viewPushButton->setText(tr("Frame"));
			}
		}
	}

	//aspect ratio label
	updateAspectPushButton();

	// leave out the field order button if the row has not the room
	updateFieldOrderVisibility();

	// Resize frame with window if resizeFrameWithWindow is enabled
	if (resizeFrameWithWindow && tbcSource.getIsSourceLoaded()) {
		resizeTimer->start();
	}
}

#ifdef LDDECODE_HAVE_AUDIO
// The audio ---------------------------------------------------------------------------------------------------------------

// The waveform panel, under the video, the View menu entry that shows it, and the engine that plays
void MainWindow::setupAudio()
{
    audio = new AudioEngine(this);
    audioStatsTimer = new QTimer(this);
    audioStatsTimer->setInterval(500);
    connect(audioStatsTimer, &QTimer::timeout, this, &MainWindow::updateAudioStatistics);
    audioStatsTimer->start();
    waveform = new WaveformPanel(audio, this);
    waveform->setEfmPossible([this]() { return !efmSourceName().isEmpty(); });
    connect(waveform, &WaveformPanel::efmDecodeRequested, this, [this]() {
        if (efmSourceName().isEmpty()) return;
        // (the decoder is a choice of its own, made in the Audio panel and kept: the source menu only asks for the EFM)
        if (audioConfig) startEfmDecoding(audioConfig->currentDecoder(), audioConfig->efmWithoutTimecodes(), audioConfig->efmConceal(), audioConfig->efmPad());
        else startEfmDecoding(configuration.audioValue("decoder", 1).toInt(), true, true, false);
    });
    showWaveform(false);
    ui->verticalLayout->insertWidget(ui->verticalLayout->indexOf(ui->mediaControl_frame), waveform);

    ui->menuView->addSeparator();
    actionWaveform = ui->menuView->addAction(tr("&Waveform"));
    actionWaveform->setCheckable(true);
    actionWaveform->setEnabled(false);
    actionAudioConfig = new QAction(tr("Audio &panel..."), this);        // (in the Config menu: buildConfigMenu)
    connect(actionAudioConfig, &QAction::triggered, this, &MainWindow::showAudioConfig);

    // What was chosen the last time
    waveformWanted = configuration.audioValue("waveformVisible", true).toBool();
    const bool left = configuration.audioValue("left", true).toBool();
    const bool right = configuration.audioValue("right", true).toBool();
    const qint32 volume = configuration.audioValue("volume", 70).toInt();
    waveform->setMix(left, right, volume);
    audio->setMix(left, right, volume / 100.0);
    waveform->setControlsVisible(configuration.audioValue("sidePanel", true).toBool());
    const bool muted = configuration.audioValue("muted", false).toBool();
    waveform->setMuted(muted);
    audio->setMuted(muted);
    waveform->setStyle(static_cast<WaveformView::Style>(configuration.audioValue("style", 0).toInt()));
    waveform->setWell(configuration.audioValue("well", true).toBool());
    waveform->setSoloChannel(configuration.audioValue("soloChannel", false).toBool());
    waveform->setDensity(static_cast<WaveformPanel::Density>(configuration.audioValue("density", 1).toInt()));
    connect(waveform, &WaveformPanel::mutedChanged, this, [this](bool isMuted) {
        audio->setMuted(isMuted);
        configuration.setAudioValue("muted", isMuted);
    });

    connect(actionWaveform, &QAction::toggled, this, [this](bool shown) {
        waveformWanted = shown;
        configuration.setAudioValue("waveformVisible", shown);
        showWaveform(shown && audio->hasAudio());
    });
    connect(waveform, &WaveformPanel::trackSelected, this, [this](qint32 index) {
        const double at = audio->isPlaying() ? audio->videoSecondsNow() : -1.0;
        waveform->setMessage(QString());
        audio->selectTrack(index);
        waveform->refresh();
        audioFollowVideo(at);
    });
    connect(waveform, &WaveformPanel::mixChanged, this, [this](bool leftOn, bool rightOn, qint32 volume) {
        audio->setMix(leftOn, rightOn, volume / 100.0);
        configuration.setAudioValue("left", leftOn);
        configuration.setAudioValue("right", rightOn);
        configuration.setAudioValue("volume", volume);
    });
    connect(waveform, &WaveformPanel::seekRequested, this, &MainWindow::waveformSeek);
    connect(audio, &AudioEngine::peaksProgress, this, [this](qint32) { waveform->refresh(); });
    connect(audio, &AudioEngine::peaksFinishedSignal, this, [this]() { waveform->refresh(); });
    connect(audio, &AudioEngine::problem, this, [this](const QString &message) { statusBar()->showMessage(message, 8000); waveform->setMessage(message); });
}


// What the decoder programs report of errors, in what they say when they end: the lines about errors, corrections, concealed or lost
// samples and frames, with a value that is not zero
static QStringList efmFindings(const QStringList &lines)
{
    static const QRegularExpression interesting("invalid|corrupt|missing|conceal|silenc|uncorrect|error|corrected|fixed|loss|lost|discard|interrupt|overshoot|undershoot|out of order",
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression value(QStringLiteral(":\\s*([0-9]+(?:\\.[0-9]+)?)\\s*(%|MBytes)?\\s*(?:\\(.*\\))?\\s*$"));
    QStringList found;
    for (const QString &line: lines) {
        // ("[stage] Info:   Invalid C1s: 853")
        const qint32 info = line.indexOf("Info:");
        const QString text = (info >= 0 ? line.mid(info + 5) : line).trimmed();
        const QRegularExpressionMatch match = value.match(text);
        if (!match.hasMatch() || !interesting.match(text).hasMatch()) continue;
        if (match.captured(1).toDouble() == 0.0) continue;
        const QString stage = line.startsWith('[') ? line.left(line.indexOf(']') + 1) + " " : QString();
        found.append(stage + text);
    }
    return found;
}

// The numbers of the sound and of the EFM decoders, for the Audio panel: where the sound is against the picture, how long it had
// to wait for audio, and what each decoder has done and said
void MainWindow::updateAudioStatistics()
{
    if (!audioConfig || !audioConfig->isVisible()) return;
    QString text;
    QTextStream out(&text);
    out.setRealNumberNotation(QTextStream::FixedNotation);
    out.setRealNumberPrecision(2);

    out << tr("Sound") << "\n";
    const qint32 current = audio->currentTrack();
    if (current >= 0 && current < audio->tracks().size()) out << "  " << tr("Track played: %1").arg(audio->tracks()[current].label) << "\n";
    if (audio->isPlaying()) {
        const double unitsPerSecond = (tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0) * (tbcSource.getFieldViewEnabled() || weaveActive ? 2.0 : 1.0);
        const double picture = positionSeconds(tbcSource.getFieldViewEnabled() || weaveActive ? currentFieldNumber : currentFrameNumber);
        const double lagMs = (picture - audio->videoSecondsNow()) * 1000.0;
        Q_UNUSED(unitsPerSecond);
        out << "  " << tr("Sound against picture: %1 ms (%2)").arg(lagMs, 0, 'f', 0).arg(lagMs > 0 ? tr("the sound is behind") : tr("the sound is ahead")) << "\n";
        out << "  " << tr("Silence while the audio had not come: %1 s in %2 waits")
                           .arg(static_cast<double>(audio->pairsWaited()) / qMax(1.0, audio->sampleRate()), 0, 'f', 2).arg(audio->waitsForAudio()) << "\n";
    } else {
        out << "  " << tr("Not playing") << "\n";
    }

    const QString names[2] = { EfmDecoding::backendName(EfmDecoding::Backend::Suite), EfmDecoding::backendName(EfmDecoding::Backend::ProcessEfm) };
    for (qint32 i = 0; i < 2; i++) {
        EfmDecoding *decoder = efmDecoders[i];
        out << "\n" << names[i] << "\n";
        if (!decoder) {
            out << "  " << tr("Not started") << "\n";
            continue;
        }
        const double seconds = decoder->seconds();
        const double run = decoder->runSeconds();
        out << "  " << (decoder->isRunning() ? tr("Running") : tr("Finished")) << tr(", audio decoded: %1 s in %2 s of work").arg(seconds, 0, 'f', 1).arg(run, 0, 'f', 1);
        if (run > 0.5) out << tr(" (%1 times the speed of the video)").arg(seconds / run, 0, 'f', 1);
        out << "\n";
        if (decoder->efmBytesFed() > 0) out << "  " << tr("EFM given: %1 MB").arg(decoder->efmBytesFed() / 1048576.0, 0, 'f', 2) << "\n";
        // (the sound against what is decoded: the decoder must be ahead of the picture, or the sound waits)
        const qint32 track = audio->currentTrack();
        if (audio->isPlaying() && track >= 0 && audio->isDecoded(track) && audio->tracks()[track].trackId == -2 - i) {
            const double ahead = seconds - static_cast<double>(audio->samplePlaying()) / 44100.0;
            out << "  " << tr("Decoded ahead of the sound: %1 s%2").arg(ahead, 0, 'f', 1).arg(ahead < 0.3 ? tr(" (the sound waits for the decoder)") : QString()) << "\n";
        }
        const QStringList lines = decoder->messages();
        if (decoder->isRunning()) {
            out << "  " << tr("The programs give their statistics (errors, corrections, concealed samples) when they end.") << "\n";
            for (qint32 line = qMax<qint32>(0, lines.size() - 3); line < lines.size(); line++) out << "    " << lines[line] << "\n";
        } else {
            const QStringList findings = efmFindings(lines);
            out << "  " << (findings.isEmpty() ? tr("No error, correction or concealed sample reported")
                                               : tr("Errors, corrections and concealed samples (not zero):")) << "\n";
            for (const QString &finding: findings) out << "    " << finding << "\n";
        }
    }
    audioConfig->setStatistics(text);
}

// The Audio config window: made when first wanted, with what is set now
void MainWindow::showAudioConfig()
{
    if (!audioConfig) {
        audioConfig = new AudioConfigDialog(this);
        audioConfig->setDevice(configuration.audioValue("device", QByteArray()).toByteArray());
        audio->setOutputDevice(configuration.audioValue("device", QByteArray()).toByteArray());
        audioConfig->setSidePanel(configuration.audioValue("sidePanel", true).toBool());
        audioConfig->setActiveDecoder(configuration.audioValue("decoder", 1).toInt());
        connect(audioConfig, &AudioConfigDialog::decoderChosen, this, [this](qint32 decoder) { configuration.setAudioValue("decoder", decoder); });
        audioConfig->setMix(waveform->leftOn(), waveform->rightOn(), waveform->volume());
        connect(audioConfig, &AudioConfigDialog::deviceChosen, this, [this](const QByteArray &id) {
            audio->setOutputDevice(id);
            configuration.setAudioValue("device", id);
        });
        connect(audioConfig, &AudioConfigDialog::mutedChanged, this, [this](bool muted) {
            waveform->setMuted(muted);
            audio->setMuted(muted);
            configuration.setAudioValue("muted", muted);
        });
        connect(waveform, &WaveformPanel::mutedChanged, audioConfig, &AudioConfigDialog::setMuted);
        connect(audioConfig, &AudioConfigDialog::waveformShownChanged, actionWaveform, &QAction::setChecked);
        connect(actionWaveform, &QAction::toggled, audioConfig, &AudioConfigDialog::setWaveformShown);
        connect(audioConfig, &AudioConfigDialog::styleChosen, this, [this](qint32 style) {
            waveform->setStyle(static_cast<WaveformView::Style>(style));
            configuration.setAudioValue("style", style);
        });
        audioConfig->setMuted(waveform->muted());
        audioConfig->setWaveformShown(actionWaveform->isChecked());
        audioConfig->setStyle(configuration.audioValue("style", 0).toInt());
        audioConfig->setWell(configuration.audioValue("well", true).toBool());
        audioConfig->setSidecars(configuration.audioValue("sidecars", true).toBool());
        connect(audioConfig, &AudioConfigDialog::sidecarsChanged, this, [this](bool on) { configuration.setAudioValue("sidecars", on); });
        audioConfig->setPreferDigital(configuration.audioValue("preferDigital", true).toBool());
        connect(audioConfig, &AudioConfigDialog::preferDigitalChanged, this, [this](bool on) { configuration.setAudioValue("preferDigital", on); });
        audioConfig->setSoloChannel(configuration.audioValue("soloChannel", false).toBool());
        connect(audioConfig, &AudioConfigDialog::soloChannelChanged, this, [this](bool on) {
            waveform->setSoloChannel(on);
            configuration.setAudioValue("soloChannel", on);
        });
        connect(audioConfig, &AudioConfigDialog::wellChanged, this, [this](bool on) {
            waveform->setWell(on);
            configuration.setAudioValue("well", on);
        });
        audioConfig->setDensity(configuration.audioValue("density", 1).toInt());
        connect(audioConfig, &AudioConfigDialog::densityChosen, this, [this](qint32 density) {
            applyWaveformDensity(density);
            configuration.setAudioValue("density", density);
        });
        connect(audioConfig, &AudioConfigDialog::sidePanelChanged, this, [this](bool shown) {
            waveform->setControlsVisible(shown);
            configuration.setAudioValue("sidePanel", shown);
        });
        // (the controls are in the panel and here: each follows the other)
        connect(audioConfig, &AudioConfigDialog::mixChanged, this, [this](bool leftOn, bool rightOn, qint32 volume) {
            waveform->setMix(leftOn, rightOn, volume);
            audio->setMix(leftOn, rightOn, volume / 100.0);
            configuration.setAudioValue("left", leftOn);
            configuration.setAudioValue("right", rightOn);
            configuration.setAudioValue("volume", volume);
        });
        connect(waveform, &WaveformPanel::mixChanged, audioConfig, &AudioConfigDialog::setMix);
        connect(audioConfig, &AudioConfigDialog::offsetChanged, this, [this](qint32 milliseconds) {
            audio->setOffsetMs(milliseconds);
            waveform->refresh();
        });
        connect(audioConfig, &AudioConfigDialog::imported, this, [this](const QVector<qint16> &samples, double rate, const QString &name) {
            audioStop();
            const qint32 index = audio->setImported(samples, rate, tr("%1 (imported)").arg(name));
            audio->selectTrack(index);
            waveform->setTracks(audio->tracks(), index);
            actionWaveform->setEnabled(true);
            const QSignalBlocker blocker(actionWaveform);
            actionWaveform->setChecked(true);
            waveformWanted = true;
            showWaveform(true);
            updateAudioPlayhead();
        });
        connect(audioConfig, &AudioConfigDialog::importRemoved, this, [this]() {
            audioStop();
            audio->clearImported();
            if (audio->hasAudio()) {
                qint32 first = 0;
                for (qint32 i = 0; i < audio->tracks().size(); i++) {
                    if (audio->tracks()[i].source == "analogue") { first = i; break; }
                }
                audio->selectTrack(first);
                waveform->setTracks(audio->tracks(), first);
            } else {
                showWaveform(false);
            }
        });
        connect(audioConfig, &AudioConfigDialog::saveRequested, this, &MainWindow::saveImportedAudio);
        connect(audioConfig, &AudioConfigDialog::decodeRequested, this, [this](qint32 decoder, bool noTimecodes, bool conceal, bool pad) {
            startEfmDecoding(decoder, noTimecodes, conceal, pad);
        });
        connect(audioConfig, &AudioConfigDialog::decodeStopRequested, this, [this](qint32 decoder) {
            if (efmDecoders[decoder]) efmDecoders[decoder]->stop();
            audioConfig->setDecodeStatus(decoder, tr("Stopped"), false);
        });
    }
    const bool isLdx = tbcSource.getIsSourceLoaded() && LdxCapture::isLdxName(tbcSource.getCurrentSourceFilename());
    QString path;
    qint32 track = 0;
    if (isLdx) LdxCapture::split(tbcSource.getCurrentSourceFilename(), path, track);
    const qint32 last = tbcSource.getIsSourceLoaded() ? (tbcSource.getFieldViewEnabled() ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames()) : 0;
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    audioConfig->setVideo(last > 0 ? last / (tbcSource.getFieldViewEnabled() ? 2.0 * frameRate : frameRate) : 0.0, path);
    {
        const QString source = efmSourceName();
        audioConfig->setEfmSource(source == "-" ? tr("the live input (the decoders wait for its EFM, which may come later)") : source);
    }
    audioConfig->show();
    audioConfig->raise();
}

void MainWindow::stopEfmDecoders()
{
    stopSidecars();
    for (EfmDecoding *&decoder: efmDecoders) {
        delete decoder;
        decoder = nullptr;
    }
    if (audioConfig) {
        audioConfig->setDecodeStatus(0, QString(), false);
        audioConfig->setDecodeStatus(1, QString(), false);
    }
}

// The EFM of the capture that can be decoded: an LDX file that has some, or the .efm beside a .tbc; empty if there is none
QString MainWindow::efmSourceName()
{
    // (a live input: "-", whatever it has sent yet -- its EFM may come later, and the decoders wait for it)
    if (tbcSource.getIsSourceLoaded() && tbcSource.getIsLive() && LdxStream::instance() && liveEfm) return QStringLiteral("-");
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return QString();
    const QString name = tbcSource.getCurrentSourceFilename();
    if (LdxCapture::isLdxName(name)) {
        QString path;
        qint32 track = 0;
        LdxCapture::split(name, path, track);
        LdxByteTrack probe;
        QString error;
        return probe.openFile(path, LdxByteTrack::Kind::Efm, error) && probe.size() > 0 ? path : QString();      // (a track with no EFM in it, as a producer may always write, is none)
    }
    const QFileInfo video(name);
    const QString efm = video.absolutePath() + "/" + video.completeBaseName() + ".efm";
    return QFileInfo::exists(efm) ? efm : QString();
}

// Decode the EFM with one of the two decoders (0: the efm-decoder suite, 1: ld-process-efm): its audio is a track as soon as it
// comes, and the waveform and the sound follow it
void MainWindow::startEfmDecoding(qint32 decoder, bool noTimecodes, bool conceal, bool pad, bool select)
{
    const QString source = efmSourceName();
    if (source.isEmpty()) return;
    const double playingAt = audio->isPlaying() ? audio->videoSecondsNow() : -1.0;
    const bool chosenByDefault = efmByDefault;
    const EfmDecoding::Backend backend = decoder == 0 ? EfmDecoding::Backend::Suite : EfmDecoding::Backend::ProcessEfm;
    // One decoder at a time: the other one is stopped, and its track goes
    const qint32 other = 1 - decoder;
    if (efmDecoders[other]) {
        audio->removeDecoded(other);
        delete efmDecoders[other];
        efmDecoders[other] = nullptr;
        if (audioConfig) audioConfig->setDecodeStatus(other, tr("Stopped"), false);
    }
    audioStop();
    audio->removeDecoded(decoder);
    delete efmDecoders[decoder];
    efmDecoders[decoder] = new EfmDecoding(backend, this);
    EfmDecoding *decoding = efmDecoders[decoder];
    EfmDecoding::Options options;
    options.noTimecodes = noTimecodes;
    options.conceal = conceal;
    options.pad = pad;
    // A live input goes on for as long as it likes: the audio that is decoded from it is kept as a ring too
    std::shared_ptr<AudioRing> decodedRing;
    if (source == "-") {
        decodedRing = std::make_shared<AudioRing>(120.0, 44100.0);
        decoding->setRing(decodedRing);
    }
    QString error;
    if (!decoding->start(source, options, error)) {
        if (audioConfig) audioConfig->setDecodeStatus(decoder, error, false);
        statusBar()->showMessage(error, 8000);
        return;
    }
    // A live input: the decoder begins a few seconds before where the video is, not at the start of what has come (which would be
    // decoded again at each change of decoder, with the sound waiting for it). The audio it makes begins at that moment of the capture.
    const bool live = (source == "-");
    double startSeconds = 0.0;
    qint64 startByte = 0;
    liveEfmStartPending[decoder] = false;
    if (live) {
        const double videoNow = positionSeconds(tbcSource.getFieldViewEnabled() || weaveActive ? currentFieldNumber : currentFrameNumber);
        const qint64 wanted = static_cast<qint64>(qMax(0.0, videoNow - 3.0) * 1.0e9);
        if (liveEfmIndex.isEmpty()) {
            liveEfmStartPending[decoder] = true;                // (no EFM yet: it begins with the first that comes)
        } else {
            qint32 chosen = liveEfmIndex.size() - 1;
            for (qint32 i = 0; i < liveEfmIndex.size(); i++) {
                if (liveEfmIndex[i].second >= wanted) { chosen = i; break; }
            }
            startByte = liveEfmIndex[chosen].first;
            startSeconds = static_cast<double>(liveEfmIndex[chosen].second) / 1.0e9;
        }
    }
    const QString trackName = tr("%1 (decoding...)").arg(EfmDecoding::backendName(backend));
    const qint32 index = decodedRing ? audio->addRing(decoder, "efm", decodedRing, trackName, startSeconds)
                                     : audio->addDecoded(decoder, decoding->cacheFile(), trackName, startSeconds);
    if (select) {
        waveform->setTracks(audio->tracks(), index);
        audio->selectTrack(index);
        // (chosen for the person on opening: the waveform is shown or not as they left it)
        if (!chosenByDefault) {
            actionWaveform->setEnabled(true);
            {
                const QSignalBlocker blocker(actionWaveform);
                actionWaveform->setChecked(true);
            }
            waveformWanted = true;
            showWaveform(true);
        }
        audioFollowVideo(playingAt);
    } else {
        // (decoded in the background: the track is there to be chosen, and what is played stays)
        waveform->setTracks(audio->tracks(), audio->currentTrack());
    }
    if (live) {
        liveEfmFed[decoder] = startByte;
        feedLiveEfm();
    }
    if (audioConfig) audioConfig->setDecodeStatus(decoder, live && liveEfm && liveEfm->endByte() == 0 ? tr("Waiting for EFM...") : tr("Decoding..."), true);
    connect(decoding, &EfmDecoding::grew, this, [this, decoder, decoding]() {
        audio->extend();
        if (audioConfig) audioConfig->setDecodeStatus(decoder, tr("Decoding: %1 seconds").arg(decoding->seconds(), 0, 'f', 1), true);
    });
    connect(decoding, &EfmDecoding::finished, this, [this, decoder, decoding, chosenByDefault](bool ok, const QString &message) {
        audio->extend();
        // The digital audio was chosen for the person and the EFM gave none: the analogue audio is heard again
        if (chosenByDefault && (!ok || decoding->seconds() <= 0.0)) {
            const QVector<AudioTrackInfo> &tracks = audio->tracks();
            const qint32 current = audio->currentTrack();
            if (current >= 0 && current < tracks.size() && tracks[current].source == QLatin1String("efm")) {
                for (qint32 i = 0; i < tracks.size(); i++) {
                    if (tracks[i].source == QLatin1String("analogue")) {
                        chooseAudioTrack(i);
                        break;
                    }
                }
            }
        }
        if (audioConfig) audioConfig->setDecodeStatus(decoder, message, false);
        statusBar()->showMessage(message, 8000);
        waveform->refresh();
    });
}

// The imported audio goes into the LDX file of the capture as a pcm track
void MainWindow::saveImportedAudio(const QVector<qint16> &samples, double sampleRate, const QString &name)
{
    QString path;
    qint32 track = 0;
    LdxCapture::split(tbcSource.getCurrentSourceFilename(), path, track);
    QTemporaryFile raw;
    if (path.isEmpty() || !raw.open() || raw.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2) != samples.size() * 2) {
        statusBar()->showMessage(tr("The audio could not be saved in the file"), 8000);
        return;
    }
    raw.close();
    audioStop();
    audio->setVideo(QString());          // (the file is not held open by the audio)
    QJsonObject parameters;
    parameters["sampleRate"] = sampleRate;
    parameters["channels"] = 2;
    parameters["source"] = "import";
    QString error;
    const bool added = LdxCapture::addTrackFromFile(path, "pcm", raw.fileName(), parameters, name, 0,
                                                    LdxCapture::historyEntry("ld-analyse", tr("audio imported from %1").arg(name)), error);
    statusBar()->showMessage(added ? tr("The audio is saved in %1; open the file again to use it from there").arg(path)
                                   : tr("The audio could not be saved: %1").arg(error), 10000);
    audioLoadedKey.clear();
    if (added) loadAudio();
}

// A new height for the waveform panel: the window follows when the panel is shown
void MainWindow::applyWaveformDensity(qint32 density)
{
    const qint32 before = waveform->maximumHeight();
    waveform->setDensity(static_cast<WaveformPanel::Density>(density));
    if (!waveform->isVisibleTo(this) || !isVisible() || isMaximized() || isFullScreen()) return;
    resize(width(), qMax(minimumHeight(), height() + waveform->maximumHeight() - before));
}

// Show or hide the waveform: the window grows or shrinks by its height, so that the video keeps its room
void MainWindow::showWaveform(bool shown)
{
    if (waveform->isVisibleTo(this) == shown) return;
    const qint32 height = qMin(waveform->sizeHint().height(), waveform->maximumHeight()) + ui->verticalLayout->spacing();
    waveform->setVisible(shown);
    if (!isVisible() || isMaximized() || isFullScreen()) return;
    resize(width(), qMax(minimumHeight(), this->height() + (shown ? height : -height)));
}

// A capture has been loaded: its audio, if it has any, is what the waveform shows
void MainWindow::loadAudio()
{
    // (the view changes (frame, field, deinterlaced) call this too: what is loaded for this source stays, with its decoders)
    const QString key = QString(tbcSource.getIsLive() ? "live:" : "file:") + tbcSource.getCurrentSourceFilename();
    if (key == audioLoadedKey) {
        updateAudioPlayhead();
        return;
    }
    stopEfmDecoders();
    if (tbcSource.getIsLive() && LdxStream::instance()) {
        loadLiveAudio();
        audioLoadedKey = key;
        return;
    }
    const bool usable = !tbcSource.getIsLive();
    const QVector<AudioTrackInfo> tracks = usable ? audio->setVideo(tbcSource.getCurrentSourceFilename()) : audio->setVideo(QString());
    // The audio files beside the capture with its name (a reference recording, the audio of another decoder), and its EFM: opened by
    // themselves, unless that is switched off in the Audio panel
    const bool beside = usable && configuration.audioValue("sidecars", true).toBool();
    const QStringList sidecars = beside ? SidecarAudio::find(tbcSource.getCurrentSourceFilename()) : QStringList();
    if (tracks.isEmpty() && sidecars.isEmpty()) {
        unloadAudio();
        return;
    }
    // The digital audio first if there is some (decoded from the EFM, in the file) and that is what is wanted, else the analogue audio of the capture
    const bool preferDigital = configuration.audioValue("preferDigital", true).toBool();
    qint32 first = 0;
    for (qint32 i = 0; i < tracks.size(); i++) {
        if (tracks[i].source == "analogue") { first = i; break; }
    }
    if (preferDigital) {
        for (qint32 i = 0; i < tracks.size(); i++) {
            if (tracks[i].source == "efm") { first = i; break; }
        }
    }
    // The analogue audio is cut to the fields: the metadata say where each field begins in it
    {
        const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
        audio->setFieldSamples(tbcSource.getAudioSamplesBeforeFields(), 2.0 * frameRate);
    }
    if (!tracks.isEmpty()) audio->selectTrack(first);
    startSidecars(sidecars);
    if (tracks.isEmpty()) first = 0;
    if (tracks.isEmpty()) audio->selectTrack(0);
    waveform->setTracks(audio->tracks(), first);
    audioLoadedKey = key;
    actionWaveform->setEnabled(true);
    const QSignalBlocker blocker(actionWaveform);
    actionWaveform->setChecked(waveformWanted);
    showWaveform(waveformWanted);
    updateAudioPlayhead();
    // The EFM beside the capture is decoded in the background, by the decoder that is chosen, unless the file has its audio already
    if (beside) {
        bool hasEfmAudio = false;
        for (const AudioTrackInfo &track: audio->tracks()) hasEfmAudio |= track.source == "efm";
        if (!hasEfmAudio && !efmSourceName().isEmpty()) {
            // (the audio it gives is the one that is heard, when that is wanted: if it gives none the analogue audio is chosen again)
            efmByDefault = preferDigital;
            startEfmDecoding(audioConfig ? audioConfig->currentDecoder() : configuration.audioValue("decoder", 1).toInt(), true, true, false, preferDigital);
            efmByDefault = false;
        }
    }
}

// The files of audio beside the capture, decoded by ffmpeg in the background as tracks that grow: the Imported source of the panel
void MainWindow::startSidecars(const QStringList &files)
{
    stopSidecars();
    const qint32 firstSlot = 3;       // (0 and 1 are the EFM decoders, 2 the analogue audio of a live input)
    for (qint32 i = 0; i < files.size() && i < 12; i++) {
        auto *decoding = new AudioFileDecoding(this);
        QString error;
        if (!decoding->start(files[i], error)) {
            statusBar()->showMessage(error, 8000);
            delete decoding;
            continue;
        }
        const QString name = QFileInfo(files[i]).fileName();
        audio->addGrowing(firstSlot + i, "import", decoding->cacheFile(), tr("%1 (beside the capture)").arg(name), 44100.0);
        connect(decoding, &AudioFileDecoding::grew, this, [this]() { audio->extend(); });
        connect(decoding, &AudioFileDecoding::finished, this, [this](bool ok, const QString &message) {
            audio->extend();
            if (!ok) statusBar()->showMessage(message, 10000);
        });
        sidecarDecodings.append(decoding);
    }
}

void MainWindow::stopSidecars()
{
    qDeleteAll(sidecarDecodings);
    sidecarDecodings.clear();
}

// A live LDX input: what its tracks other than the video carry. Its audio is written, as it comes, to a file that the player and the
// waveform read while it grows, and its EFM is kept for the decoders. The EFM may come late, or not at all: the decoders can be started
// at any time, and read what came and what comes.
void MainWindow::loadLiveAudio()
{
    liveAnalogue.reset();
    liveEfm.reset();
    liveKinds.clear();
    livePcmTrack = 0;
    liveEfmFed[0] = liveEfmFed[1] = 0;
    liveEfmIndex.clear();
    liveEfmBytes = 0;
    liveEfmStartPending[0] = liveEfmStartPending[1] = false;
    livePcmStartSet = false;
    audio->setVideo(QString());

    liveEfm = std::make_unique<EfmByteRing>();

    for (const LdxStream::OtherTrack &track: LdxStream::instance()->otherTracks()) {
        qInfo().noquote() << "Live input: track" << track.number << track.kind << track.name;
        liveKinds.insert(track.number, track.kind);
        if (track.kind == "pcm" && livePcmTrack == 0 && track.parameters.value("source").toString() != "efm") {
            livePcmTrack = track.number;
            livePcmBits = track.depth;
            livePcmRate = track.parameters.value("sampleRate").toDouble(44100.0);
        }

    }


    if (livePcmTrack != 0) {
        liveAnalogue = std::make_shared<AudioRing>(120.0, livePcmRate);
        const qint32 index = audio->addRing(2, "analogue", liveAnalogue, tr("Analogue (live)"));
        waveform->setTracks(audio->tracks(), index);
        audio->selectTrack(index);
        actionWaveform->setEnabled(true);
        const QSignalBlocker blocker(actionWaveform);
        actionWaveform->setChecked(waveformWanted);
        showWaveform(waveformWanted);
        waveform->setMessage(tr("Waiting for audio..."));
    } else {
        showWaveform(false);
        actionWaveform->setEnabled(false);
    }

    if (!liveAudioTimer) {
        liveAudioTimer = new QTimer(this);
        liveAudioTimer->setInterval(100);
        connect(liveAudioTimer, &QTimer::timeout, this, &MainWindow::pollLiveTracks);
    }
    liveAudioTimer->start();
}

void MainWindow::pollLiveTracks()
{
    LdxStream *stream = LdxStream::instance();
    if (!stream || !liveEfm) return;
    bool audioCame = false, efmCame = false;
    static bool reported = false;
    const QVector<LdxStream::OtherBlock> blocks = stream->takeOtherBlocks();
    if (!blocks.isEmpty() && !reported) {
        reported = true;
        qInfo().noquote() << "Live input: other tracks began to arrive, first block of track" << blocks.first().track << "kind" << liveKinds.value(blocks.first().track) << "with" << blocks.first().data.size() << "bytes";
    }
    for (const LdxStream::OtherBlock &block: blocks) {
        const QString kind = liveKinds.value(block.track);
        if (kind == "pcm" && block.track == livePcmTrack && liveAnalogue) {
            QByteArray pairs = block.data;
            if (livePcmBits == 24) {
                // (the top 16 bits of each 24-bit sample)
                QByteArray narrow;
                narrow.resize(block.data.size() / 3 * 2);
                for (qint32 i = 0; i < narrow.size() / 2; i++) {
                    narrow[2 * i] = block.data[3 * i + 1];
                    narrow[2 * i + 1] = block.data[3 * i + 2];
                }
                pairs = narrow;
            }
            // (the first sample of the track is where its first block is in the capture, which may not be the start)
            if (!livePcmStartSet) {
                livePcmStartSet = true;
                audio->setGrowingStart(2, static_cast<double>(block.timeNs) / 1.0e9);
            }
            liveAnalogue->append(pairs);
            audioCame = true;
        } else if (kind == "efm") {
            // Where in the capture each piece of EFM is, to decode from a place of it (a decoder started later need not begin at the start)
            liveEfmIndex.append({ liveEfmBytes, block.timeNs });
            liveEfmBytes += block.data.size();
            for (qint32 i = 0; i < 2; i++) {
                if (efmDecoders[i] && liveEfmStartPending[i]) {
                    // (a decoder that was started before any EFM: its audio begins with this)
                    liveEfmStartPending[i] = false;
                    liveEfmFed[i] = liveEfmIndex.last().first;
                    audio->setGrowingStart(i, static_cast<double>(block.timeNs) / 1.0e9);
                }
            }
            liveEfm->append(block.data);
            efmCame = true;
        }
    }
    if (audioCame) {
        waveform->setMessage(QString());
        audio->extend();
    }
    feedLiveEfm();
    // Only the last part of the EFM is kept, as for the video of a live input: what is older than that is let go (a decoder that is
    // started, or changed, begins a few seconds before the video, not at the start)
    if (efmCame && !liveEfmIndex.isEmpty()) {
        const qint64 keepFrom = liveEfmIndex.last().second - static_cast<qint64>(30.0e9);
        qint32 drop = 0;
        while (drop < liveEfmIndex.size() - 1 && liveEfmIndex[drop].second < keepFrom) drop++;
        if (drop > 0) {
            liveEfm->trimBefore(liveEfmIndex[drop].first);
            liveEfmIndex.remove(0, drop);
        }
    }
    // The end of the stream: the decoders are told there is no more
    if (tbcSource.getIsLiveEnded() && liveEfm) {
        feedLiveEfm();
        for (EfmDecoding *decoder: efmDecoders) {
            if (decoder && decoder->isRunning() && liveEfmFed[decoder == efmDecoders[0] ? 0 : 1] >= liveEfm->endByte()) decoder->finishInput();
        }
    }
}

// The EFM that came and was not yet given to a decoder that runs on the live input
void MainWindow::feedLiveEfm()
{
    if (!liveEfm) return;
    for (qint32 i = 0; i < 2; i++) {
        if (!efmDecoders[i] || !efmDecoders[i]->isRunning() || liveEfmFed[i] >= liveEfm->endByte()) continue;
        // (a decoder that fell behind by more than what is kept goes on from the oldest that is there)
        if (liveEfmFed[i] < liveEfm->firstByte()) liveEfmFed[i] = liveEfm->firstByte();
        const QByteArray data = liveEfm->read(liveEfmFed[i], 4 << 20);
        efmDecoders[i]->feed(data);
        liveEfmFed[i] += data.size();
    }
}

// No capture, or one with no audio: nothing to show or to play
void MainWindow::unloadAudio()
{
    audioLoadedKey.clear();
    stopEfmDecoders();
    if (liveAudioTimer) liveAudioTimer->stop();
    liveAnalogue.reset();
    liveEfm.reset();
    audioStop();
    audio->setVideo(QString());
    showWaveform(false);
    actionWaveform->setEnabled(false);
}

double MainWindow::positionSeconds(qint32 position)
{
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    const double rate = tbcSource.getFieldViewEnabled() || weaveActive ? 2.0 * frameRate : frameRate;
    return (position - 1) / rate;
}

// Play the sound from a position of the video, and let the video keep to it. False if there is no sound to play.
bool MainWindow::audioStart(qint32 position)
{
    return audioStartAt(positionSeconds(position));
}

// The sound goes on, or begins, from a moment of the video (a track was chosen, or a decoder began, while the video plays); a negative
// moment is the one the video is at
void MainWindow::audioFollowVideo(double videoSeconds)
{
    if (!playback->isPlaying()) return;
    audioStop();
    audioStartAt(videoSeconds >= 0.0 ? videoSeconds : positionSeconds(tbcSource.getFieldViewEnabled() || weaveActive ? currentFieldNumber : currentFrameNumber));
}

bool MainWindow::audioStartAt(double videoSeconds)
{
    if (!audio->hasAudio() || audio->currentTrack() < 0 || playback->getSpeed() != 1.0) return false;
    if (!audio->start(videoSeconds)) return false;
    // (a live input keeps its own clock: the sound is behind the picture at the edge of what has come, and must not hold it back)
    if (!tbcSource.getIsLive()) playback->setExternalClock([this]() { return audio->playedSeconds(); });
    return true;
}

void MainWindow::audioStop()
{
    if (!audio) return;
    audio->stop();
    playback->setExternalClock(nullptr);
}

void MainWindow::updateAudioPlayhead()
{
    if (!audio || !waveform || !waveform->isVisible()) return;
    const qint32 position = tbcSource.getFieldViewEnabled() || weaveActive ? currentFieldNumber : currentFrameNumber;
    waveform->setPlayheadSeconds(positionSeconds(position));
}

// A click on the waveform: go to that place of the video
void MainWindow::waveformSeek(double seconds)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    pausePlayback();
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    const double rate = tbcSource.getFieldViewEnabled() ? 2.0 * frameRate : frameRate;
    const qint32 last = tbcSource.getFieldViewEnabled() ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames();
    ui->posHorizontalSlider->setValue(qBound(1, static_cast<qint32>(seconds * rate) + 1, qMax(1, last)));
}
#endif // LDDECODE_HAVE_AUDIO

// Seeking by the keyboard ------------------------------------------------------------------------------------------------------------
// The keys of a jog and a shuttle, as a laserdisc player has them: the arrows step (held, they repeat: a jog), with Shift, Alt and the
// page keys they go further, and J, K and L are a shuttle (J backward, L forward, each press faster, K stops).

// What the controls count in: the field in the field view, the frame in the others
qint32 MainWindow::seekPosition()
{
    return tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
}

qint32 MainWindow::seekLast()
{
    return tbcSource.getFieldViewEnabled() ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames();
}

qint32 MainWindow::seekFirst()
{
    if (!tbcSource.getIsLive()) return 1;
    const qint32 firstFrame = tbcSource.getFirstAvailableFrame();
    return tbcSource.getFieldViewEnabled() ? 2 * firstFrame - 1 : firstFrame;
}

// A second, in those units (the frame rate is 25 or 30, rounded)
qint32 MainWindow::seekUnitsPerSecond()
{
    return (tbcSource.getSystem() == PAL ? 25 : 30) * (tbcSource.getFieldViewEnabled() ? 2 : 1);
}

// Go to a position (kept to what there is): the playback stops, as when the buttons are used
void MainWindow::seekTo(qint32 position)
{
    if (!tbcSource.getIsSourceLoaded()) return;
    endStill();
    // (a live input can not be sought while it plays)
    if (tbcSource.getIsLive() && playback->isPlaying()) return;
    pausePlayback();
    const qint32 first = seekFirst();
    position = qBound(first, position, qMax(first, seekLast()));
    if (position == seekPosition()) return;
    if (tbcSource.getFieldViewEnabled()) setCurrentField(position); else setCurrentFrame(position);
    const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
    const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
    ui->posNumberSpinBox->setValue(position);
    ui->posHorizontalSlider->setValue(position);
}

void MainWindow::seekBy(qint32 units)
{
    seekTo(seekPosition() + units);
}

// The shuttle: a scan at a multiple of the normal speed, forward or backward, showing the pictures that are due as the clock goes (the
// others are not shown), without the colour while it lasts
void MainWindow::startScan(double speed)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    pausePlayback();
#ifdef LDDECODE_HAVE_AUDIO
    audioStop();
#endif
    if (!scanChromaSuppressed && qAbs(speed) > 2.0 && tbcSource.getChromaDecoder()) {
        scanChromaSuppressed = true;
        tbcSource.setChromaDecoder(false);
    }
    scanSpeed = speed;
    scanRemainder = 0.0;
    scanClock.start();
    scanTimer->start();
    statusBar()->showMessage(tr("Scan %1%2x").arg(speed < 0 ? QString("-") : QString("+")).arg(qAbs(speed)), 0);
    updateRemote();
}

void MainWindow::stopScan()
{
    if (scanSpeed == 0.0) return;
    scanTimer->stop();
    scanSpeed = 0.0;
    statusBar()->clearMessage();
    if (scanChromaSuppressed) {
        scanChromaSuppressed = false;
        tbcSource.setChromaDecoder(true);
        showImage();
    }
    updateRemote();
}

void MainWindow::scanTick()
{
    if (scanSpeed == 0.0) return;
    const double seconds = scanClock.restart() / 1000.0;
    const double units = scanSpeed * seekUnitsPerSecond() * seconds + scanRemainder;
    const qint32 step = static_cast<qint32>(units);
    scanRemainder = units - step;
    if (step == 0) return;
    const qint32 before = seekPosition();
    seekTo(before + step);
    // (at the start or at the end: the scan is over)
    if (seekPosition() == before) stopScan();
}

// A press of J (direction -1) or L (+1): from rest it plays at the normal speed (forward) or scans at it (backward), and each press in
// the same direction doubles the speed, up to 32 times; against the movement it goes the other way at the normal speed
void MainWindow::shuttle(qint32 direction)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    static const double ladder[] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    const double current = scanSpeed != 0.0 ? scanSpeed : (playback->isPlaying() ? 1.0 : 0.0);
    double next = direction;
    if (current != 0.0 && (current > 0) == (direction > 0)) {
        qint32 level = 0;
        while (level < 5 && ladder[level] < qAbs(current)) level++;
        next = direction * ladder[qMin(5, level + 1)];
    }
    if (next == 1.0) {
        // (forward at the normal speed is the playback)
        stopScan();
        if (!playback->isPlaying()) togglePlayback();
    } else {
        startScan(next);
    }
}

void MainWindow::shuttleStop()
{
    stopScan();
    pausePlayback();
}

// The actions of the keys, in Playback > Seek
void MainWindow::setupSeekActions()
{
    scanTimer = new QTimer(this);
    scanTimer->setInterval(25);
    scanTimer->setTimerType(Qt::PreciseTimer);
    connect(scanTimer, &QTimer::timeout, this, &MainWindow::scanTick);

    const auto add = [this](const QString &id, const QString &text, const QKeySequence &key, auto &&handler) {
        QAction *action = new QAction(text, this);
        addAction(action);                    // (not in a menu: it works wherever the window is, as the remote control has the keys too)
        remoteActions.append(action);
        action->setObjectName(id);
        action->setShortcut(key);
        action->setAutoRepeat(true);          // (held, a key goes on: a jog)
        connect(action, &QAction::triggered, this, handler);
        return action;
    };
    add("seekStepBack", tr("Step back"), Qt::Key_Left, [this]() { stopScan(); seekBy(-1); });
    add("seekStepForward", tr("Step forward"), Qt::Key_Right, [this]() { stopScan(); seekBy(1); });
    add("seekBack10", tr("Back 10"), Qt::SHIFT | Qt::Key_Left, [this]() { stopScan(); seekBy(-10); });
    add("seekForward10", tr("Forward 10"), Qt::SHIFT | Qt::Key_Right, [this]() { stopScan(); seekBy(10); });
    add("seekBackSecond", tr("Back one second"), Qt::ALT | Qt::Key_Left, [this]() { stopScan(); seekBy(-seekUnitsPerSecond()); });
    add("seekForwardSecond", tr("Forward one second"), Qt::ALT | Qt::Key_Right, [this]() { stopScan(); seekBy(seekUnitsPerSecond()); });
    add("seekBackMinute", tr("Back one minute"), Qt::Key_PageUp, [this]() { stopScan(); seekBy(-60 * seekUnitsPerSecond()); });
    add("seekForwardMinute", tr("Forward one minute"), Qt::Key_PageDown, [this]() { stopScan(); seekBy(60 * seekUnitsPerSecond()); });
    add("seekStart", tr("Go to the start"), Qt::Key_Home, [this]() { stopScan(); seekTo(seekFirst()); });
    add("seekEnd", tr("Go to the end"), Qt::Key_End, [this]() { stopScan(); seekTo(seekLast()); });
    add("shuttleBackward", tr("Shuttle backward (each press faster)"), Qt::Key_J, [this]() { shuttle(-1); })->setAutoRepeat(false);
    add("shuttleStop", tr("Shuttle stop"), Qt::Key_K, [this]() { shuttleStop(); })->setAutoRepeat(false);
    add("shuttleForward", tr("Shuttle forward (each press faster)"), Qt::Key_L, [this]() { shuttle(1); })->setAutoRepeat(false);
    // What the remote control has besides: the chapters, the memory, the sound
    add("chapterPrevious", tr("Chapter before (or the start of this one)"), Qt::CTRL | Qt::Key_Left, [this]() { skipChapter(-1); })->setAutoRepeat(false);
    add("chapterNext", tr("Next chapter"), Qt::CTRL | Qt::Key_Right, [this]() { skipChapter(1); })->setAutoRepeat(false);
    add("rememberPlace", tr("One-shot memory (remember the place, then go back to it)"), Qt::Key_M, [this]() { toggleMemory(); })->setAutoRepeat(false);
    add("digitalAnalogue", tr("Digital / analogue sound"), Qt::Key_D, [this]() { toggleDigitalAnalog(); })->setAutoRepeat(false);
    add("stillWithSound", tr("Still with sound (the picture stays, the sound goes on)"), Qt::Key_S, [this]() { toggleStillWithSound(); })->setAutoRepeat(false);
    setupRemote();
}

// The remote control ------------------------------------------------------------------------------------------------------------------
// A remote of a laserdisc player, in a window of its own (View > Remote control) or, when the picture fills the screen, over the picture
// at its left. The numbers of the disc (the time, the chapter, the picture) can be over the picture too, at its right. In the full screen both
// go away after a few seconds without the mouse moving, and come back with it.

void MainWindow::setupRemote()
{
    remote = new RemoteWidget();
    // (the windows have no frame of the system: their own title bar has the pin, which keeps them when the others go away, and the close button)
    remoteWindow = new QDialog(this, Qt::Tool | Qt::FramelessWindowHint);
    remoteWindow->setObjectName("remoteWindow");
    remoteWindow->setWindowTitle(tr("Remote control"));
    remoteWindow->setStyleSheet("QDialog#remoteWindow { background: #26262a; border: 1px solid #0c0c0e; border-radius: 6px; }");
    QVBoxLayout *remoteLayout = new QVBoxLayout(remoteWindow);
    remoteLayout->setContentsMargins(1, 1, 1, 1);
    remoteLayout->setSpacing(0);
    remoteTitleBar = new PinTitleBar(tr("Remote control"), remoteWindow);
    remoteTitleBar->setPinToolTip(tr("Pinned: the remote stays when the other controls go away in the full screen. Not pinned: it goes with them."));
    remoteTitleBar->setPinned(configuration.playbackValue("remotePinned", false).toBool());
    connect(remoteTitleBar, &PinTitleBar::pinToggled, this, [this](bool pinned) { configuration.setPlaybackValue("remotePinned", pinned); });
    connect(remoteTitleBar, &PinTitleBar::closeRequested, remoteWindow, &QDialog::reject);
    remoteLayout->addWidget(remoteTitleBar);
    remoteLayout->addWidget(remote);
    // (the keys of the keyboard work when the remote is the window that has them)
    for (QAction *action: remoteActions) remoteWindow->addAction(action);
    // (not narrower than its keys are: a window kept from before could be squeezed)
    remote->setMinimumSize(250, 540);
    remoteWindow->adjustSize();
    remoteWindow->restoreGeometry(configuration.playbackValue("remoteGeometry").toByteArray());

    // The numbers of the disc: a window of its own too, which floats where it is put (in the full screen as well)
    infoPanel = new InfoPanel(this);
    infoPanel->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    infoPanel->setWindowTitle(tr("Disc numbers"));
    infoPanel->setAttribute(Qt::WA_ShowWithoutActivating);
    remoteWindow->setAttribute(Qt::WA_ShowWithoutActivating);
    setupSeekPanel();
    infoPanel->hide();
    infoPanel->setMinimumWidth(230);
    if (!infoPanel->restoreGeometry(configuration.playbackValue("infoGeometry").toByteArray())) infoPanel->move(geometry().topRight() - QPoint(280, -80));
    connect(infoPanel, &InfoPanel::closed, this, [this]() { actionInfoDisplay->setChecked(false); });
    // (it stays on screen unless the person asked for it to go with the others)
    infoPanel->setAutoHide(configuration.playbackValue("infoAutoHide", false).toBool());
    connect(infoPanel, &InfoPanel::autoHideChanged, this, [this](bool on) {
        configuration.setPlaybackValue("infoAutoHide", on);
        if (fullScreenOn) showOverlays();
    });

    overlayTimer = new QTimer(this);
    overlayTimer->setSingleShot(true);
    overlayTimer->setInterval(2500);
    connect(overlayTimer, &QTimer::timeout, this, &MainWindow::hideOverlays);
    cursorPoll = new QTimer(this);
    cursorPoll->setInterval(100);
    connect(cursorPoll, &QTimer::timeout, this, &MainWindow::pollCursor);
    searchMessageTimer = new QTimer(this);
    searchMessageTimer->setSingleShot(true);
    searchMessageTimer->setInterval(2000);
    connect(searchMessageTimer, &QTimer::timeout, this, [this]() { searchMessage.clear(); updateRemote(); });

    // The View menu: the remote, the numbers over the picture, the full screen
    ui->menuView->addSeparator();
    actionRemote = ui->menuView->addAction(tr("&Remote control"));
    actionRemote->setObjectName("actionRemote");
    actionRemote->setCheckable(true);
    actionRemote->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
    actionInfoDisplay = ui->menuView->addAction(tr("Disc &numbers over the picture"));
    actionInfoDisplay->setObjectName("actionInfoDisplay");
    actionInfoDisplay->setCheckable(true);
    actionInfoDisplay->setShortcut(Qt::Key_I);
    actionFullScreen = ui->menuView->addAction(tr("&Full screen"));
    actionFullScreen->setObjectName("actionFullScreen");
    actionFullScreen->setCheckable(true);
    actionFullScreen->setShortcut(Qt::Key_F11);
    // (the remote is in the Window menu too)
    ui->menuWindow->addSeparator();
    ui->menuWindow->addAction(actionRemote);
    ui->menuWindow->addAction(actionInfoDisplay);
    remoteWindow->addAction(actionRemote);
    remoteWindow->addAction(actionInfoDisplay);
    remoteWindow->addAction(actionFullScreen);

    // (in the full screen they go away when the mouse is still: overlaysHiddenNow)
    connect(actionRemote, &QAction::toggled, this, [this](bool on) {
        remoteWindow->setVisible(on && !(overlaysHiddenNow && !remoteTitleBar->pinned()));
        if (!fullScreenOn) configuration.setPlaybackValue("remoteShown", on);
    });
    connect(remoteWindow, &QDialog::rejected, this, [this]() { actionRemote->setChecked(false); });
    connect(actionInfoDisplay, &QAction::toggled, this, [this](bool on) {
        remote->setDisplayOn(on);
        infoPanel->setVisible(on && !(overlaysHiddenNow && infoPanel->autoHide()));
        updateInfoPanel();
        if (!fullScreenOn) configuration.setPlaybackValue("infoDisplay", on);
    });
    connect(actionFullScreen, &QAction::toggled, this, &MainWindow::setFullScreen);

    // The keys of the remote
    connect(remote, &RemoteWidget::digitPressed, this, [this](qint32 digit) { keypad.digit(digit); searchMessage.clear(); updateRemote(); });
    connect(remote, &RemoteWidget::plusTenPressed, this, [this]() { keypad.plusTen(); searchMessage.clear(); updateRemote(); });
    connect(remote, &RemoteWidget::clearPressed, this, [this]() { keypad.clear(); searchMessage.clear(); updateRemote(); });
    connect(remote, &RemoteWidget::modePressed, this, [this]() {
        keypad.setMode(RemoteKeypad::nextMode(keypad.mode(), tbcSource.hasChapters(), tbcSource.hasPictureNumbers(), tbcSource.hasTimes()));
        searchMessage.clear();
        updateRemote();
    });
    connect(remote, &RemoteWidget::searchPressed, this, [this]() { remoteSearch(false); });
    connect(remote, &RemoteWidget::frameSearchPressed, this, [this]() { remoteSearch(true); });
    connect(remote, &RemoteWidget::programPressed, this, &MainWindow::showChapterList);
    connect(remote, &RemoteWidget::filterPressed, this, [this]() { setFiltersEnabled(!filtersEnabled); });
    connect(remote, &RemoteWidget::playPressed, this, [this]() { moveAt(1.0); });
    connect(remote, &RemoteWidget::pausePressed, this, [this]() { endStill(); stopScan(); pausePlayback(); updateRemote(); });
    connect(remote, &RemoteWidget::stepPressed, this, [this](qint32 direction) { stopScan(); seekBy(direction); });
    connect(remote, &RemoteWidget::skipPressed, this, &MainWindow::skipChapter);
    connect(remote, &RemoteWidget::multiSpeedPressed, this, [this](qint32 direction) {
        multiSpeed = RemoteSpeed::step(multiSpeed, direction);
        const double now = scanSpeed != 0.0 ? scanSpeed : (playback->isPlaying() ? playback->getSpeed() : 0.0);
        if (now != 0.0) moveAt(now < 0 ? -multiSpeed : multiSpeed);
        statusBar()->showMessage(tr("Speed %1").arg(RemoteSpeed::text(multiSpeed)), 2000);
        updateRemote();
    });
    connect(remote, &RemoteWidget::directionPressed, this, [this](qint32 direction) { moveAt(direction * multiSpeed); });
    connect(remote, &RemoteWidget::jogged, this, [this](qint32 steps) { stopScan(); seekBy(steps); });
    connect(remote, &RemoteWidget::shuttled, this, [this](double deflection) {
        const double speed = RemoteSpeed::fromDeflection(deflection);
        if (speed == shuttleSpeedNow) return;
        shuttleSpeedNow = speed;
        if (speed == 0.0) {
            stopScan();
            pausePlayback();
            updateRemote();
        } else {
            moveAt(speed);
        }
    });
    connect(remote, &RemoteWidget::jogModeChanged, this, [this](bool jog) { configuration.setPlaybackValue("jogMode", jog); });
    connect(remote, &RemoteWidget::displayPressed, this, [this]() { actionInfoDisplay->toggle(); });
    connect(remote, &RemoteWidget::digitalAnalogPressed, this, &MainWindow::toggleDigitalAnalog);
    connect(remote, &RemoteWidget::repeatAPressed, this, [this]() { markRepeat(false); });
    connect(remote, &RemoteWidget::repeatBPressed, this, [this]() { markRepeat(true); });
    connect(remote, &RemoteWidget::memoryPressed, this, &MainWindow::toggleMemory);
    connect(remote, &RemoteWidget::stillWithSoundPressed, this, &MainWindow::toggleStillWithSound);
    connect(remote, &RemoteWidget::chromaPressed, this, [this]() { on_videoPushButton_clicked(); updateRemote(); });
    connect(remote, &RemoteWidget::viewModePressed, this, [this]() { on_viewPushButton_clicked(); updateRemote(); });
    connect(remote, &RemoteWidget::mutePressed, this, [this]() {
#ifdef LDDECODE_HAVE_AUDIO
        const bool muted = !waveform->muted();
        waveform->setMuted(muted);
        audio->setMuted(muted);
        configuration.setAudioValue("muted", muted);
#endif
        updateRemote();
    });

    // What was set the last time
    remote->setJogMode(configuration.playbackValue("jogMode", true).toBool());
    QTimer::singleShot(0, this, [this]() {
        actionInfoDisplay->setChecked(configuration.playbackValue("infoDisplay", false).toBool());
        if (configuration.playbackValue("remoteShown", false).toBool()) actionRemote->setChecked(true);
    });
#ifdef LDDECODE_HAVE_AUDIO
    // (the sounds that can be chosen come and go: the choice of the remote is kept up to date)
    if (audioStatsTimer) connect(audioStatsTimer, &QTimer::timeout, this, [this]() { if (remote && remote->isVisible()) updateRemote(); });
#endif
    updateRemote();
}

// The bar of the position, at the bottom of the picture in the full screen: it is clicked or dragged to go through the capture, and goes away
// with the remote
void MainWindow::setupSeekPanel()
{
    seekPanel = new QFrame(ui->centralWidget);
    seekPanel->setObjectName("seekPanel");
    seekPanel->setStyleSheet("QFrame#seekPanel { background: rgba(12, 12, 14, 205); border: 1px solid rgba(255, 255, 255, 40); border-radius: 6px; }"
                             "QLabel { color: #e6e6ea; background: transparent; font-family: monospace; font-size: 13px; }");
    QHBoxLayout *row = new QHBoxLayout(seekPanel);
    row->setContentsMargins(14, 6, 14, 6);
    row->setSpacing(12);
    seekPlayButton = new QToolButton(seekPanel);
    seekPlayButton->setFocusPolicy(Qt::NoFocus);
    seekPlayButton->setFixedSize(30, 26);
    seekPlayButton->setToolTip(tr("Play / pause"));
    seekPlayButton->setStyleSheet("QToolButton { color: #e6e6ea; background: transparent; border: none; font-size: 17px; }"
                                  "QToolButton:hover { color: #f2b84a; }");
    seekPlayButton->setText(QString(QChar(0x25B6)));
    connect(seekPlayButton, &QToolButton::clicked, this, [this]() {
        if (playback->isPlaying() || scanSpeed != 0.0 || stillWithSound) {
            endStill();
            stopScan();
            pausePlayback();
            updateRemote();
        } else {
            moveAt(1.0);
        }
        showOverlays();
    });
    // The skips, as on the remote: the start of the chapter that is in (or the one before), and of the next
    const auto skipButton = [this](const QString &iconName, const QString &tip, qint32 direction) {
        QToolButton *button = new QToolButton(seekPanel);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(30, 26);
        button->setToolTip(tip);
        // (the icons of the buttons of the main window: not a symbol of the font, which some systems draw as a coloured emoji)
        button->setIcon(QIcon(iconName));
        button->setIconSize(QSize(22, 22));
        button->setStyleSheet("QToolButton { background: transparent; border: none; border-radius: 4px; }"
                              "QToolButton:hover { background: rgba(255, 255, 255, 40); }");
        connect(button, &QToolButton::clicked, this, [this, direction]() { skipChapter(direction); showOverlays(); });
        return button;
    };
    QToolButton *skipBackButton = skipButton(QStringLiteral(":/icons/Graphics/start-frame.svg"), tr("The start of the chapter, or the one before"), -1);
    QToolButton *skipForwardButton = skipButton(QStringLiteral(":/icons/Graphics/end-frame.svg"), tr("The start of the next chapter"), 1);
    seekTimeLabel = new QLabel(QStringLiteral("0:00:00"), seekPanel);
    seekTotalLabel = new QLabel(QStringLiteral("0:00:00"), seekPanel);
    seekBar = new SeekBar(seekPanel);
    seekMarks = new ChapterMarks(seekBar);
    sliderMarks = new ChapterMarks(ui->posHorizontalSlider);
    row->addWidget(skipBackButton);
    row->addWidget(seekPlayButton);
    row->addWidget(skipForwardButton);
    row->addWidget(seekTimeLabel);
    row->addWidget(seekBar, 1);
    row->addWidget(seekTotalLabel);
    // The button that leaves the full screen, at the right of the bar
    QToolButton *leaveButton = new QToolButton(seekPanel);
    leaveButton->setFocusPolicy(Qt::NoFocus);
    leaveButton->setFixedSize(30, 26);
    leaveButton->setToolTip(tr("Leave the full screen (Esc or F11)"));
    {
        // (four corners that point to the middle, drawn: a symbol of the font could be an emoji)
        QPixmap pixmap(44, 44);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(0xe6, 0xe6, 0xea), 3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter.drawPolyline(QPolygon({ QPoint(5, 17), QPoint(17, 17), QPoint(17, 5) }));
        painter.drawPolyline(QPolygon({ QPoint(27, 5), QPoint(27, 17), QPoint(39, 17) }));
        painter.drawPolyline(QPolygon({ QPoint(5, 27), QPoint(17, 27), QPoint(17, 39) }));
        painter.drawPolyline(QPolygon({ QPoint(27, 39), QPoint(27, 27), QPoint(39, 27) }));
        painter.end();
        leaveButton->setIcon(QIcon(pixmap));
        leaveButton->setIconSize(QSize(20, 20));
    }
    leaveButton->setStyleSheet("QToolButton { background: transparent; border: none; border-radius: 4px; }"
                               "QToolButton:hover { background: rgba(255, 255, 255, 40); }");
    connect(leaveButton, &QToolButton::clicked, this, [this]() { actionFullScreen->setChecked(false); });
    row->addWidget(leaveButton);
    seekPanel->hide();
    ui->centralWidget->installEventFilter(this);

    // A drag asks for many pictures: they are asked for at most every few milliseconds, the last place being the one that is gone to
    scrubTimer = new QTimer(this);
    scrubTimer->setSingleShot(true);
    scrubTimer->setInterval(40);
    connect(scrubTimer, &QTimer::timeout, this, [this]() { seekTo(scrubValue); });
    connect(seekBar, &SeekBar::scrubbed, this, [this](qint32 value) {
        scrubValue = value;
        stopScan();
        if (!scrubTimer->isActive()) scrubTimer->start();
        showOverlays();
    });
    connect(seekBar, &SeekBar::scrubFinished, this, [this]() {
        scrubTimer->stop();
        seekTo(scrubValue);
    });
}

void MainWindow::layoutSeekPanel()
{
    if (!seekPanel) return;
    static constexpr qint32 MARGIN = 24;
    const QSize area = ui->centralWidget->size();
    const qint32 height = seekPanel->sizeHint().height();
    seekPanel->setGeometry(MARGIN, area.height() - height - MARGIN, qMax(120, area.width() - 2 * MARGIN), height);
    seekPanel->raise();
}

// The marks of the chapters on the slider of the window and on the bar of the full screen: where each chapter of the disc begins (in the units of the
// controls: fields in the field view)
void MainWindow::updateChapterMarks()
{
    if (!sliderMarks) return;
    QVector<qint32> values;
    if (tbcSource.getIsSourceLoaded() && !tbcSource.getIsLive()) {
        const bool byFields = tbcSource.getFieldViewEnabled();
        for (const qint32 frame: tbcSource.chapterStartFrames()) values.append(byFields ? 2 * frame - 1 : frame);
    }
    sliderMarks->setMarks(values);
    seekMarks->setMarks(values);
}

// Where the picture is in the capture (its time, and the end of it), unless the person is moving it
void MainWindow::updateSeekPanel()
{
    if (!seekPanel || !seekPanel->isVisible() || seekBar->isScrubbing() || !tbcSource.getIsSourceLoaded()) return;
    const qint32 first = seekFirst();
    const qint32 last = qMax(first, seekLast());
    const qint32 position = seekPosition();
    const double perSecond = seekUnitsPerSecond();
    {
        const QSignalBlocker blocker(seekBar);
        seekBar->setRange(first, last);
        seekBar->setValue(position);
    }
    seekBar->setEnabled(!tbcSource.getIsLive());
    seekTimeLabel->setText(clockText(static_cast<qint64>(qMax(0, position - 1) / perSecond)));
    seekTotalLabel->setText(clockText(static_cast<qint64>(last / perSecond)));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->centralWidget && event->type() == QEvent::Resize) layoutSeekPanel();
    return QMainWindow::eventFilter(watched, event);
}

// What the keys show: the number, the state, the lamps
void MainWindow::updateRemote()
{
    if (!remote) return;
    // (the play button of the bar of the full screen shows what a press does: pause while it plays or scans)
    if (seekPlayButton) seekPlayButton->setText(playback->isPlaying() || scanSpeed != 0.0 ? QString(QChar(0x275A)).repeated(2) : QString(QChar(0x25B6)));
    const bool loaded = tbcSource.getIsSourceLoaded();
    remote->setSourceLoaded(loaded);
    if (!loaded) {
        remote->setDisplay(RemoteKeypad::modeName(keypad.mode()), keypad.text(), QString());
        return;
    }
    const bool scanning = scanSpeed != 0.0;
    QString state;
    if (!searchMessage.isEmpty()) state = searchMessage;
    else if (scanning) state = tr("SCAN %1").arg(RemoteSpeed::text(scanSpeed));
    else if (stillWithSound) state = tr("STILL + SOUND");
    else if (playback->isPlaying()) state = playback->getSpeed() == 1.0 ? tr("PLAY") : tr("PLAY %1").arg(RemoteSpeed::text(playback->getSpeed()));
    else state = tr("PAUSE");
    remote->setDisplay(RemoteKeypad::modeName(keypad.mode()), keypad.text(), state);
    remote->setTransport(scanning ? QStringLiteral("scan") : (playback->isPlaying() ? QStringLiteral("play") : QStringLiteral("pause")));
    remote->setMemoryStored(memoryFrame > 0);
    remote->setRepeatPoints(repeatStartFrame > 0, repeatEndFrame > 0);
    remote->setStillWithSound(stillWithSound);
    remote->setFilter(filtersEnabled, VideoFilter::isAvailable());
    remote->setRaw(!tbcSource.getChromaDecoder());
    {
        QString viewName = tr("FRAM");
        if (tbcSource.getViewMode() == TbcSource::ViewMode::SPLIT_VIEW) viewName = tr("SPLT");
        else if (tbcSource.getViewMode() == TbcSource::ViewMode::FIELD_VIEW) viewName = tr("FLD");
        else if (deinterlacedView) viewName = tr("DEIN");
        remote->setViewMode(viewName);
    }
#ifdef LDDECODE_HAVE_AUDIO
    remote->setMuted(waveform && waveform->muted());
#endif
    remote->setSearchAvailable(tbcSource.hasChapters(), tbcSource.hasPictureNumbers(), tbcSource.hasTimes());
#ifdef LDDECODE_HAVE_AUDIO
    bool digitalThere = !efmSourceName().isEmpty(), analogueThere = false, digital = false;
    if (audio) {
        const QVector<AudioTrackInfo> &tracks = audio->tracks();
        for (const AudioTrackInfo &track: tracks) {
            if (track.source == QLatin1String("efm")) digitalThere = true;
            if (track.source == QLatin1String("analogue")) analogueThere = true;
        }
        const qint32 chosen = audio->currentTrack();
        digital = chosen >= 0 && chosen < tracks.size() && tracks[chosen].source == QLatin1String("efm");
    }
    remote->setAudioChoice(digitalThere, analogueThere, digital);
#else
    remote->setAudioChoice(false, false, false);
#endif
}

// The numbers of the disc for the picture on show (the time code, the chapter, the picture number...), and the lamps of the remote that
// come from the VBI (the side, CX)
void MainWindow::updateInfoPanel()
{
    if (!infoPanel || !tbcSource.getIsSourceLoaded()) return;
    const bool showPanel = infoPanel->isVisible();
    const bool showRemote = remote && remote->isVisible();
    if (!showPanel && !showRemote) return;

    const bool vbiValid = tbcSource.getIsFrameVbiValid();
    const VbiDecoder::Vbi vbi = vbiValid ? tbcSource.getFrameVbi() : VbiDecoder::Vbi();
    if (showRemote) {
        remote->setCx(vbiValid && vbi.cx);
        remote->setSide(vbiValid && vbi.type != VbiDecoder::unknownDiscType ? (vbi.side ? 0 : 1) : -1);
    }
    if (!showPanel) return;

    QString headline;
    if (vbi.clvHr != -1) headline = clockText(vbi.clvHr * 3600 + vbi.clvMin * 60 + vbi.clvSec);
    else if (vbi.picNo != -1) headline = QString("%1").arg(vbi.picNo, 5, 10, QChar('0'));
    else headline = QString::number(currentFrameNumber);

    InfoPanel::Rows rows;
    if (vbiValid && vbi.type != VbiDecoder::unknownDiscType) rows.append({ tr("Disc"), vbi.type == VbiDecoder::cav ? tr("CAV") : tr("CLV") });
    if (vbi.chNo != -1) rows.append({ tr("Chapter"), QString::number(vbi.chNo) });
    if (vbi.picNo != -1) rows.append({ tr("Picture"), QString::number(vbi.picNo) });
    if (tbcSource.getIsFrameVitcValid()) {
        const VitcDecoder::Vitc vitc = tbcSource.getFrameVitc();
        rows.append({ tr("VITC"), QString("%1:%2:%3%4%5").arg(vitc.hour, 2, 10, QChar('0')).arg(vitc.minute, 2, 10, QChar('0'))
                                      .arg(vitc.second, 2, 10, QChar('0')).arg(vitc.isDropFrame ? QChar(';') : QChar(':')).arg(vitc.frame, 2, 10, QChar('0')) });
    }
    if (tbcSource.getFieldViewEnabled()) rows.append({ tr("Field"), tr("%1 / %2").arg(currentFieldNumber).arg(tbcSource.getNumberOfFields()) });
    else rows.append({ tr("Frame"), tr("%1 / %2").arg(currentFrameNumber).arg(tbcSource.getNumberOfFrames()) });
    if (vbiValid && vbi.cx) rows.append({ tr("CX"), tr("on the disc") });
    if (scanSpeed != 0.0) rows.append({ tr("Scan"), RemoteSpeed::text(scanSpeed) });
    else if (playback->isPlaying()) rows.append({ tr("Speed"), RemoteSpeed::text(playback->getSpeed()) });
#ifdef LDDECODE_HAVE_AUDIO
    if (audio && audio->currentTrack() >= 0 && audio->currentTrack() < audio->tracks().size()) {
        const AudioTrackInfo &track = audio->tracks()[audio->currentTrack()];
        rows.append({ tr("Sound"), track.source == QLatin1String("efm") ? tr("digital (EFM)") : (track.source == QLatin1String("analogue") ? tr("analogue") : tr("imported")) });
    }
#endif
    infoPanel->setRows(headline, rows);
}

// A frame (from 1), shown: the position of the controls is in fields in the field view
void MainWindow::seekToFrame(qint32 frame)
{
    seekTo(tbcSource.getFieldViewEnabled() ? 2 * frame - 1 : frame);
}

// A search by the number that was typed: a chapter, a picture, a time or a frame
void MainWindow::remoteSearch(bool asFrame)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive() || keypad.isEmpty()) return;
    // (the frame search takes the digits as the number of a frame, whatever the mode of the other search is)
    const SearchMode mode = asFrame ? SearchMode::Frame : keypad.mode();
    const qint64 number = asFrame ? keypad.number() : keypad.value();
    qint32 frame = -1;
    switch (mode) {
    case SearchMode::Chapter: frame = tbcSource.frameOfChapter(static_cast<qint32>(number)); break;
    case SearchMode::Picture: frame = tbcSource.frameOfPicture(static_cast<qint32>(number)); break;
    case SearchMode::Time: frame = tbcSource.frameOfTime(static_cast<qint32>(number)); break;
    case SearchMode::Frame: frame = number >= 1 && number <= tbcSource.getNumberOfFrames() ? static_cast<qint32>(number) : -1; break;
    }
    if (frame < 1) {
        searchMessage = tr("NOT FOUND");
        searchMessageTimer->start();
        statusBar()->showMessage(tr("%1 %2 is not in this capture").arg(RemoteKeypad::modeName(mode).toLower(), asFrame ? QString::number(number) : keypad.text()), 4000);
        updateRemote();
        return;
    }
    const bool resume = playback->isPlaying();
    stopScan();
    seekToFrame(frame);
    if (resume) togglePlayback();
    keypad.clear();
    updateRemote();
}

// PGM: a small window with the chapters of the disc, each with its time, and a button to go to one
void MainWindow::showChapterList()
{
    if (!chapterListDialog) {
        chapterListDialog = new ChapterListDialog(this);
        connect(chapterListDialog, &ChapterListDialog::goToFrame, this, [this](qint32 frame) {
            if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
            const bool resume = playback->isPlaying();
            stopScan();
            seekToFrame(frame);
            if (resume) togglePlayback();
        });
    }
    refreshChapterList();
    chapterListDialog->show();
    chapterListDialog->raise();
}

// The list is made again from the chapters of the capture (and the one the picture is in is chosen)
void MainWindow::refreshChapterList()
{
    if (!chapterListDialog) return;
    QVector<ChapterListDialog::Row> rows;
    if (tbcSource.getIsSourceLoaded() && !tbcSource.getIsLive()) {
        const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
        for (const TbcSource::ChapterEntry &entry: tbcSource.chapterList()) {
            ChapterListDialog::Row row;
            row.chapter = entry.chapter;
            row.frame = entry.frame;
            row.picture = entry.picture;
            row.discSeconds = entry.seconds;
            row.captureSeconds = static_cast<qint32>((entry.frame - 1) / frameRate);
            rows.append(row);
        }
    }
    chapterListDialog->setChapters(rows, currentFrameNumber);
}

// The skip keys: the start of the chapter that comes (or of the one that is in, or before it); with no chapters, the ends
void MainWindow::skipChapter(qint32 direction)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    qint32 target = direction > 0 ? tbcSource.chapterStartAfter(currentFrameNumber) : tbcSource.chapterStartBefore(currentFrameNumber);
    if (target < 0) target = direction > 0 ? tbcSource.getNumberOfFrames() : 1;
    const bool resume = playback->isPlaying();
    stopScan();
    seekToFrame(target);
    if (resume) togglePlayback();
}

// The speed of the picture (negative: backward). Up to four times forward is the playback, which has the colour and, at the normal speed,
// the sound; the others are a scan.
void MainWindow::moveAt(double speed)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    endStill();
    if (speed > 0.0 && speed <= 4.0) {
        stopScan();
        applyPlaybackSpeed(speed);
        speedFromRemote = speed != 1.0;
        if (!playback->isPlaying()) togglePlayback();
    } else {
        startScan(speed);
    }
    updateRemote();
}

// The speed of the playback, which is in the Playback menu too
void MainWindow::applyPlaybackSpeed(double speed)
{
    for (QAction *action: speedActions->actions()) {
        if (qFuzzyCompare(action->data().toDouble(), speed)) action->setChecked(true);
    }
    playback->setSpeed(speed);
#ifdef LDDECODE_HAVE_AUDIO
    // The sound plays at the speed of the video only: at another speed it is off
    if (playback->isPlaying()) {
        audioStop();
        if (speed == 1.0) audioStart(playbackDue);
    }
#endif
}

// The picture stays and the sound goes on: the end of that (any other key of the transport does it)
void MainWindow::endStill()
{
    if (!stillWithSound) return;
    stillWithSound = false;
#ifdef LDDECODE_HAVE_AUDIO
    audioStop();
#endif
    if (remote) remote->setStillWithSound(false);
}

void MainWindow::toggleStillWithSound()
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    if (stillWithSound) {
        endStill();
        updateRemote();
        return;
    }
#ifdef LDDECODE_HAVE_AUDIO
    if (!audio || !audio->hasAudio() || audio->currentTrack() < 0) {
        statusBar()->showMessage(tr("There is no sound to go on with"), 3000);
        return;
    }
    stopScan();
    pausePlayback();
    // (the sound alone: the clock of the video is left as it is)
    const double at = positionSeconds(tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber);
    if (!audio->start(at)) return;
    stillWithSound = true;
    updateRemote();
#else
    statusBar()->showMessage(tr("This build has no sound"), 3000);
#endif
}

// One-shot memory: the first press keeps the place, the next one goes back to it
void MainWindow::toggleMemory()
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    if (memoryFrame < 1) {
        memoryFrame = currentFrameNumber;
        statusBar()->showMessage(tr("Memory: frame %1 kept").arg(memoryFrame), 3000);
    } else {
        const qint32 frame = memoryFrame;
        memoryFrame = -1;
        const bool resume = playback->isPlaying();
        stopScan();
        seekToFrame(frame);
        if (resume) togglePlayback();
    }
    updateRemote();
}

// A and B: the first A press marks where a repeated part begins, B where it ends (it then plays again and again); a press of a key that
// is set clears the part
void MainWindow::markRepeat(bool endPoint)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsLive()) return;
    if (!endPoint) {
        if (repeatStartFrame > 0) {
            clearRepeat();
            return;
        }
        repeatStartFrame = currentFrameNumber;
        repeatEndFrame = -1;
        statusBar()->showMessage(tr("Repeat: start at frame %1, now mark the end").arg(repeatStartFrame), 3000);
    } else {
        if (repeatStartFrame < 1) {
            statusBar()->showMessage(tr("Mark the start of the part with A first"), 3000);
            return;
        }
        if (repeatEndFrame > 0) {
            clearRepeat();
            return;
        }
        if (currentFrameNumber <= repeatStartFrame) {
            statusBar()->showMessage(tr("The end must be after the start"), 3000);
            return;
        }
        repeatEndFrame = currentFrameNumber;
        stopScan();
        pausePlayback();
        seekToFrame(repeatStartFrame);
        togglePlayback();
        statusBar()->showMessage(tr("Repeating frames %1 to %2").arg(repeatStartFrame).arg(repeatEndFrame), 3000);
    }
    updateRemote();
}

void MainWindow::clearRepeat()
{
    const bool wasRepeating = repeatEndFrame > 0;
    repeatStartFrame = repeatEndFrame = -1;
    statusBar()->showMessage(tr("Repeat cleared"), 2000);
    // (the playback goes on from here over the whole capture)
    if (wasRepeating && playback->isPlaying()) {
        pausePlayback();
        togglePlayback();
    }
    updateRemote();
}

// DIGITAL / ANALOG: the sound decoded from the EFM, or the analogue one. The EFM is decoded if that was not done yet.
void MainWindow::toggleDigitalAnalog()
{
#ifdef LDDECODE_HAVE_AUDIO
    if (!audio || !tbcSource.getIsSourceLoaded()) return;
    const QVector<AudioTrackInfo> &tracks = audio->tracks();
    const qint32 chosen = audio->currentTrack();
    const bool digitalNow = chosen >= 0 && chosen < tracks.size() && tracks[chosen].source == QLatin1String("efm");
    const QString wanted = digitalNow ? QStringLiteral("analogue") : QStringLiteral("efm");
    for (qint32 index = 0; index < tracks.size(); index++) {
        if (tracks[index].source == wanted) {
            chooseAudioTrack(index);
            updateRemote();
            return;
        }
    }
    if (wanted == QLatin1String("efm") && !efmSourceName().isEmpty()) {
        if (audioConfig) startEfmDecoding(audioConfig->currentDecoder(), audioConfig->efmWithoutTimecodes(), audioConfig->efmConceal(), audioConfig->efmPad());
        else startEfmDecoding(configuration.audioValue("decoder", 1).toInt(), true, true, false);
        statusBar()->showMessage(tr("Decoding the EFM: the digital sound is heard as it comes"), 4000);
    } else {
        statusBar()->showMessage(wanted == QLatin1String("efm") ? tr("This capture has no EFM") : tr("This capture has no analogue sound"), 3000);
    }
#else
    statusBar()->showMessage(tr("This build has no sound"), 3000);
#endif
}

#ifdef LDDECODE_HAVE_AUDIO
// A sound chosen from here (as the list of the waveform panel does), the panel being told
void MainWindow::chooseAudioTrack(qint32 index)
{
    const double at = audio->isPlaying() ? audio->videoSecondsNow() : -1.0;
    waveform->setMessage(QString());
    audio->selectTrack(index);
    waveform->setTracks(audio->tracks(), index);
    waveform->refresh();
    audioFollowVideo(at);
}
#else
void MainWindow::chooseAudioTrack(qint32) {}
#endif

// The view of the picture as the view button sets it (the full screen sets it too): the playback goes on in the new one
void MainWindow::setViewState(TbcSource::ViewMode mode, bool deinterlaced)
{
    const bool wasPlaying = playback->isPlaying();
    pausePlayback();
    tbcSource.setViewMode(mode);
    if (mode == TbcSource::ViewMode::FIELD_VIEW) tbcSource.setStretchField(true);
    deinterlacedView = deinterlaced && mode == TbcSource::ViewMode::FRAME_VIEW && VideoFilter::isAvailable();
    const QString problem = updateVideoFilterGraph();
    if (deinterlacedView && !problem.isEmpty()) {
        deinterlacedView = false;
        updateVideoFilterGraph();
        statusBar()->showMessage(tr("The deinterlacer could not be used: %1").arg(problem), 10000);
    }
    setViewValues();
    updateGuiLoaded();
    showImage();
    if (wasPlaying) togglePlayback();
}

// The full screen: the picture (its active area) takes all of the screen, and the bars and the menus go. The remote and the numbers of the
// disc are windows that float over it, where they were put; they go away when the mouse does not move, and come back with it.
void MainWindow::setFullScreen(bool on)
{
    if (on == fullScreenOn) return;
    if (on) {
        fullScreenWasMaximized = isMaximized();
        // (the shortcuts of the menus go on working with the menu bar hidden: the actions are the window's too)
        for (const ShortcutCommand &command: shortcutCommands()) addAction(command.action);
        hiddenInFullScreen.clear();
        QVector<QWidget *> bars = { ui->menuBar, ui->statusBar, ui->mediaControl_frame };
#ifdef LDDECODE_HAVE_AUDIO
        bars.append(waveform);
#endif
        for (QWidget *bar: bars) {
            if (bar && bar->isVisible()) {
                bar->hide();
                hiddenInFullScreen.append(bar);
            }
        }
        // The picture is made to fit the screen, as the window did
        scaleBeforeFull = scaleFactor;
        fitWasOn = resizeFrameWithWindow;
        if (!fitWasOn) ui->actionResizeFrameWithWindow->setChecked(true);
        // (black round the picture, as a screen is)
        centralStyleBefore = ui->centralWidget->styleSheet();
        ui->centralWidget->setStyleSheet("QWidget { background-color: black; }");
        fullScreenOn = true;
        // The remote and the numbers are there when it begins (what they were is given back when it ends)
        // The colour and the deinterlaced view, as a screen is for (given back when it ends)
        chromaBeforeFull = tbcSource.getChromaDecoder();
        viewBeforeFull = tbcSource.getViewMode();
        deinterlacedBeforeFull = deinterlacedView;
        if (tbcSource.getIsSourceLoaded()) {
            if (!chromaBeforeFull) {
                tbcSource.setChromaDecoder(true);
                ui->videoPushButton->setText(tr("Chroma"));
            }
            if (viewBeforeFull != TbcSource::ViewMode::FRAME_VIEW || !deinterlacedView) setViewState(TbcSource::ViewMode::FRAME_VIEW, true);
            else showImage();
        }
        remoteWasOn = actionRemote->isChecked();
        infoWasOn = actionInfoDisplay->isChecked();
        actionRemote->setChecked(true);
        actionInfoDisplay->setChecked(true);
        showFullScreen();
        lastCursor = QCursor::pos();
        cursorPoll->start();
        showOverlays();
    } else {
        fullScreenOn = false;
        cursorPoll->stop();
        overlayTimer->stop();
        overlaysHiddenNow = false;
        unsetCursor();
        for (QWidget *bar: hiddenInFullScreen) bar->show();
        hiddenInFullScreen.clear();
        if (fullScreenWasMaximized) showMaximized(); else showNormal();
        if (!fitWasOn) ui->actionResizeFrameWithWindow->setChecked(false);
        ui->centralWidget->setStyleSheet(centralStyleBefore);
        seekPanel->hide();
        // (the picture was fitted to the screen: it goes back to the size it had, and is fitted again if the window fits it)
        scaleFactor = scaleBeforeFull;
        if (tbcSource.getIsSourceLoaded()) {
            if (!chromaBeforeFull && tbcSource.getChromaDecoder()) {
                tbcSource.setChromaDecoder(false);
                ui->videoPushButton->setText(tr("Source"));
            }
            if (tbcSource.getViewMode() != viewBeforeFull || deinterlacedView != deinterlacedBeforeFull) setViewState(viewBeforeFull, deinterlacedBeforeFull);
            else showImage();
        }
        actionRemote->setChecked(remoteWasOn);
        actionInfoDisplay->setChecked(infoWasOn);
        remoteWindow->setVisible(actionRemote->isChecked());
        infoPanel->setVisible(actionInfoDisplay->isChecked());
        updateRemote();
    }
    // (the picture is the active area only in the full screen: it is drawn again, and fitted when the window has its size)
    if (tbcSource.getIsSourceLoaded()) {
        updateImageViewer();
        resizeTimer->start();
    }
}

bool MainWindow::overlaysVisible() const
{
    return fullScreenOn && !overlaysHiddenNow;
}

// The remote and the numbers come back (and the pointer), and go after a few seconds of the mouse being still
void MainWindow::showOverlays()
{
    if (!fullScreenOn) return;
    overlaysHiddenNow = false;
    unsetCursor();
    remoteWindow->setVisible(actionRemote->isChecked());
    infoPanel->setVisible(actionInfoDisplay->isChecked());
    seekPanel->setVisible(tbcSource.getIsSourceLoaded());
    layoutSeekPanel();
    updateRemote();
    updateInfoPanel();
    updateSeekPanel();
    overlayTimer->start();
}

void MainWindow::hideOverlays()
{
    if (!fullScreenOn) return;
    // Each one goes by itself: the bar unless it is being dragged, the remote unless it is pinned or the mouse is on it or a number is being
    // typed, the numbers if their pin is off (and the mouse is not on them). What is kept is looked at again later.
    bool kept = false;
    // (a drag whose mouse release never came is over: the bar is not held when no button is down)
    if (seekBar->isScrubbing() && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) seekBar->cancelScrub();
    // The bar goes after the time without a move of the mouse, as the controls of a video player do, even if the pointer is on it (a move brings it
    // back); only a drag holds it
    if (seekBar->isScrubbing()) kept = true;
    else seekPanel->hide();
    if (!remoteTitleBar->pinned()) {
        if (remoteWindow->underMouse() || !keypad.isEmpty()) kept = true;
        else remoteWindow->hide();
    }
    // (the display of the numbers is the screen of the player: it stays, unless the pin says it goes too)
    if (infoPanel->autoHide()) {
        if (infoPanel->underMouse()) kept = true;
        else infoPanel->hide();
    }
    if (kept) overlayTimer->start();
    // The pointer goes with the bar (the windows have a pointer of their own)
    if (!seekPanel->isVisible()) {
        overlaysHiddenNow = true;
        setCursor(Qt::BlankCursor);
    }
}

// A move of the mouse over the picture brings the overlays back. A move on another screen does not, nor one over the remote or the numbers
// (those are not the picture: the mouse is there to use them)
void MainWindow::pollCursor()
{
    const QPoint now = QCursor::pos();
    if (now == lastCursor) return;
    lastCursor = now;
    const auto over = [now](const QWidget *widget) { return widget->isVisible() && widget->frameGeometry().contains(now); };
    if (over(remoteWindow) || over(infoPanel)) return;
    if (frameGeometry().contains(now)) showOverlays();
}

// In the full screen the digits and the other keys of the remote are the keyboard's, and Escape goes back to the window
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (fullScreenOn) {
        if (event->key() == Qt::Key_Escape) {
            actionFullScreen->setChecked(false);
            return;
        }
        const qint32 code = event->key();
        if ((code >= Qt::Key_0 && code <= Qt::Key_9) || code == Qt::Key_Return || code == Qt::Key_Enter || code == Qt::Key_Backspace
            || code == Qt::Key_Delete || code == Qt::Key_Plus) {
            QCoreApplication::sendEvent(remote, event);
            showOverlays();
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

// The keys of the program ------------------------------------------------------------------------------------------------------------

// The commands that have a place in the menus, with where, and an id that stays the same (the name of the action, or its place and text)
QVector<ShortcutCommand> MainWindow::shortcutCommands()
{
    QVector<ShortcutCommand> commands;
    QSet<QAction *> seen;
    const auto plain = [](QString text) {
        text.remove(QChar('&'));
        return text;
    };
    const std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &path) {
        for (QAction *action: menu->actions()) {
            if (action->isSeparator()) continue;
            if (QMenu *sub = action->menu()) {
                walk(sub, path.isEmpty() ? plain(sub->title()) : path + " > " + plain(sub->title()));
                continue;
            }
            if (seen.contains(action)) continue;
            seen.insert(action);
            ShortcutCommand command;
            command.text = plain(action->text()).remove("...");
            command.menu = path;
            command.action = action;
            command.id = action->objectName().isEmpty() ? (path + "/" + command.text) : action->objectName();
            command.defaults = shortcutDefaults.value(action);
            commands.append(command);
        }
    };
    for (QAction *top: ui->menuBar->actions()) {
        if (QMenu *menu = top->menu()) walk(menu, plain(menu->title()));
    }
    // The keys of the remote control: no menu has them
    for (QAction *action: remoteActions) {
        if (seen.contains(action)) continue;
        seen.insert(action);
        ShortcutCommand command;
        command.text = plain(action->text());
        command.menu = tr("Remote control");
        command.action = action;
        command.id = action->objectName();
        command.defaults = shortcutDefaults.value(action);
        commands.append(command);
    }
    return commands;
}

// The keys the commands have as the program begins (before the ones that were changed are put on)
void MainWindow::captureShortcutDefaults()
{
    for (const ShortcutCommand &command: shortcutCommands()) shortcutDefaults.insert(command.action, command.action->shortcuts());
}

void MainWindow::applyStoredShortcuts()
{
    for (const ShortcutCommand &command: shortcutCommands()) {
        bool found = false;
        const QString text = configuration.shortcutValue(command.id, found);
        if (!found) continue;
        QList<QKeySequence> keys;
        for (const QString &part: text.split(QChar(';'), Qt::SkipEmptyParts)) keys << QKeySequence(part, QKeySequence::PortableText);
        command.action->setShortcuts(keys);
    }
}

void MainWindow::showShortcuts()
{
    ShortcutsDialog dialog(shortcutCommands(), this);
    connect(&dialog, &ShortcutsDialog::changed, this, [this](const QString &id, const QList<QKeySequence> &keys) {
        for (const ShortcutCommand &command: shortcutCommands()) {
            if (command.id != id) continue;
            command.action->setShortcuts(keys);
            if (keys == command.defaults) {
                configuration.clearShortcutValue(id);
            } else {
                QStringList parts;
                for (const QKeySequence &key: keys) parts << key.toString(QKeySequence::PortableText);
                configuration.setShortcutValue(id, parts.join(QChar(';')));
            }
        }
    });
    if (dialog.exec() == QDialog::Accepted) configuration.writeConfiguration();
}
