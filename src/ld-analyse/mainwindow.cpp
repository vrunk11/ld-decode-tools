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

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "tbc/logging.h"
#include "cvbs.h"

#include <QActionGroup>

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
    scaleFactor = configuration.getMainWindowScaleFactor();

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
    for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
        connect(bar, &SnrSourceBar::measuringToggled, this, [this](bool measuring) {
            tbcSource.setLiveSnrEnabled(measuring);
            for (SnrSourceBar *other: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
                other->setLiveSource(measuring, tbcSource.getLiveSnrIsWholeHistory());
            }
        });
    }
    setupVideoFilter();

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
    // The decoding threads read the same files (or stream) as the source
    renderer->close();

    // Save the window geometry and settings to the configuration
    configuration.setMainWindowGeometry(saveGeometry());
    configuration.setMainWindowScaleFactor(scaleFactor);
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
    configuration.writeConfiguration();

    // Close the source video if open
    if (tbcSource.getIsSourceLoaded()) {
        tbcSource.unloadSource();
    }

    delete ui;
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
    ui->menuWindow->addAction(action);
    ui->menuWindow->addAction(actionFilters);

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


// Show the image through the video filter, if there is one. Returns false if the picture
// on show should be left as it is (a filter that is still filling up gave nothing yet).
bool MainWindow::filterImage(QImage &image, qint32 displayWidth, qint32 displayHeight)
{
    if (!videoFilter.isActive() || image.isNull()) return true;

    const bool isFieldView = tbcSource.getFieldViewEnabled();
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

    QImage filtered = videoFilter.filter(image, format, continuous);
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
    const qint32 first = isFieldView ? 2 * firstFrame - 1 : firstFrame;
    const qint32 last = isFieldView ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames();
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
        // Playing: the whole of what has arrived, as a timeline (a range that moved with the
        // buffer would make the handle jump about). Paused: the buffer, which is what can be reached.
        const qint32 low = navigable ? first : 1;
        ui->posNumberSpinBox->setRange(low, qMax(low, last));
        ui->posHorizontalSlider->setRange(low, qMax(low, last));
        ui->posHorizontalSlider->setPageStep(qMax(1, (last - low) / 100));
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
    ui->menuBar->insertMenu(ui->menuWindow->menuAction(), menu);

    actionPlayPause = menu->addAction(tr("&Play"));
    actionPlayPause->setShortcut(Qt::Key_Space);
    actionPlayPause->setShortcutContext(Qt::ApplicationShortcut);
    connect(actionPlayPause, &QAction::triggered, this, &MainWindow::togglePlayback);

    actionLoop = menu->addAction(tr("&Loop"));
    actionLoop->setCheckable(true);
    connect(actionLoop, &QAction::toggled, playback, &PlaybackController::setLoop);

    // Speed relative to the real speed of the video
    speedSeparator = menu->addSeparator();
    speedActions = new QActionGroup(this);
    for (const double speed: {0.25, 0.5, 1.0, 2.0, 4.0}) {
        QAction *action = menu->addAction(tr("%1x speed").arg(speed));
        action->setCheckable(true);
        action->setChecked(speed == 1.0);
        if (speed == 1.0) speedNormalAction = action;
        speedActions->addAction(action);
        connect(action, &QAction::triggered, this, [this, speed]() { playback->setSpeed(speed); });
    }

    updatePlaybackState(false);
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
    updatePlayButtonIcon();
    actionPlayPause->setText(playing ? tr("&Pause") : tr("&Play"));
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
    const double frameRate = tbcSource.getSystem() == PAL ? 25.0 : 30000.0 / 1001.0;
    playback->setRate(isFieldView ? 2.0 * frameRate : frameRate);
    const bool isLive = tbcSource.getIsLive();
    if (isLive) tbcSource.unfreezeLive();
    if (isLive) updateLive();
    const qint32 lastPosition = isFieldView ? tbcSource.getNumberOfFields() : tbcSource.getNumberOfFrames();
    const qint32 firstPosition = isLive ? (isFieldView ? 2 * tbcSource.getFirstAvailableFrame() - 1 : tbcSource.getFirstAvailableFrame()) : 1;
    playback->setLive(isLive, isLive && tbcSource.getIsLiveEnded());
    playback->setRange(firstPosition, lastPosition);

    // Start decoding ahead; the clock starts once the first image is ready
    playbackStartPosition = isFieldView ? currentFieldNumber : currentFrameNumber;
    // (a live stream plays on at its end: there is more to come)
    if (playbackStartPosition >= lastPosition && !isLive) playbackStartPosition = 1;
    playbackStartPosition = qMax(playbackStartPosition, firstPosition);
    playbackDue = playbackStartPosition;
    renderGeneration = tbcSource.getSettingsGeneration();
    renderer->start(tbcSource.getRenderSettings(), renderGeneration, playbackStartPosition, lastPosition,
                    isLive ? &tbcSource : nullptr);
    // The clock starts at once: the picture on show stays until the first decoded one is ready,
    // rather than the button waiting for it
    firstImageShown = false;
    playback->play(playbackStartPosition);
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

    tbcSource.setPrerenderedImage(position, image);
    const bool isFieldView = tbcSource.getFieldViewEnabled();
    if (isFieldView) setCurrentField(position); else setCurrentFrame(position);

    // Move the controls along without making them seek
    const QSignalBlocker spinBlocker(ui->posNumberSpinBox);
    const QSignalBlocker sliderBlocker(ui->posHorizontalSlider);
    ui->posNumberSpinBox->setValue(position);
    ui->posHorizontalSlider->setValue(position);
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
    // Enable the GUI controls
    setGuiEnabled(true);

    // Where the SNR in the graphs comes from
    for (SnrSourceBar *bar: { blackSnrAnalysisDialog->sourceBar(), whiteSnrAnalysisDialog->sourceBar() }) {
        if (tbcSource.getIsLive()) bar->setLiveSource(tbcSource.getLiveSnrEnabled(), tbcSource.getLiveSnrIsWholeHistory());
        else bar->setMetadataSource();
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
    statusText += tr(" source loaded with ");

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
    QImage image = tbcSource.getImage();

    if (ui->mouseModePushButton->isChecked() && !image.isNull() && image.width() > 0 && image.height() > 0) {
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

    // Get the aspect ratio adjustment if required
    qint32 adjustment = getAspectAdjustment();
    const int width = static_cast<int>(scaleFactor * (image.width() + adjustment));
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
}

// Set the current field, frame is updated based on field number
void MainWindow::setCurrentField(qint32 number)
{
    if (number == currentFieldNumber) return;

    currentFieldNumber = number;
    currentFrameNumber = std::ceil((double)number / 2);

    sanitizeCurrentPosition();
    showImage();
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
    pausePlayback();
    tbcDebugStream() << "MainWindow::on_actionOpen_TBC_file_triggered(): Called";

    QString inputFileName = QFileDialog::getOpenFileName(this,
                tr("Open TBC or CVBS file"),
                configuration.getSourceDirectory()+tr("/ldsample.tbc"),
                tr("TBC and CVBS files (*.tbc *.tbcy *.tbcc *.cvbs *.cvbsy *.cvbsc);;TBC output (*.tbc *.tbcy *.tbcc);;CVBS (*.cvbs *.cvbsy *.cvbsc);;All Files (*)"));

    // Was a filename specified?
    if (!inputFileName.isEmpty() && !inputFileName.isNull()) {
        lastFilename = inputFileName;
        loadTbcFile(inputFileName);
    }
}

// Reload the current TBC selection from the GUI
void MainWindow::on_actionReload_TBC_triggered()
{
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
	MainWindow::resize_on_aspect();
}

// Zoom out menu option
void MainWindow::on_actionZoom_Out_triggered()
{
    on_zoomOutPushButton_clicked();
	MainWindow::resize_on_aspect();
}

// Original size 1:1 zoom menu option
void MainWindow::on_actionZoom_1x_triggered()
{
    on_originalSizePushButton_clicked();
	MainWindow::resize_on_aspect();
}

// 2:1 zoom menu option
void MainWindow::on_actionZoom_2x_triggered()
{
    scaleFactor = 2.0;
    updateImageViewer();
	MainWindow::resize_on_aspect();
}

// 3:1 zoom menu option
void MainWindow::on_actionZoom_3x_triggered()
{
    scaleFactor = 3.0;
    updateImageViewer();
	MainWindow::resize_on_aspect();
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
	QImage originalImage = tbcSource.getImage();
	if (originalImage.isNull()) {
		return;
	}

	// Calculate scale factor to fit image within available space while maintaining aspect ratio
	qint32 adjustment = getAspectAdjustment();
	double scaleX = static_cast<double>(availableSize.width()) / static_cast<double>(originalImage.width() + adjustment);
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
    resize_on_aspect();
}

// Zoom out
void MainWindow::on_zoomOutPushButton_clicked()
{
    constexpr double factor = 0.9;
    if (((scaleFactor * factor) > 0.333) && ((scaleFactor * factor) < 3.0)) {
        scaleFactor *= factor;
    }

    updateImageViewer();
    resize_on_aspect();
}

// Original size 1:1 zoom
void MainWindow::on_originalSizePushButton_clicked()
{
    scaleFactor = 1.0;
    updateImageViewer();
    resize_on_aspect();
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
        // Generate the graph data
        updateGraphs();

        // Update the GUI
        resetGui();
        updateGuiLoaded();

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
