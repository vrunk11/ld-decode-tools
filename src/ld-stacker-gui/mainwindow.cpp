/******************************************************************************
 * mainwindow.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mainwindow.h"
#include "aspectratio.h"
#include "discnumbering.h"

#include "stackercore.h"
#include "ldxcapture.h"
#include "tbcfiles.h"

#include "processdialog.h"

#include <QElapsedTimer>

#include <QAction>
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QGridLayout>
#include <QFileDialog>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <limits>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace {

const QColor SOURCE_COLORS[] = {
    QColor("#378ADD"), QColor("#1D9E75"), QColor("#D85A30"), QColor("#7F77DD"),
    QColor("#BA7517"), QColor("#D4537E"), QColor("#639922"), QColor("#E24B4A"),
};
constexpr qint32 SOURCE_COLOR_COUNT = sizeof(SOURCE_COLORS) / sizeof(SOURCE_COLORS[0]);
const QColor STACK_COLOR("#888780");

QIcon dotIcon(const QColor &color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(1, 1, 10, 10));
    return QIcon(pixmap);
}

const char *const MODE_NAMES[] = { "Auto", "Mean", "Median", "Smart mean", "Smart neighbour", "Neighbour" };

} // namespace

MainWindow::MainWindow(const QStringList &arguments, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("ld-stacker-gui"));
    // The stacking is off to begin with: turning it on asks to map the sources that follow the map and have no mapping
    project.showStack = false;

    // Colour theme and widget style, as in ld-analyse: View > Theme and View > Style, kept in the settings.
    // --force-dark-theme overrides the theme for this session without changing the setting.
    {
        QSettings settings;
        appearance = new AppearanceMenus(settings.value("themeMode", static_cast<qint32>(ThemeMode::Auto)).toInt(),
                                         settings.value("customPalette").toString(), settings.value("styleName").toString(), this);
    }

    backend = new FrameBackend;
    backend->moveToThread(&backendThread);
    connect(&backendThread, &QThread::finished, backend, &QObject::deleteLater);
    connect(backend, &FrameBackend::sourceLoaded, this, &MainWindow::on_sourceLoaded);
    connect(backend, &FrameBackend::frameReady, this, &MainWindow::on_frameReady);
    connect(backend, &FrameBackend::stackSnrReady, this, &MainWindow::on_stackSnrReady);
    connect(backend, &FrameBackend::sourceSnrReady, this, &MainWindow::on_sourceSnrReady);
    backendThread.start();

    buildUi();
    buildMenus();

    stackWindow = new StackOutputWindow(this);
    stackWindow->setWindowFlag(Qt::Window, true);
    connect(stackWindow, &StackOutputWindow::closed, this, [this]() { stackOutputAction->setChecked(false); });

    optionsDialog = new StackingOptionsDialog(this);
    enhanceDialog = new EnhanceDialog(this);
    chromaDialog = new ChromaConfigDialog(this);
    for (QDialog *dialog: { static_cast<QDialog *>(optionsDialog), static_cast<QDialog *>(enhanceDialog), static_cast<QDialog *>(chromaDialog) }) {
        dialog->setWindowFlag(Qt::Window, true);
    }
    connect(optionsDialog, &StackingOptionsDialog::changed, this, [this]() {
        const bool reversed = project.stacking.reverseFields;
        const qint32 mode = project.stacking.mode;
        project.stacking = optionsDialog->settings();
        project.stacking.mode = mode;
        if (project.stacking.reverseFields != reversed) {
            QMetaObject::invokeMethod(backend, "setReverseFields", Qt::QueuedConnection, Q_ARG(bool, project.stacking.reverseFields));
        }
        sourcesChanged();
    });
    connect(enhanceDialog, &EnhanceDialog::changed, this, [this]() {
        applyZoom();
        if (stackWindow->isVisible() && lastStackResult.view == "stack") showStackPicture(lastStackResult);
    });
    connect(chromaDialog, &ChromaConfigDialog::changed, this, [this]() {
        QMetaObject::invokeMethod(backend, "setChromaConfiguration", Qt::QueuedConnection,
                                  Q_ARG(PalColour::Configuration, chromaDialog->palConfiguration()),
                                  Q_ARG(Comb::Configuration, chromaDialog->ntscConfiguration()));
        if (project.chroma) requestFrame();
    });
    syncDialogs();
    setAcceptDrops(true);

    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    if (!settings.contains("geometry")) resize(1200, 800);

    zoom = 1.0;
    setZoom(zoom);
    updateViewCombo();

    // A session file, or the sources to start with
    QStringList files;
    QString session;
    for (const QString &argument: arguments) {
        if (StackProject::isSession(argument)) session = argument; else files.append(argument);
    }
    if (!session.isEmpty()) loadSession(session);
    if (!files.isEmpty()) addSources(files);
    updateStatus();
}

MainWindow::~MainWindow()
{
    backendThread.quit();
    backendThread.wait();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------------------------
// Interface

void MainWindow::buildUi()
{
    // The side panel: the sources
    QWidget *side = new QWidget;
    QVBoxLayout *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 8, 8, 8);
    QLabel *sourcesTitle = new QLabel(tr("Sources"));
    sourcesTitle->setForegroundRole(QPalette::PlaceholderText);
    sideLayout->addWidget(sourcesTitle);

    sourceList = new QListWidget;
    sourceList->setDragDropMode(QAbstractItemView::InternalMove);
    sourceList->setDefaultDropAction(Qt::MoveAction);
    sourceList->setSelectionMode(QAbstractItemView::SingleSelection);
    sourceList->setContextMenuPolicy(Qt::CustomContextMenu);
    sourceList->setSpacing(0);
    // The row picked shows it by its own frame: the list's highlight is made invisible (with its palette, not a
    // style sheet, which would keep the list from following the theme)
    {
        QPalette listPalette = sourceList->palette();
        listPalette.setColor(QPalette::All, QPalette::Highlight, Qt::transparent);
        sourceList->setPalette(listPalette);
    }
    sourceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    side->setMinimumWidth(300);
    sideLayout->addWidget(sourceList, 1);
    QHBoxLayout *addRow = new QHBoxLayout;
    QPushButton *addButton = new QPushButton(tr("Add source..."));
    addRow->addWidget(addButton, 1);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addSourcesDialog);
    // The panel of what is known of the source that is chosen (the history of an LDX file, the VBI, whether it was mapped...)
    infoButton = new QPushButton(tr("Info"));
    infoButton->setCheckable(true);
    infoButton->setToolTip(tr("Show what is known of the source that is chosen: where it is, the video, what the VBI says of the disc, whether it was mapped, its SNR and the history of an LDX file"));
    addRow->addWidget(infoButton);
    sideLayout->addLayout(addRow);

    // After a drag the rows are made again, in the new order
    connect(sourceList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        QVector<StackSource> reordered;
        for (qint32 row = 0; row < sourceList->count(); row++) {
            const qint32 index = project.indexOfSource(sourceList->item(row)->data(Qt::UserRole).toString());
            if (index >= 0) reordered.append(project.sources[index]);
        }
        if (reordered.size() == project.sources.size()) project.sources = reordered;
        QTimer::singleShot(0, this, [this]() {
            rebuildSourceList();
            rebuildTabs();
            sourcesChanged();
        });
    });
    connect(sourceList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = sourceList->itemAt(position);
        if (!item) return;
        QMenu menu;
        QAction *remove = menu.addAction(tr("Remove"));
        if (menu.exec(sourceList->viewport()->mapToGlobal(position)) == remove) removeSource(item->data(Qt::UserRole).toString());
    });
    // A row clicked is the tab shown (the list picks a current row of its own as it is filled, which is not a choice)
    const auto showRow = [this](QListWidgetItem *item) {
        if (!item) return;
        const qint32 index = project.indexOfSource(item->data(Qt::UserRole).toString());
        if (index < 0) return;
        if (stackButton->isChecked()) stackButton->setChecked(false);
        tabBar->setCurrentIndex(index);
    };
    connect(sourceList, &QListWidget::itemClicked, this, showRow);
    connect(sourceList, &QListWidget::itemActivated, this, showRow);
    QShortcut *deleteKey = new QShortcut(QKeySequence::Delete, sourceList);
    connect(deleteKey, &QShortcut::activated, this, [this]() {
        if (sourceList->currentItem()) removeSource(sourceList->currentItem()->data(Qt::UserRole).toString());
    });

    // The main area: tabs, picture, SNR, navigation and options
    QWidget *main = new QWidget;
    QVBoxLayout *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    tabBar = new QTabBar;
    tabBar->setExpanding(false);
    tabBar->setUsesScrollButtons(true);
    tabBar->setElideMode(Qt::ElideNone);
    tabBar->setDrawBase(true);
    mainLayout->addWidget(tabBar);

    imageLabel = new QLabel(tr("Add sources to start"));
    imageLabel->setAlignment(Qt::AlignCenter);
    scrollArea = new QScrollArea;
    scrollArea->setWidget(imageLabel);
    scrollArea->setWidgetResizable(true);
    scrollArea->setAlignment(Qt::AlignCenter);
    scrollArea->setBackgroundRole(QPalette::Dark);
    scrollArea->viewport()->installEventFilter(this);
    scrollArea->viewport()->setCursor(Qt::OpenHandCursor);
    mainLayout->addWidget(scrollArea, 1);

    snrPlot = new SnrGraph(false);
    snrPlot->setPlotFixedHeight(76);
    mainLayout->addWidget(snrPlot);
    dropoutPlot = new DropoutGraph;
    dropoutPlot->setVisible(false);
    mainLayout->addWidget(dropoutPlot);

    // Navigation
    QWidget *navigation = new QWidget;
    QHBoxLayout *navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(8, 6, 8, 2);
    // A button with an icon (those of ld-analyse), and its name as the tip
    const auto makeButton = [this](const QString &icon, const QString &tip, bool repeat) {
        QToolButton *button = new QToolButton;
        button->setIcon(QIcon(icon));
        button->setIconSize(QSize(22, 22));
        button->setToolTip(tip);
        button->setAutoRepeat(repeat);
        button->setAutoRepeatDelay(500);
        button->setAutoRepeatInterval(30);
        return button;
    };
    firstButton = makeButton(":/icons/start-frame.svg", tr("First frame"), false);
    previousButton = makeButton(":/icons/prev-frame.svg", tr("Previous frame"), true);
    nextButton = makeButton(":/icons/next-frame.svg", tr("Next frame"), true);
    lastButton = makeButton(":/icons/end-frame.svg", tr("Last frame"), false);
    frameSpin = new QSpinBox;
    frameSpin->setRange(1, 1);
    frameSpin->setKeyboardTracking(false);
    frameSlider = new QSlider(Qt::Horizontal);
    frameSlider->setRange(1, 1);
    totalLabel = new QLabel(tr("of 0"));
    navigationLayout->addWidget(firstButton);
    navigationLayout->addWidget(previousButton);
    navigationLayout->addWidget(frameSpin);
    navigationLayout->addWidget(nextButton);
    navigationLayout->addWidget(lastButton);
    navigationLayout->addWidget(frameSlider, 1);
    navigationLayout->addWidget(totalLabel);
    discLabel = new QLabel;
    discLabel->setForegroundRole(QPalette::PlaceholderText);
    discLabel->setToolTip(tr("Where the picture is on the disc (from the VBI), and in the file of the source that is shown"));
    navigationLayout->addWidget(discLabel);
    mainLayout->addWidget(navigation);

    // Options
    QWidget *options = new QWidget;
    QHBoxLayout *optionsLayout = new QHBoxLayout(options);
    optionsLayout->setContentsMargins(8, 2, 8, 6);
    modeCombo = new QComboBox;
    for (const char *name: MODE_NAMES) modeCombo->addItem(tr(name));
    modeCombo->setToolTip(tr("Stacking mode"));
    modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    chromaButton = new QPushButton(tr("Chroma"));
    chromaButton->setCheckable(true);
    chromaButton->setToolTip(tr("Decode the pictures to colour (with a 2D decoder)"));
    optionsLayout->addWidget(modeCombo);
    // The stack is a button, on or off: on, the picture is the stack, off it is the source of the tab chosen,
    // so that the two are compared by switching it
    stackButton = new QPushButton(tr("Stack"));
    stackButton->setIcon(dotIcon(STACK_COLOR));
    stackButton->setCheckable(true);
    stackButton->setChecked(false);       // (off to begin with: turning it on asks to map the sources that follow the map and have none)
    stackButton->setMinimumWidth(120);
    // Clearly on or off, whatever the theme
    const QString toggleStyle = "QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); }";
    stackButton->setStyleSheet(toggleStyle);
    chromaButton->setStyleSheet(toggleStyle);
    stackButton->setToolTip(tr("On: the picture is the stack. Off: it is the source of the tab chosen. (key 0 or S)"));
    optionsLayout->addWidget(stackButton);
    optionsLayout->addWidget(chromaButton);
    aspectButton = new QPushButton;
    optionsLayout->addWidget(aspectButton);
    // What the picture is: the frame, both fields of it one above the other, or one field with its lines doubled
    viewCombo = new QComboBox;
    viewCombo->addItems({ tr("Frame"), tr("Split"), tr("Field 1"), tr("Field 2") });
    viewCombo->setToolTip(tr("The picture: the frame, both fields (split, the first above the second), or one field with its lines doubled (key F goes through them)"));
    optionsLayout->addWidget(viewCombo);
    optionsLayout->addStretch(1);
    QToolButton *zoomOut = makeButton(":/icons/zoom-out.svg", tr("Zoom out"), true);
    QToolButton *zoomIn = makeButton(":/icons/zoom-in.svg", tr("Zoom in"), true);
    zoomSpin = new QSpinBox;
    zoomSpin->setRange(25, 600);
    zoomSpin->setSingleStep(25);
    zoomSpin->setSuffix("%");
    zoomSpin->setKeyboardTracking(false);
    zoomSpin->setToolTip(tr("Zoom: type a value, or use the + and - buttons"));
    // As tall as the zoom buttons beside it
    zoomSpin->setFixedHeight(zoomOut->sizeHint().height());
    optionsLayout->addWidget(zoomOut);
    optionsLayout->addWidget(zoomSpin);
    optionsLayout->addWidget(zoomIn);
    originalSizeButton = makeButton(":/icons/zoom-original.svg", tr("Original size: zoom to 100% and resize the window to show the whole picture"), false);
    optionsLayout->addWidget(originalSizeButton);
    mainLayout->addWidget(options);

    infoPanel = new SourceInfoPanel;
    infoPanel->setVisible(false);
    connect(infoPanel, &SourceInfoPanel::mapRequested, this, &MainWindow::mapSelectedSource);
    connect(infoPanel, &SourceInfoPanel::snrUpdateRequested, this, &MainWindow::updateSnrOfSelectedSource);
    connect(infoButton, &QPushButton::toggled, this, [this](bool on) {
        // The window gets wider by the room of the panel when it is shown, and narrower by it when it is hidden (the picture keeps its room), unless the
        // window is maximized or fills the screen
        const bool resizes = !isMaximized() && !isFullScreen();
        const qint32 handle = mainSplitter ? mainSplitter->handleWidth() : 0;
        if (on) {
            infoPanel->setVisible(true);
            if (mainSplitter) mainSplitter->setSizes({ sourceList->parentWidget() ? sourceList->parentWidget()->width() : 340, infoPanelWidth, mainSplitter->widget(2)->width() });
            if (resizes) resize(width() + infoPanelWidth + handle, height());
        } else {
            if (infoPanel->isVisible() && infoPanel->width() > 0) infoPanelWidth = infoPanel->width();
            infoPanel->setVisible(false);
            // (the window cannot get narrower while its layout still counts the panel: it does it when the layout has been made again)
            if (resizes) {
                const qint32 target = width() - infoPanelWidth - handle;
                QTimer::singleShot(0, this, [this, target]() {
                    // (the minimum width that the layout set while the panel was there is made again, or the window would not go below it)
                    if (layout()) {
                        layout()->invalidate();
                        layout()->activate();
                    }
                    if (!isMaximized() && !isFullScreen() && !infoPanel->isVisible()) resize(qMax(minimumSizeHint().width(), target), height());
                });
            }
        }
        updateInfoPanel();
    });

    QSplitter *splitter = new QSplitter;
    mainSplitter = splitter;
    splitter->addWidget(side);
    splitter->addWidget(infoPanel);
    splitter->addWidget(main);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({ 340, 300, 880 });
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    // (the panel is hidden when the program starts)
    infoButton->setChecked(false);

    // The status bar: what contributes, and how
    statusSources = new QLabel;
    statusMode = new QLabel;
    statusFormat = new QLabel;
    statusShowing = new QLabel;
    statusBar()->addWidget(statusSources);
    statusBar()->addWidget(statusMode);
    hideCaptureDropouts = new QCheckBox(tr("Hide capture dropouts"));
    hideCaptureDropouts->setToolTip(tr("The dropouts of the captures (the ones their metadata have) are not drawn, whatever the boxes of the sources say"));
    showResidualDropouts = new QCheckBox(tr("Show residual dropouts"));
    showResidualDropouts->setToolTip(tr("On the stack, draws in red the dropouts that the stacking left: where every source that has the frame has a dropout"));
    connect(hideCaptureDropouts, &QCheckBox::toggled, this, [this]() { requestFrame(); });
    connect(showResidualDropouts, &QCheckBox::toggled, this, [this]() { requestFrame(); });
    statusBar()->addPermanentWidget(hideCaptureDropouts);
    statusBar()->addPermanentWidget(showResidualDropouts);
    statusBar()->addPermanentWidget(statusFormat);
    statusBar()->addPermanentWidget(statusShowing);

    // Wiring
    connect(tabBar, &QTabBar::currentChanged, this, [this](qint32 index) {
        if (index < 0 || index >= project.sources.size()) return;
        // Choosing a source shows it: the stack is off
        project.selected = project.sources[index].id;
        updateInfoPanel();
        project.showStack = false;
        updateFollowEnabled();
        {
            const QSignalBlocker blocker(stackButton);
            stackButton->setChecked(false);
            const QSignalBlocker listBlocker(sourceList);
            for (qint32 row = 0; row < sourceList->count(); row++) {
                if (sourceList->item(row)->data(Qt::UserRole).toString() == project.selected) sourceList->setCurrentRow(row);
            }
        }
        viewChanged();
    });
    connect(stackButton, &QPushButton::toggled, this, [this](bool on) {
        // Turning the stack on: the sources that follow the map and have no mapping are offered to be mapped first
        if (on && !confirmMappingBeforeStack()) {
            const QSignalBlocker blocker(stackButton);
            stackButton->setChecked(false);
            return;
        }
        project.showStack = on;
        updateFollowEnabled();
        updateInfoPanel();
        viewChanged();
        ensureStackSnr();
    });
    connect(frameSpin, &QSpinBox::valueChanged, this, [this](qint32 value) { setFrame(value + viewOffset()); });
    connect(frameSlider, &QSlider::valueChanged, this, [this](qint32 value) { setFrame(value + viewOffset()); });
    connect(firstButton, &QToolButton::clicked, this, [this]() { setFrame(viewOffset() + 1); });
    connect(previousButton, &QToolButton::clicked, this, [this]() { setFrame(frame - 1); });
    connect(nextButton, &QToolButton::clicked, this, [this]() { setFrame(frame + 1); });
    connect(lastButton, &QToolButton::clicked, this, [this]() { setFrame(viewOffset() + viewFrames()); });
    connect(snrPlot, &SnrGraph::frameRequested, this, [this](qint32 requested) { setFrame(requested + viewOffset()); });
    connect(dropoutPlot, &DropoutGraph::frameRequested, this, [this](qint32 requested) { setFrame(requested + viewOffset()); });
    connect(snrPlot, &SnrGraph::dropoutsToggled, this, &MainWindow::setDropoutGraphVisible);
    measureMode = qBound(0, QSettings().value("measureMode", 0).toInt(), 2);
    connect(snrPlot, &SnrGraph::measureModeChanged, this, [this](qint32 mode) {
        measureMode = mode;
        QSettings().setValue("measureMode", mode);
        // (what was measured stays; the measures in progress stop and the new ones go on from the start)
        backend->snrGeneration.store(++snrJob);
        stackSnrActive = false;
        ensureStackSnr();
        backend->sourceSnrGeneration.store(++sourceSnrJob);
        measuringId.clear();
        measureQueue.clear();
        for (const StackSource &source: project.sources) {
            const SourceState state = states.value(source.id);
            if (state.loaded && state.measure && !state.measuredFull && measureMode != 2) measureQueue.append(source.id);
        }
        startNextMeasure();
        updatePlot();
    });
    connect(snrPlot, &SnrGraph::plotHeightChanged, this, &MainWindow::resizeHeightBy);
    connect(snrPlot, &SnrGraph::measureToggled, this, [this](bool on) {
        const QString id = measureTargetId();
        if (!id.isEmpty()) setSourceMeasure(id, on);
    });
    connect(modeCombo, &QComboBox::currentIndexChanged, this, [this](qint32 index) {
        project.stacking.mode = index - 1;
        sourcesChanged();
    });
    connect(chromaButton, &QPushButton::toggled, this, [this](bool on) {
        project.chroma = on;
        requestFrame();
    });
    connect(zoomIn, &QToolButton::clicked, this, [this]() { stepZoom(1); });
    connect(zoomOut, &QToolButton::clicked, this, [this]() { stepZoom(-1); });
    connect(viewCombo, &QComboBox::currentIndexChanged, this, [this](qint32 index) {
        fieldView = index != 0;
        fieldSplit = index == 1;
        fieldIndex = index == 3 ? 1 : 0;
        project.fieldView = fieldView;
        project.fieldSplit = fieldSplit;
        project.fieldIndex = fieldIndex;
        viewModeChanged();
    });
    connect(aspectButton, &QPushButton::clicked, this, [this]() {
        displayAspect = !displayAspect;
        project.aspect = displayAspect;
    project.fieldView = fieldView;
    project.fieldSplit = fieldSplit;
        updateAspectButton();
        applyZoom();
        if (stackWindow->isVisible() && lastStackResult.view == "stack") showStackPicture(lastStackResult);
    });
    connect(originalSizeButton, &QToolButton::clicked, this, [this]() {
        setZoom(1.0);
        resizeToImage();
    });
    connect(zoomSpin, &QSpinBox::valueChanged, this, [this](qint32 percent) { setZoom(percent / 100.0); });

    new QShortcut(Qt::Key_F, this, [this]() { viewCombo->setCurrentIndex((viewCombo->currentIndex() + 1) % viewCombo->count()); });
    new QShortcut(Qt::Key_Left, this, [this]() { setFrame(frame - 1); });
    new QShortcut(Qt::Key_Right, this, [this]() { setFrame(frame + 1); });
    // 1 to 9 pick a source, 0 (or S) switches the stack
    for (qint32 digit = 1; digit <= 9; digit++) {
        new QShortcut(QKeySequence(Qt::Key_0 + digit), this, [this, digit]() {
            if (digit - 1 < tabBar->count()) tabBar->setCurrentIndex(digit - 1);
        });
    }
    new QShortcut(QKeySequence(Qt::Key_0), this, [this]() { stackButton->toggle(); });
    new QShortcut(QKeySequence(Qt::Key_S), this, [this]() { stackButton->toggle(); });

    rebuildTabs();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New session"), QKeySequence::New, this, &MainWindow::newSession);
    file->addAction(tr("&Open session..."), QKeySequence::Open, this, &MainWindow::openSession);
    file->addAction(tr("&Add source..."), this, &MainWindow::addSourcesDialog);
    file->addSeparator();
    file->addAction(tr("&Export session..."), QKeySequence::Save, this, &MainWindow::exportSession);
    file->addAction(tr("&Process..."), this, &MainWindow::processSession);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Stacking options..."), this, [this]() { optionsDialog->show(); optionsDialog->raise(); });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    stackOutputAction = view->addAction(tr("Stack output"));
    stackOutputAction->setCheckable(true);
    stackOutputAction->setToolTip(tr("A window with the final stack, which stays in view while the main window shows a source"));
    connect(stackOutputAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            stackWindow->show();
            stackWindow->raise();
            requestFrame();
        } else {
            stackWindow->hide();
        }
    });
    dropoutGraphAction = view->addAction(tr("Dropout graph"));
    dropoutGraphAction->setCheckable(true);
    dropoutGraphAction->setToolTip(tr("The graph of the dropouts of the captures, under the SNR graph"));
    connect(dropoutGraphAction, &QAction::toggled, this, &MainWindow::setDropoutGraphVisible);
    dropoutGraphAction->setChecked(QSettings().value("dropoutGraph", false).toBool());
    view->addAction(tr("&Chroma decoder configuration..."), this, [this]() { chromaDialog->show(); chromaDialog->raise(); });
    view->addAction(tr("&Enhance..."), this, [this]() { enhanceDialog->show(); enhanceDialog->raise(); });
    // The first source imported sets the size of the picture and of the window, unless this is off (kept in the settings)
    resizeOnFirstImport = QSettings().value("resizeOnFirstImport", true).toBool();
    QAction *resizeAction = view->addAction(tr("Resize on first file import"));
    resizeAction->setCheckable(true);
    resizeAction->setChecked(resizeOnFirstImport);
    resizeAction->setToolTip(tr("The first source that is imported is shown at its original size and the window is resized to it"));
    connect(resizeAction, &QAction::toggled, this, [this](bool on) {
        resizeOnFirstImport = on;
        QSettings().setValue("resizeOnFirstImport", on);
    });
    // The window follows the zoom, when this is on (kept in the settings; off to begin with: the zoom changes the picture in the window as it is)
    scaleWindowWithZoom = QSettings().value("scaleWindowWithZoom", false).toBool();
    QAction *scaleAction = view->addAction(tr("Scale window with zoom"));
    scaleAction->setCheckable(true);
    scaleAction->setChecked(scaleWindowWithZoom);
    scaleAction->setToolTip(tr("The window is resized to the picture each time the zoom changes"));
    connect(scaleAction, &QAction::toggled, this, [this](bool on) {
        scaleWindowWithZoom = on;
        QSettings().setValue("scaleWindowWithZoom", on);
        if (on) resizeToImage();
    });
    // How the picture is enlarged: line doubling keeps the pixels as they are
    QMenu *enlarge = view->addMenu(tr("Scaling algorithm"));
    QActionGroup *enlargeGroup = new QActionGroup(this);
    const QStringList methods = { tr("Line doubling"), tr("Smooth") };
    for (qint32 i = 0; i < methods.size(); i++) {
        QAction *action = enlarge->addAction(methods[i]);
        action->setCheckable(true);
        action->setChecked(i == zoomMethod);
        enlargeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            zoomMethod = i;
            applyZoom();
            if (stackWindow->isVisible() && lastStackResult.view == "stack") showStackPicture(lastStackResult);
        });
    }
    QAction *sameScaleAction = view->addAction(tr("Same scale for all the SNR graphs"));
    sameScaleAction->setCheckable(true);
    snrSameScale = QSettings().value("snrSameScale", true).toBool();
    sameScaleAction->setChecked(snrSameScale);
    sameScaleAction->setToolTip(tr("The graphs of the sources and of the stack share one vertical scale, which holds all of them, so that they can be compared. Off: each graph has the scale that fits what it shows."));
    connect(sameScaleAction, &QAction::toggled, this, [this](bool on) {
        snrSameScale = on;
        QSettings().setValue("snrSameScale", on);
        updatePlot();
    });
    QAction *tintAction = view->addAction(tr("Tint the source rows"));
    tintAction->setCheckable(true);
    tintRows = QSettings().value("tintRows", true).toBool();
    tintAction->setChecked(tintRows);
    tintAction->setToolTip(tr("Give each row of the list of sources a faint tint of the colour of its source"));
    connect(tintAction, &QAction::toggled, this, [this](bool on) {
        tintRows = on;
        QSettings().setValue("tintRows", on);
        rebuildSourceList();
    });
    view->addSeparator();
    setupAppearanceMenus(view);
}

// ---------------------------------------------------------------------------------------------
// Sources and session

QColor MainWindow::colorFor(const QString &id)
{
    SourceState &state = states[id];
    if (!state.color.isValid()) state.color = SOURCE_COLORS[colorCounter++ % SOURCE_COLOR_COUNT];
    return state.color;
}

QString MainWindow::sourceTitle(const StackSource &source) const
{
    // (the track of a capture that came by a pipe can be named after the pipe, which says nothing: the name of the file does then)
    static const QStringList pipeNames = { QStringLiteral("stdin"), QStringLiteral("stdin.tbc"), QStringLiteral("stdin.cvbs"), QStringLiteral("-"), QStringLiteral("/dev/stdin") };
    if (!source.label.isEmpty() && !pipeNames.contains(source.label.trimmed(), Qt::CaseInsensitive)) return source.label;
    return QFileInfo(source.file).fileName() + (source.track > 0 ? QString(" #%1").arg(source.track) : QString());
}

void MainWindow::addSourcesDialog()
{
    QSettings settings;
    const QString directory = settings.value("sourceDirectory").toString();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add sources"), directory,
                                                            tr("TBC, CVBS, LDX and stack files (*.tbc *.cvbs *.ldx *.stack);;TBC (*.tbc);;CVBS (*.cvbs);;LDX (*.ldx);;Stacking sessions (*.stack);;All files (*)"));
    if (files.isEmpty()) return;
    settings.setValue("sourceDirectory", QFileInfo(files.first()).absolutePath());
    addSources(files);
}

void MainWindow::addSources(const QStringList &files)
{
    // The first source imported shows at its original size; those after it change nothing
    if (project.sources.isEmpty() && resizeOnFirstImport) fitFirstSource = true;
    for (const QString &name: files) {
        // What a name gives: a capture, or, for an LDX file that holds several and is not named by a track, each of them
        QVector<qint32> tracks = { 0 };
        QStringList names = { QString() };
        QString path;
        qint32 track = 0;
        LdxCapture::split(name, path, track);
        if (LdxCapture::isLdxName(name) && track == 0 && !LdxCapture::hasSession(path)) {
            QString error;
            tracks.clear();
            names.clear();
            if (!LdxCapture::listCaptures(path, tracks, names, error) || tracks.isEmpty()) {
                QMessageBox::warning(this, tr("Add source"), error.isEmpty() ? tr("%1 holds no TBC capture").arg(QFileInfo(path).fileName()) : error);
                continue;
            }
        } else if (track > 0) {
            tracks = { track };
        }
        for (qint32 i = 0; i < tracks.size(); i++) {
            if (tracks[i] > 0 && LdxCapture::isMappedTrack(LdxCapture::trackName(path, tracks[i]))) continue;
            StackSource source;
            source.file = QFileInfo(path).absoluteFilePath();
            source.track = tracks[i];
            if (tracks[i] > 0 && i < names.size()) source.label = names[i];
            source.id = project.newSourceId();
            project.sources.append(source);
            startSource(source);
        }
    }
    rebuildSourceList();
    rebuildTabs();
}

void MainWindow::startSource(const StackSource &source)
{
    colorFor(source.id);
    states[source.id].followMap = source.followMap;
    states[source.id].loaded = false;
    states[source.id].failed = false;
    QByteArray mapJson;
    if (const FrameMap *map = source.activeMap()) mapJson = map->toJsonBytes();
    QMetaObject::invokeMethod(backend, "addSource", Qt::QueuedConnection, Q_ARG(QString, source.id),
                              Q_ARG(QString, source.reference()), Q_ARG(bool, project.stacking.reverseFields), Q_ARG(QByteArray, mapJson), Q_ARG(bool, source.followMap));
}

void MainWindow::removeSource(const QString &id)
{
    const qint32 index = project.indexOfSource(id);
    if (index < 0) return;
    measureQueue.removeAll(id);
    if (measuringId == id) {
        backend->sourceSnrGeneration.store(++sourceSnrJob);
        measuringId.clear();
    }
    project.sources.remove(index);
    states.remove(id);
    delete snrWindows.take(id);
    delete dropoutWindows.take(id);
    QMetaObject::invokeMethod(backend, "removeSource", Qt::QueuedConnection, Q_ARG(QString, id));
    QTimer::singleShot(0, this, [this]() { startNextMeasure(); });
    rebuildSourceList();
    rebuildTabs();
    sourcesChanged();
}

void MainWindow::loadSession(const QString &fileName)
{
    StackProject loaded;
    QString error;
    if (!loaded.load(fileName, error)) {
        QMessageBox::warning(this, tr("Open session"), error);
        return;
    }
    applySession(loaded, fileName);
}

void MainWindow::newSession()
{
    if (mappingBusy) return;
    if (!project.sources.isEmpty()
        && QMessageBox::question(this, tr("New session"), tr("Start an empty session? The sources are taken out of the list; the files are not touched, and the session is not saved."),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    StackProject empty;
    applySession(empty, QString());
}

void MainWindow::applySession(const StackProject &loaded, const QString &fileName)
{
    fitFirstSource = false;   // a session has its own zoom
    fitPicture = false;
    fitWindow = true;

    // What was loaded is let go of
    for (const StackSource &source: project.sources) {
        QMetaObject::invokeMethod(backend, "removeSource", Qt::QueuedConnection, Q_ARG(QString, source.id));
    }
    states.clear();
    // (the measures of the SNR of the sources that were open stop)
    measureQueue.clear();
    measuringId.clear();
    backend->sourceSnrGeneration.store(++sourceSnrJob);
    colorCounter = 0;
    stackWhite.clear();
    stackBlack.clear();
    stackSnrDone = 0;
    currentImage = QImage();

    qDeleteAll(snrWindows);
    snrWindows.clear();
    qDeleteAll(dropoutWindows);
    dropoutWindows.clear();

    // What the session says was measured is used when it is good for these captures (the keys say)
    for (const StackMeasure &measure: loaded.measures) snrCache.insert(measure.key, { measure.white, measure.black, measure.residual });

    project = loaded;
    // (the stacking is off when a session is opened too: turning it on asks to map the sources that follow the map and have no mapping)
    project.showStack = false;
    // (the session an LDX file keeps is saved to a .stack file of its own, never over the LDX file)
    sessionFile = (fileName.isEmpty() || LdxCapture::isLdxName(fileName)) ? QString() : fileName;
    for (const StackSource &source: project.sources) startSource(source);

    {
        const QSignalBlocker blocker(modeCombo);
        modeCombo->setCurrentIndex(qBound(0, project.stacking.mode + 1, modeCombo->count() - 1));
    }
    setZoom(project.zoom);
    displayAspect = project.aspect;
    updateAspectButton();
    fieldView = project.fieldView;
    fieldSplit = project.fieldSplit;
    fieldIndex = project.fieldIndex;
    updateViewCombo();
    syncDialogs();
    {
        const QSignalBlocker blocker(chromaButton);
        chromaButton->setChecked(project.chroma);
    }
    setWindowTitle(fileName.isEmpty() ? tr("ld-stacker-gui") : tr("ld-stacker-gui - %1").arg(QFileInfo(fileName).fileName()));

    rebuildSourceList();
    rebuildTabs();
    sourcesChanged();
}

void MainWindow::openSession()
{
    QSettings settings;
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Open session"), settings.value("sessionDirectory").toString(),
                                                          tr("Stack sessions (*.stack *.ldx);;All files (*)"));
    if (fileName.isEmpty()) return;
    settings.setValue("sessionDirectory", QFileInfo(fileName).absolutePath());
    loadSession(fileName);
}

// Stack the sources to a file
void MainWindow::processSession()
{
    QString suggested = project.outputFile;
    if (suggested.isEmpty() && !project.sources.isEmpty()) {
        const QFileInfo first(project.sources.first().file);
        suggested = first.absoluteDir().absoluteFilePath(first.completeBaseName() + "-stack." + first.suffix());
    }
    ProcessDialog dialog(projectForProcessing(), suggested, this);
    dialog.exec();
}

void MainWindow::exportSession()
{
    QSettings settings;
    QString suggested = sessionFile.isEmpty() ? settings.value("sessionDirectory").toString() + "/session.stack" : sessionFile;
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Export session"), suggested, tr("Stack sessions (*.stack)"));
    if (fileName.isEmpty()) return;
    settings.setValue("sessionDirectory", QFileInfo(fileName).absolutePath());

    project.zoom = zoom;
    project.aspect = displayAspect;
    // What was measured of these captures goes in the file: the SNR and the dropouts of the stack, and the SNR measured of each source
    project.measures.clear();
    for (const bool withResidual: { false, true }) {
        const auto found = snrCache.constFind(stackCacheKey(withResidual));
        if (found != snrCache.constEnd()) project.measures.append({ found.key(), found->white, found->black, found->residual });
    }
    for (const StackSource &source: project.sources) {
        const auto found = snrCache.constFind(source.measureKey());
        if (found != snrCache.constEnd()) project.measures.append({ found.key(), found->white, found->black, QVector<float>() });
    }
    if (project.outputFile.isEmpty() && !project.sources.isEmpty()) {
        // Where the stack would go: beside the session, named after it
        project.outputFile = QFileInfo(fileName).absoluteDir().absoluteFilePath(QFileInfo(fileName).completeBaseName() + ".tbc");
    }

    QString error;
    if (!project.save(fileName, error)) {
        QMessageBox::warning(this, tr("Export session"), error);
        return;
    }
    // (the session an LDX file keeps is saved to a .stack file of its own, never over the LDX file)
    sessionFile = LdxCapture::isLdxName(fileName) ? QString() : fileName;
    setWindowTitle(tr("ld-stacker-gui - %1").arg(QFileInfo(fileName).fileName()));
    statusBar()->showMessage(tr("Session saved to %1").arg(fileName), 5000);
}

// A source has been loaded (or has not)
void MainWindow::on_sourceLoaded(const SourceInfo &info)
{
    if (project.indexOfSource(info.id) < 0) return;   // removed meanwhile

    // The sources are of one video system: a source of the other one is not kept
    if (info.ok) {
        for (const StackSource &other: project.sources) {
            if (other.id == info.id) continue;
            const SourceState otherState = states.value(other.id);
            if (otherState.loaded && otherState.info.system != info.system) {
                const QString name = QFileInfo(info.file).fileName();
                const QString system = info.system == NTSC ? "NTSC" : (info.system == PAL ? "PAL" : "PAL-M");
                const QString existing = otherState.info.system == NTSC ? "NTSC" : (otherState.info.system == PAL ? "PAL" : "PAL-M");
                removeSource(info.id);
                QMessageBox::warning(this, tr("Add source"), tr("%1 is %2, but the sources already here are %3: PAL and NTSC cannot be stacked together.").arg(name, system, existing));
                return;
            }
        }
        for (const StackSource &other: project.sources) {
            if (other.id == info.id) continue;
            const SourceState otherState = states.value(other.id);
            if (otherState.loaded && otherState.info.cvbs != info.cvbs) {
                const QString name = QFileInfo(info.file).fileName();
                removeSource(info.id);
                QMessageBox::warning(this, tr("Add source"), tr("%1 is %2, but the sources already here are %3: TBC and CVBS cannot be stacked together.")
                                     .arg(name, info.cvbs ? "CVBS" : "TBC", otherState.info.cvbs ? "CVBS" : "TBC"));
                return;
            }
        }
        chromaDialog->setSystem(info.system, true);
    }

    if (info.ok && fitFirstSource) {
        fitFirstSource = false;
        fitPicture = true;
    }

    SourceState &state = states[info.id];
    state.info = info;
    state.loaded = info.ok;
    state.failed = !info.ok;
    if (!info.ok) statusBar()->showMessage(info.error, 10000);

    updateAspectButton();
    rebuildSourceList();
    updateTabTexts();
    sourcesChanged();
    if (info.ok) resolveFollowMap(info.id);
}

// The bar of the mapping in the row of a source: hidden when nothing is going on, a bar that fills (or goes back and forth while there is no count),
// and red with "Map failed" when ld-discmap did not succeed
void MainWindow::updateMapBar(const QString &id)
{
    QProgressBar *bar = mapBars.value(id);
    if (!bar) return;
    const SourceState state = states.value(id);
    const bool show = state.mapProgress >= -1 || state.mapFailed;
    bar->setVisible(show);
    if (!show) return;
    if (state.mapFailed) {
        bar->setRange(0, 100);
        bar->setValue(100);
        bar->setFormat(tr("Map failed"));
        bar->setStyleSheet(QStringLiteral("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #d33a3a; }"));
    } else if (state.mapProgress < 0) {
        bar->setRange(0, 0);
        bar->setFormat(QString());
        bar->setStyleSheet(QString());
    } else {
        bar->setRange(0, 100);
        bar->setValue(state.mapProgress);
        bar->setFormat(tr("Mapping %p%"));
        bar->setStyleSheet(QString());
    }
    // (the row keeps the room of its bar, so that the list does not jump when it appears)
    for (qint32 row = 0; row < sourceList->count(); row++) {
        QListWidgetItem *item = sourceList->item(row);
        if (item->data(Qt::UserRole).toString() != id) continue;
        if (QWidget *widget = sourceList->itemWidget(item)) item->setSizeHint(widget->sizeHint() + QSize(0, 2));
    }
}

// What cannot be done while sources are being mapped: the stack, the Map button, the Follow map boxes
void MainWindow::updateMappingControls()
{
    stackButton->setEnabled(!mappingBusy);
    updateFollowEnabled();
    updateInfoPanel();
}

// The sources that the stack would use and that follow the map but have no mapping: TBC captures that are on, whose metadata do not say they were
// mapped (a mapping that exists was taken in their place when they were loaded). followingOnly false: all that can be mapped.
QVector<qint32> MainWindow::sourcesToMap(bool followingOnly) const
{
    QVector<qint32> indexes;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        const SourceState state = states.value(source.id);
        if (!state.loaded || state.info.isMapped || state.info.cvbs) continue;
        if (followingOnly && (!source.enabled || !state.followMap)) continue;
        // (a track of an LDX file is mapped as a track of its own; the chroma track of a pair is not)
        if (LdxCapture::isLdxName(source.reference())) {
            if (LdxCapture::isChromaTrack(source.reference())) continue;
        } else if (TbcFiles::mappedFileName(source.file).isEmpty()) {
            continue;
        }
        indexes.append(index);
    }
    return indexes;
}

// The Stack button: if some sources that follow the map have none, ask whether to map them
bool MainWindow::confirmMappingBeforeStack()
{
    const QVector<qint32> needing = sourcesToMap(true);
    if (needing.isEmpty()) return true;
    QStringList names;
    for (const qint32 index: needing) names << sourceTitle(project.sources[index]);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Map the sources?"));
    box.setText(needing.size() == 1 ? tr("This source has no map:\n%1").arg(names.join("\n"))
                                    : tr("These sources have no map:\n%1").arg(names.join("\n")));
    box.setInformativeText(tr("Mapping puts the frames in the order of the disc and pads the missing ones, so that the sources line up. The map is kept in the session; it can also be kept in the metadata of each source (its .tbc.db, .tbc.json or LDX file), where every tool that reads the capture finds it. The captures themselves are not written again."));
    QPushButton *map = box.addButton(tr("Map, in the session"), QMessageBox::AcceptRole);
    QPushButton *mapAndSave = box.addButton(tr("Map, save in the sources"), QMessageBox::AcceptRole);
    QPushButton *without = box.addButton(tr("Stack without mapping"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(map);
    box.exec();
    if (box.clickedButton() == without) return true;
    if (box.clickedButton() != map && box.clickedButton() != mapAndSave) return false;
    // (the mapping goes on in the window, and the stack is turned on when it is done)
    startMapping(needing, false, true, MapDialog::Map, QString(), box.clickedButton() == mapAndSave);
    return false;
}

// Map the sources with ld-discmap, in the background: a bar in the row of each source shows how it goes (and a window has the log of the tool). A capture that
// is overwritten has to be let go of first (a file that is open cannot be replaced on Windows) and is read again afterwards.
bool MainWindow::startMapping(const QVector<qint32> &indexes, bool overwrite, bool thenStack, MapDialog::Task task, const QString &inputOverride, bool saveInSources)
{
    if (mappingBusy || indexes.isEmpty()) return false;
    QVector<MapDialog::Job> jobs;
    QStringList ids;
    for (const qint32 index: indexes) {
        MapDialog::Job job;
        job.id = project.sources[index].id;
        job.title = sourceTitle(project.sources[index]);
        job.input = inputOverride.isEmpty() ? project.sources[index].reference() : inputOverride;
        jobs.append(job);
        ids << job.id;
    }
    // The sources that are let go of, and read again afterwards: with an overwrite, those that are mapped; and, for a track of an LDX file (the file is
    // written again, whether the track is replaced or the mapping is added), every source that reads that file
    QStringList reload;
    if (overwrite || task == MapDialog::UpdateSnr) reload = ids;
    for (const MapDialog::Job &job: jobs) {
        if (task != MapDialog::UpdateSnr || !MapDialog::isLdxJob(job)) continue;
        QString path;
        qint32 track;
        LdxCapture::split(job.input, path, track);
        for (const StackSource &other: project.sources) {
            if (reload.contains(other.id) || !LdxCapture::isLdxName(other.reference())) continue;
            QString otherPath;
            qint32 otherTrack;
            LdxCapture::split(other.reference(), otherPath, otherTrack);
            if (QFileInfo(otherPath) == QFileInfo(path)) reload << other.id;
        }
    }
    if (!reload.isEmpty()) {
        // (what measures the SNR of the sources, and their handles on the files, stop)
        backend->snrGeneration.store(++snrJob);
        backend->sourceSnrGeneration.store(++sourceSnrJob);
        measureQueue.clear();
        measuringId.clear();
        for (const QString &id: reload) {
            QMetaObject::invokeMethod(backend, "removeSource", Qt::BlockingQueuedConnection, Q_ARG(QString, id));
            states[id].loaded = false;
        }
        rebuildSourceList();
    }
    for (const QString &id: ids) {
        states[id].mapProgress = -1;
        states[id].mapFailed = false;
        updateMapBar(id);
    }
    mappingBusy = true;
    updateMappingControls();
    mapDialog = new MapDialog(jobs, overwrite, this, task);
    mapDialog->setSaveInSources(saveInSources);
    mapDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(mapDialog, &MapDialog::jobStarted, this, [this](const QString &id) {
        if (!states.contains(id)) return;
        states[id].mapProgress = -1;
        updateMapBar(id);
    });
    connect(mapDialog, &MapDialog::jobProgress, this, [this](const QString &id, qint32 percent) {
        if (!states.contains(id)) return;
        states[id].mapProgress = percent;
        updateMapBar(id);
    });
    connect(mapDialog, &MapDialog::jobFinished, this, [this](const QString &id, bool ok) {
        if (!states.contains(id)) return;
        states[id].mapProgress = -2;
        states[id].mapFailed = !ok;
        updateMapBar(id);
    });
    connect(mapDialog, &MapDialog::allFinished, this, [this, ids, reload, overwrite, thenStack, task](bool success) {
        // (the sources whose SNR was measured are only read again)
        mappingFinished(task == MapDialog::UpdateSnr ? QStringList() : ids, reload, overwrite, thenStack, success);
    });
    mapDialog->show();
    mapDialog->start();
    return true;
}

// ld-discmap is done (or stopped): the sources that were mapped read their mapping, and those that were let go of are read again
void MainWindow::mappingFinished(const QStringList &ids, const QStringList &reload, bool overwrite, bool thenStack, bool success)
{
    const QStringList mapped = mapDialog ? mapDialog->mappedIds() : QStringList();
    QHash<QString, QString> replaced;
    for (const QString &id: mapped) {
        if (mapDialog && !mapDialog->replacedReference(id).isEmpty()) replaced.insert(id, mapDialog->replacedReference(id));
    }
    mappingBusy = false;
    // (the other sources of a file that was written again read it as it is now)
    for (const QString &id: reload) {
        const qint32 index = project.indexOfSource(id);
        if (index < 0 || ids.contains(id)) continue;
        switchSource(id, project.sources[index].reference());
    }
    for (const QString &id: ids) {
        const qint32 index = project.indexOfSource(id);
        if (index < 0) continue;
        // The source that was mapped has the map in the session now, and is read through it (the capture is not written)
        if (const QByteArray bytes = mapDialog ? mapDialog->mapFor(id) : QByteArray(); !bytes.isEmpty()) {
            StackSource &source = project.sources[index];
            QString mapError;
            if (FrameMap::fromJsonBytes(bytes, source.map, mapError)) {
                // (a map kept in the metadata of the source is read from there: the session does not keep a second one that could go stale)
                source.hasMap = !(mapDialog && mapDialog->savedIds().contains(id));
                source.followMap = true;
                states[id].followMap = true;
                if (followBoxes.contains(id)) {
                    const QSignalBlocker blocker(followBoxes.value(id));
                    followBoxes.value(id)->setChecked(true);
                }
                switchSource(id, source.reference());
            } else {
                QMessageBox::warning(this, tr("Map the sources"), tr("The map of %1 cannot be used: %2").arg(sourceTitle(source), mapError));
            }
            continue;
        }
        if (overwrite) {
            // (the capture is the mapping now, or as it was if the mapping failed: it is read again either way; a track of an LDX file that was replaced is
            // another track, which the mapping took the place of)
            switchSource(id, replaced.value(id, project.sources[index].reference()));
        } else if (reload.contains(id)) {
            // (the file was written again: the source is read again, and the mapping that was added to it is taken if it was made)
            switchSource(id, project.sources[index].reference());
            if (mapped.contains(id)) {
                setFollowMap(id, true);
                if (followBoxes.contains(id)) {
                    const QSignalBlocker blocker(followBoxes.value(id));
                    followBoxes.value(id)->setChecked(true);
                }
            }
        } else if (mapped.contains(id)) {
            // (the mapping was asked for: it is the one that is used)
            setFollowMap(id, true);
            if (followBoxes.contains(id)) {
                const QSignalBlocker blocker(followBoxes.value(id));
                followBoxes.value(id)->setChecked(true);
            }
            resolveFollowMap(id);
        }
    }
    updateMappingControls();
    if (!success) {
        QMessageBox::warning(this, tr("Map the sources"), tr("The mapping was not finished: the sources that it did not reach are as they were."));
        return;
    }
    if (thenStack) stackButton->setChecked(true);
}

// The Update SNR button of the panel: the SNR of the source that is chosen is measured again from its video and written in its metadata
void MainWindow::updateSnrOfSelectedSource()
{
    const QString id = project.selected;
    const qint32 index = project.indexOfSource(id);
    if (index < 0 || project.showStack || mappingBusy) return;
    const QString reference = project.sources[index].reference();

    // The two versions of the capture, when it has a mapping: the SNR is written in the one that is chosen
    QString mappedVersion, originalVersion;
    if (LdxCapture::isLdxName(reference)) {
        const bool isMapped = LdxCapture::isMappedTrack(reference);
        mappedVersion = isMapped ? reference : LdxCapture::mappedOfOriginal(reference);
        originalVersion = isMapped ? LdxCapture::originalOfMapped(reference) : reference;
    } else {
        const bool isMapped = TbcFiles::isMappedFileName(reference);
        mappedVersion = isMapped ? reference : TbcFiles::mappedFileName(reference);
        originalVersion = isMapped ? TbcFiles::originalFileName(reference) : reference;
        if (mappedVersion.isEmpty() || !QFileInfo::exists(mappedVersion)) mappedVersion.clear();
        if (originalVersion.isEmpty() || !QFileInfo::exists(originalVersion)) originalVersion.clear();
    }
    if (!mappedVersion.isEmpty() && !originalVersion.isEmpty() && mappedVersion != originalVersion) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Update the SNR"));
        box.setText(tr("Where is the SNR of %1 written?").arg(sourceTitle(project.sources[index])));
        box.setInformativeText(tr("The SNR is measured again from the test signals of every field. This capture has a mapped version and the capture it was made from: it is written in the metadata of one of them (the tool keeps a backup of a metadata file; for a track of an LDX file, the new metadata are added to the file)."));
        QPushButton *inMapped = box.addButton(tr("The mapped version"), QMessageBox::AcceptRole);
        QPushButton *inOriginal = box.addButton(tr("The original"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(reference == mappedVersion ? inMapped : inOriginal);
        box.exec();
        if (box.clickedButton() == inMapped) startMapping({ index }, false, false, MapDialog::UpdateSnr, mappedVersion);
        else if (box.clickedButton() == inOriginal) startMapping({ index }, false, false, MapDialog::UpdateSnr, originalVersion);
        return;
    }
    if (QMessageBox::question(this, tr("Update the SNR"),
                              tr("The SNR of %1 is measured again from the test signals of every field, and written in its metadata: %2 (the tool keeps a backup of the metadata file; for a track of an LDX file, the new metadata are added to the file). Continue?").arg(sourceTitle(project.sources[index]), QDir::toNativeSeparators(reference)),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes) {
        return;
    }
    startMapping({ index }, false, false, MapDialog::UpdateSnr);
}

void MainWindow::setFollowMap(const QString &id, bool follow)
{
    states[id].followMap = follow;
    const qint32 index = project.indexOfSource(id);
    if (index >= 0) project.sources[index].followMap = follow;
}

// Why a source needs no mapping: its metadata say it is a mapped copy, or it is read through a map (of its own metadata, or of the session)
QString MainWindow::mappedAlreadyText(const SourceInfo &info)
{
    if (info.throughMap) {
        return info.mapInSource ? tr("This source is read through the map kept in its metadata already")
                                : tr("This source is read through a map of the session already");
    }
    return tr("The metadata say that this source is mapped already");
}

// The Map button of the panel: the source that is chosen, if it has no mapped version; the Follow map box does not matter, the button was pressed
void MainWindow::mapSelectedSource()
{
    QString id = project.selected;
    const qint32 index = project.indexOfSource(id);
    if (index < 0 || project.showStack || mappingBusy) return;
    const SourceState state = states.value(id);
    if (state.info.isMapped) {
        QMessageBox::information(this, tr("Map"), mappedAlreadyText(state.info) + QLatin1Char('.'));
        return;
    }
    const QString reference = project.sources[index].reference();
    const QString mapped = LdxCapture::isLdxName(reference) ? LdxCapture::mappedOfOriginal(reference) : TbcFiles::mappedFileName(project.sources[index].file);
    if (!mapped.isEmpty() && (LdxCapture::isLdxName(reference) || QFileInfo::exists(mapped))) {
        // A mapped version is there: it is the one to use (nothing is made again)
        QMessageBox::information(this, tr("Map"), tr("A mapped version exists beside it (%1): it is the one that is used.").arg(QFileInfo(mapped).fileName()));
        setFollowMap(id, true);
        resolveFollowMap(id);
        rebuildSourceList();
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Map the source"));
    box.setText(tr("%1 has no map.").arg(sourceTitle(project.sources[index])));
    box.setInformativeText(tr("The frames are put in the order of the disc and the missing ones padded. The map is kept in the metadata of the source (its .tbc.db, .tbc.json or LDX file), where every tool that reads the capture finds it; the capture itself is not written again. A backup of a metadata file is kept (name.bup)."));
    QPushButton *saveButton = box.addButton(tr("Map, save in the source"), QMessageBox::AcceptRole);
    QPushButton *sessionButton = box.addButton(tr("Map, in the session only"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(saveButton);
    box.exec();
    if (box.clickedButton() != saveButton && box.clickedButton() != sessionButton) return;
    startMapping({ index }, false, false, MapDialog::Map, QString(), box.clickedButton() == saveButton);
}

// The Follow map boxes can be changed with the stack off only (they change what is stacked)
void MainWindow::updateFollowEnabled()
{
    for (auto it = followBoxes.constBegin(); it != followBoxes.constEnd(); ++it) {
        if (it.value()) it.value()->setEnabled(!project.showStack && !mappingBusy);
    }
}

// The version of the source that is shown and stacked. Follow map on: the source is used as it is if its metadata say it was mapped; if not, the
// mapping of it that exists (the file beside it with _map in its name, or the track of an LDX file that is the mapping of it) takes its place, in its
// name and in what is stacked. Off: the capture it was made from. (A mapping that does not exist yet is made by the Map button.)
void MainWindow::resolveFollowMap(const QString &id)
{
    const qint32 index = project.indexOfSource(id);
    if (index < 0 || !states.contains(id) || !states.value(id).loaded) return;
    const SourceState state = states.value(id);
    const QString reference = project.sources[index].reference();
    const bool ldx = LdxCapture::isLdxName(reference);
    // A source with a map of its own: it is read through the map, or (the box off) as the capture, and loaded again if that is not what it is now
    if (project.sources[index].hasMap || state.info.mapInSource) {
        if (project.sources[index].followMap != state.info.throughMap) switchSource(id, reference);
        return;
    }
    QString target;
    if (state.followMap) {
        if (state.info.isMapped) return;
        if (ldx) {
            target = LdxCapture::mappedOfOriginal(reference);
        } else {
            const QString mapped = TbcFiles::mappedFileName(reference);
            if (!mapped.isEmpty() && QFileInfo::exists(mapped)) target = mapped;
        }
    } else {
        if (ldx) {
            const QString original = LdxCapture::originalOfMapped(reference);
            if (original != reference) target = original;
        } else {
            const QString original = TbcFiles::originalFileName(reference);
            if (original != reference && QFileInfo::exists(original)) target = original;
        }
    }
    if (!target.isEmpty() && target != reference) switchSource(id, target);
}

// The source reads another file (or track) from now on: the same row, with the same offset, loaded again
void MainWindow::switchSource(const QString &id, const QString &reference)
{
    const qint32 index = project.indexOfSource(id);
    if (index < 0) return;
    StackSource &source = project.sources[index];
    QString path;
    qint32 track = 0;
    LdxCapture::split(reference, path, track);
    source.file = QFileInfo(path).absoluteFilePath();
    source.track = track;
    // (an LDX track is named, and the row shows its name)
    if (track > 0) {
        QVector<qint32> tracks;
        QStringList names;
        QString error;
        if (LdxCapture::listCaptures(path, tracks, names, error)) {
            const qint32 at = tracks.indexOf(track);
            if (at >= 0) source.label = names[at];
        }
    }
    // What was measured of the other one goes
    measureQueue.removeAll(id);
    if (measuringId == id) {
        backend->sourceSnrGeneration.store(++sourceSnrJob);
        measuringId.clear();
    }
    SourceState &state = states[id];
    state.loaded = false;
    state.failed = false;
    state.info = SourceInfo();
    state.measure = false;
    state.measuredWhite.clear();
    state.measuredBlack.clear();
    state.measuredDone = 0;
    QMetaObject::invokeMethod(backend, "removeSource", Qt::QueuedConnection, Q_ARG(QString, id));
    startSource(source);
    rebuildSourceList();
    rebuildTabs();
    sourcesChanged();
    statusBar()->showMessage(tr("%1 is read from %2").arg(QString("source %1").arg(index + 1), sourceTitle(source)), 6000);
}

// The sources shown as rows: switch, colour, name, offset
void MainWindow::rebuildSourceList()
{
    followBoxes.clear();
    mapBars.clear();
    const QString selected = sourceList->currentItem() ? sourceList->currentItem()->data(Qt::UserRole).toString() : project.selected;
    const QSignalBlocker blocker(sourceList);
    sourceList->clear();

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        const SourceState state = states.value(source.id);
        const QString id = source.id;

        QWidget *row = new QWidget;

        // A row of a list: a line under it, a bar of the colour of the source on its left, and a faint tint of that
        // colour (a dark transparent one) across it, which the View menu can turn off; the row picked has more of it
        {
            const QColor sourceColor = colorFor(id);
            QColor line = palette().color(QPalette::Text);
            line.setAlpha(90);
            const bool picked = id == selected;
            QString fill = "transparent";
            if (tintRows) {
                QColor tint = sourceColor;
                tint.setAlphaF(source.enabled ? (picked ? 0.40 : 0.20) : 0.07);
                fill = QString("rgba(%1, %2, %3, %4)").arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alpha());
            } else if (picked) {
                QColor mark = palette().color(QPalette::Text);
                mark.setAlpha(40);
                fill = QString("rgba(%1, %2, %3, %4)").arg(mark.red()).arg(mark.green()).arg(mark.blue()).arg(mark.alpha());
            }
            row->setObjectName("sourceRow");
            row->setAttribute(Qt::WA_StyledBackground, true);
            row->setStyleSheet(QString("#sourceRow { border: none; border-left: %1px solid %2; border-bottom: 1px solid rgba(%3, %4, %5, %6); background-color: %7; }")
                               .arg(picked ? 6 : 4).arg(sourceColor.name()).arg(line.red()).arg(line.green()).arg(line.blue()).arg(line.alpha()).arg(fill));
        }
        QVBoxLayout *layout = new QVBoxLayout(row);
        layout->setContentsMargins(6, 4, 6, 4);
        layout->setSpacing(4);

        // First line: in the stack or not, colour, name
        QHBoxLayout *top = new QHBoxLayout;
        top->setSpacing(6);
        QCheckBox *enabled = new QCheckBox(tr("On"));
        enabled->setChecked(source.enabled);
        enabled->setToolTip(tr("In the stack: off leaves the source out without removing it"));
        top->addWidget(enabled);

        QLabel *dot = new QLabel;
        dot->setPixmap(dotIcon(colorFor(id)).pixmap(12, 12));
        top->addWidget(dot);

        QString detail;
        if (state.failed) detail = tr("could not be loaded");
        else if (!state.loaded) detail = tr("loading...");
        else detail = tr("%1 frames - %2").arg(state.info.frames).arg(state.info.system == PAL ? "PAL" : (state.info.system == NTSC ? "NTSC" : "PAL-M")) + (state.info.isMapped ? tr(" - mapped") : QString());
        QLabel *name = new QLabel(QString("%1<br><small>%2</small>").arg(sourceTitle(source).toHtmlEscaped(), detail.toHtmlEscaped()));
        name->setToolTip(source.reference());
        if (state.failed) name->setForegroundRole(QPalette::PlaceholderText);
        name->setEnabled(source.enabled);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        top->addWidget(name, 1);

        QToolButton *snrButton = new QToolButton;
        snrButton->setIcon(QIcon(":/icons/snr.svg"));
        snrButton->setIconSize(QSize(22, 22));
        snrButton->setToolTip(tr("The SNR of this source over the disc, in a window"));
        top->addWidget(snrButton);
        connect(snrButton, &QToolButton::clicked, this, [this, id]() { openSnrWindow(id); });
        QToolButton *dropoutButton = new QToolButton;
        dropoutButton->setIcon(QIcon(":/icons/dropout.svg"));
        dropoutButton->setIconSize(QSize(22, 22));
        dropoutButton->setToolTip(tr("The dropouts of this source over the disc, in a window"));
        top->addWidget(dropoutButton);
        connect(dropoutButton, &QToolButton::clicked, this, [this, id]() { openDropoutWindow(id); });
        layout->addLayout(top);

        // Second line: offset and SNR
        QHBoxLayout *bottom = new QHBoxLayout;
        bottom->setSpacing(6);
        bottom->addSpacing(20);
        bottom->addWidget(new QLabel(tr("Offset:")));
        QSpinBox *offset = new QSpinBox;
        offset->setRange(-1000000, 1000000);
        offset->setValue(source.offset);
        offset->setSuffix(tr(" fr"));
        offset->setToolTip(tr("Offset in frames, added to the alignment by the numbers of the disc (the sources are lined up by them, mapped or not): the source is shifted by this many frames more"));
        offset->setMinimumWidth(90);
        bottom->addWidget(offset);
        QCheckBox *dropoutsBox = new QCheckBox(tr("Show dropouts"));
        dropoutsBox->setChecked(state.showDropouts);
        dropoutsBox->setToolTip(tr("Draws the dropouts of this capture on the picture, in its colour (the first field lighter, the second darker); not for a frame that is marked as a dropout as a whole"));
        bottom->addWidget(dropoutsBox);
        connect(dropoutsBox, &QCheckBox::toggled, this, [this, id](bool on) {
            states[id].showDropouts = on;
            requestFrame();
        });
        bottom->addStretch(1);
        QCheckBox *follow = new QCheckBox(tr("Follow map"));
        follow->setChecked(state.followMap);
        follow->setEnabled(!project.showStack && !mappingBusy);
        followBoxes.insert(id, follow);
        follow->setToolTip(tr("Use the mapped version of the source (the file beside it with _map in its name, or the mapped track of an LDX file) when there is one; off, the capture it was made from"));
        bottom->addWidget(follow);
        connect(follow, &QCheckBox::toggled, this, [this, id](bool on) {
            if (!states.contains(id)) return;
            setFollowMap(id, on);
            resolveFollowMap(id);
            // (where the source is on the timeline depends on it, even when no other file is read)
            sourcesChanged();
        });
        layout->addLayout(bottom);

        // The mapping of the source: a small bar while ld-discmap works on it, red if it failed
        QProgressBar *mapBar = new QProgressBar;
        mapBar->setFixedHeight(14);
        layout->addWidget(mapBar);
        mapBars.insert(id, mapBar);
        mapBar->setVisible(false);

        connect(enabled, &QCheckBox::toggled, this, [this, id, name](bool on) {
            const qint32 at = project.indexOfSource(id);
            if (at < 0) return;
            project.sources[at].enabled = on;
            name->setEnabled(on);
            sourcesChanged();
            QTimer::singleShot(0, this, [this]() { rebuildSourceList(); });
        });
        connect(offset, &QSpinBox::valueChanged, this, [this, id, offset](qint32 value) {
            const qint32 at = project.indexOfSource(id);
            if (at < 0) return;
            project.sources[at].offset = value;
            sourcesChanged();
        });

        QListWidgetItem *item = new QListWidgetItem;
        item->setData(Qt::UserRole, id);
        item->setSizeHint(row->sizeHint() + QSize(0, 2));
        sourceList->addItem(item);
        sourceList->setItemWidget(item, row);
        if (id == selected) sourceList->setCurrentItem(item);
    }
    for (const StackSource &source: project.sources) updateMapBar(source.id);
    fitSourceListWidth();
}

// The side panel is as wide as the rows of the list need (they are cut otherwise: the list has no scroll bar of its own sideways)
void MainWindow::fitSourceListWidth()
{
    if (!mainSplitter || !sourceList) return;
    qint32 needed = 0;
    for (qint32 row = 0; row < sourceList->count(); row++) {
        if (QWidget *widget = sourceList->itemWidget(sourceList->item(row))) needed = qMax(needed, widget->sizeHint().width());
    }
    if (needed == 0) return;
    QWidget *side = sourceList->parentWidget();
    // (the frame of the list, the bar of its sources, and the margins of the panel)
    needed += 2 * sourceList->frameWidth() + sourceList->verticalScrollBar()->sizeHint().width() + 2 * 8 + 4;
    side->setMinimumWidth(needed);
    const qint32 current = side->width();
    if (current >= needed) return;
    const qint32 grow = needed - current;
    QList<qint32> sizes = mainSplitter->sizes();
    if (sizes.size() == 3) {
        sizes[0] += grow;
        const qint32 available = sizes[2] - 360;      // (what the picture can give up before the window grows)
        if (available < grow) {
            sizes[2] = qMax(360, sizes[2] - grow);
            if (isVisible() && !isMaximized() && !isFullScreen()) resize(width() + grow - qMax(0, available), height());
        } else {
            sizes[2] -= grow;
        }
        mainSplitter->setSizes(sizes);
    }
}

// The tabs: the stack, then a tab for each source
void MainWindow::rebuildTabs()
{
    const QSignalBlocker blocker(tabBar);
    while (tabBar->count() > 0) tabBar->removeTab(0);

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        tabBar->addTab(dotIcon(colorFor(source.id)), QString::number(index + 1));
        tabBar->setTabToolTip(index, source.reference());
    }

    // The tab chosen, or the first source if it is gone
    qint32 current = project.indexOfSource(project.selected);
    if (current < 0) {
        current = 0;
        project.selected = project.sources.isEmpty() ? QString() : project.sources.first().id;
    }
    if (!project.sources.isEmpty()) tabBar->setCurrentIndex(current);
    {
        const QSignalBlocker stackBlocker(stackButton);
        stackButton->setChecked(project.showStack);
    }
    updateTabTexts();
}

// The first source on that is loaded is the reference: the stack is as long as it is, and the other sources
// are lined up with it by their offsets (its own offset means nothing)
qint32 MainWindow::referenceIndex() const
{
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        if (source.enabled && states.value(source.id).loaded) return index;
    }
    return -1;
}

// The shift of a source on the timeline: where it begins by the numbers of the disc (and the offset that was set on it, in addition)
qint32 MainWindow::effectiveOffset(qint32 index) const
{
    return index >= 0 && index < alignedOffsets.size() ? alignedOffsets[index] : project.sources[index].offset;
}

// The SNR of a source at a frame: of the white flag, or, when it has none (VHS for instance), of the black level.
// `black` says which one it is.
float MainWindow::sourceSnr(const SourceState &state, qint32 atFrame, qint32 offset, bool &known, bool *black) const
{
    known = false;
    if (black) *black = false;
    if (!state.loaded) return 0;
    const qint32 own = atFrame - offset;
    if (own < 1 || own > state.info.whiteSnr.size()) return 0;
    float value = state.info.whiteSnr[own - 1];
    if (!std::isfinite(value) && own <= state.info.blackSnr.size()) {
        value = state.info.blackSnr[own - 1];
        if (black) *black = true;
    }
    known = std::isfinite(value);
    return known ? value : 0;
}

// Each tab says what the SNR of its picture is at this frame
void MainWindow::updateTabTexts()
{
    if (tabBar->count() != project.sources.size()) return;

    const QSignalBlocker blocker(tabBar);
    // (the black level stands in for the white flag where there is none)
    float stackValue = 0;
    bool stackKnown = false, stackBlackLevel = false;
    if (frame >= 1 && frame <= stackWhite.size()) {
        stackValue = stackWhite[frame - 1];
        if (!std::isfinite(stackValue) && frame <= stackBlack.size()) {
            stackValue = stackBlack[frame - 1];
            stackBlackLevel = true;
        }
        stackKnown = std::isfinite(stackValue);
    }
    stackButton->setText(stackKnown ? tr("Stack  %1 dB").arg(stackValue, 0, 'f', 1) : tr("Stack"));
    stackButton->setToolTip(stackKnown ? (stackBlackLevel ? tr("The SNR of the black level: this video has no white flag") : tr("The SNR of the white flag")) : QString());

    // The source with the best SNR here (compared on the white flag where the sources have one)
    bool anyWhite = false;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        bool known, black;
        sourceSnr(states.value(project.sources[index].id), frame, effectiveOffset(index), known, &black);
        if (known && !black) anyWhite = true;
    }
    qint32 best = -1;
    float bestValue = 0;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known, black;
        const float value = sourceSnr(states.value(source.id), frame, effectiveOffset(index), known, &black);
        if (known && source.enabled && black != anyWhite && (best < 0 || value > bestValue)) { best = index; bestValue = value; }
    }

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known, black;
        const float value = sourceSnr(states.value(source.id), frame, effectiveOffset(index), known, &black);
        tabBar->setTabText(index, known ? tr("%1  %2 dB").arg(index + 1).arg(value, 0, 'f', 1) : QString::number(index + 1));
        tabBar->setTabToolTip(index, known ? (black ? tr("The SNR of the black level: this video has no white flag") : tr("The SNR of the white flag")) : QString());
        tabBar->setTabTextColor(index, index == best ? QColor("#3b6d11") : palette().color(QPalette::WindowText));
    }
}

// How many frames the timeline has: as far as any source on goes
void MainWindow::updateTimeline()
{
    // Each source that is loaded is where the numbers of the disc put it (the earliest frame of any source is the first of the timeline), with its own offset
    // in addition; a source whose VBI gives no number begins at the number 0 of the disc. The timeline is from the first frame of any source that is on to the last of any
    // of them: where a source has no frame, the picture is blank.
    const qint32 count = project.sources.size();
    bool numbered = false;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (state.loaded && source.enabled && state.followMap && state.info.originDisc != DiscNumbering::NoNumber) numbered = true;
    }
    QVector<qint32> position(count, 0);
    qint32 lowest = std::numeric_limits<qint32>::max();
    for (qint32 index = 0; index < count; index++) {
        const StackSource &source = project.sources[index];
        const SourceState state = states.value(source.id);
        if (!state.loaded) continue;
        // (a source that does not follow the map is used as the capture is: its numbers are not trusted, and it begins at the start of the timeline)
        const qint32 shift = state.followMap && state.info.originDisc != DiscNumbering::NoNumber ? state.info.originDisc : 0;
        position[index] = shift + source.offset;
        if (source.enabled) lowest = qMin(lowest, position[index]);
    }
    if (lowest == std::numeric_limits<qint32>::max()) lowest = 0;
    alignedOffsets = QVector<qint32>(count, 0);
    qint32 total = 0;
    for (qint32 index = 0; index < count; index++) {
        const StackSource &source = project.sources[index];
        const SourceState state = states.value(source.id);
        alignedOffsets[index] = state.loaded ? position[index] - lowest : source.offset;
        if (state.loaded && source.enabled) total = qMax(total, alignedOffsets[index] + state.info.frames);
    }
    if (total == 0) {
        // None on: the longest of those that are loaded
        for (qint32 index = 0; index < count; index++) {
            const SourceState state = states.value(project.sources[index].id);
            if (state.loaded) total = qMax(total, alignedOffsets[index] + state.info.frames);
        }
    }
    // The picture stays on the same moment of the disc when the start of the timeline moves (a source that begins earlier, an offset changed)
    const qint32 newDiscAtStart = numbered ? lowest : DiscNumbering::NoNumber;
    if (discAtStart != DiscNumbering::NoNumber && newDiscAtStart != DiscNumbering::NoNumber && newDiscAtStart != discAtStart) frame += discAtStart - newDiscAtStart;
    discAtStart = newDiscAtStart;
    totalFrames = total;

    syncFrameControls();

    const bool enabled = total > 0;
    for (QWidget *widget: { static_cast<QWidget *>(firstButton), static_cast<QWidget *>(previousButton),
                            static_cast<QWidget *>(nextButton), static_cast<QWidget *>(lastButton),
                            static_cast<QWidget *>(frameSpin), static_cast<QWidget *>(frameSlider) }) {
        widget->setEnabled(enabled);
    }
}

// The panel of information shows the source that is chosen (the tab that is shown, or the one that was last)
void MainWindow::updateInfoPanel()
{
    if (!infoPanel || !infoPanel->isVisible()) return;
    QString id = project.selected;
    if (project.indexOfSource(id) < 0 && !project.sources.isEmpty()) id = project.sources.first().id;
    const qint32 index = project.indexOfSource(id);
    if (index < 0) {
        infoPanel->setMessage(tr("Add a source to see what is known of it."));
        return;
    }
    const SourceState state = states.value(id);
    SourceInfo info = state.info;
    if (info.file.isEmpty()) info.file = project.sources[index].reference();
    infoPanel->setSource(info, tr("Source %1 - %2").arg(index + 1).arg(sourceTitle(project.sources[index])), state.loaded);
    {
        const QString file = project.sources[index].file;
        QString why;
        if (!state.loaded) why = tr("The source is not loaded");
        else if (mappingBusy) why = tr("Sources are being mapped");
        else if (project.showStack) why = tr("Turn the stack off to map a source");
        else if (state.info.isMapped) why = mappedAlreadyText(state.info);
        else if (state.info.cvbs) why = tr("ld-discmap maps TBC captures, not CVBS ones");
        else if (LdxCapture::isLdxName(project.sources[index].reference())) {
            if (LdxCapture::isChromaTrack(project.sources[index].reference())) why = tr("ld-discmap maps the luma (or composite) track of a pair, not the chroma one");
        }
        else if (TbcFiles::mappedFileName(file).isEmpty()) why = tr("ld-discmap maps a .tbc file (not the chroma file of a pair)");
        infoPanel->setMappable(why.isEmpty(), why);
        // (the SNR is measured for the same captures: TBC ones, and not the chroma track of a pair)
        QString snrWhy = why;
        if (state.info.isMapped && snrWhy == why) snrWhy.clear();
        if (snrWhy.isEmpty() && LdxCapture::isLdxName(project.sources[index].reference()) && LdxCapture::isChromaTrack(project.sources[index].reference())) snrWhy = tr("The chroma track of a pair has no SNR of its own");
        infoPanel->setSnrUpdatable(snrWhy.isEmpty(), snrWhy);
    }
}

// What is stacked changed (a source on or off, an offset, the mode): the stack is worked out again
void MainWindow::sourcesChanged()
{
    dropoutRangeDirty = true;
    updateInfoPanel();
    snrRangeDirty = true;
    updateTimeline();
    startStackSnr();
    updateTabTexts();
    updatePlot();
    updateStatus();
    requestFrame();
}

// ---------------------------------------------------------------------------------------------
// Frames

QString MainWindow::currentView() const
{
    if (project.showStack || project.sources.isEmpty() || project.indexOfSource(project.selected) < 0) return QStringLiteral("stack");
    return project.selected;
}

qint32 MainWindow::viewOffset() const
{
    const qint32 index = project.indexOfSource(currentView());
    return (index >= 0 && states.value(project.sources[index].id).loaded) ? effectiveOffset(index) : 0;
}

qint32 MainWindow::viewFrames() const
{
    const qint32 index = project.indexOfSource(currentView());
    if (index >= 0 && states.value(project.sources[index].id).loaded) return states.value(project.sources[index].id).info.frames;
    return totalFrames;
}

void MainWindow::syncFrameControls()
{
    const qint32 offset = viewOffset();
    const qint32 count = qMax(1, viewFrames());
    frame = qBound(offset + 1, frame, offset + count);
    const QSignalBlocker spinBlocker(frameSpin);
    const QSignalBlocker sliderBlocker(frameSlider);
    frameSpin->setRange(1, count);
    frameSlider->setRange(1, count);
    frameSpin->setValue(frame - offset);
    frameSlider->setValue(frame - offset);
    frameSpin->setPrefix(tr("Frame "));
    totalLabel->setText(tr("of %1").arg(count));
}

void MainWindow::setFrame(qint32 newFrame)
{
    if (totalFrames < 1) return;
    const qint32 offset = viewOffset();
    newFrame = qBound(offset + 1, newFrame, offset + qMax(1, viewFrames()));
    if (newFrame == frame && frameSpin->value() == frame - offset && frameSlider->value() == frame - offset) return;
    frame = newFrame;

    {
        const QSignalBlocker spinBlocker(frameSpin);
        const QSignalBlocker sliderBlocker(frameSlider);
        frameSpin->setValue(frame - offset);
        frameSlider->setValue(frame - offset);
    }
    snrPlot->setMarker(frame - viewOffset());
    dropoutPlot->setMarker(frame - viewOffset());
    for (auto it = dropoutWindows.constBegin(); it != dropoutWindows.constEnd(); ++it) {
        const qint32 at = project.indexOfSource(it.key());
        if (at >= 0) it.value()->setMarker(frame - effectiveOffset(at));
    }
    updateTabTexts();
    updateStatus();
    requestFrame();
}

// The list shows what the picture is (the session may have set it)
void MainWindow::updateViewCombo()
{
    const QSignalBlocker blocker(viewCombo);
    viewCombo->setCurrentIndex(!fieldView ? 0 : (fieldSplit ? 1 : (fieldIndex ? 3 : 2)));
}

// The view was changed: the picture is made again
void MainWindow::viewModeChanged()
{
    updateStatus();
    requestFrame();
}

void MainWindow::requestFrame()
{
    if (totalFrames < 1) {
        currentImage = QImage();
        imageLabel->setPixmap(QPixmap());
        imageLabel->setText(project.sources.isEmpty() ? tr("Add sources to start") : tr("Loading the sources..."));
        if (stackWindow->isVisible()) stackWindow->setMessage(tr("No sources"));
        return;
    }

    sendRequest(currentView(), 0);
    // The stack in its own window is worked out too, unless it is what the main view shows
    if (stackWindow->isVisible() && currentView() != "stack") sendRequest("stack", 1);
}

void MainWindow::sendRequest(const QString &view, qint32 channel)
{
    FrameRequest request;
    request.serial = ++serials[channel];
    request.channel = channel;
    request.frame = frame;
    request.view = view;
    request.chroma = project.chroma;
    request.viewMode = fieldView ? (fieldSplit ? 2 : 1) : 0;
    request.fieldIndex = fieldIndex;
    request.residualDropouts = showResidualDropouts->isChecked();
    request.residualColor = dropoutResidualColour().rgb();
    request.settings = project.stacking;
    for (const StackSource &source: project.sources) {
        FrameRequest::Entry entry;
        entry.id = source.id;
        entry.enabled = source.enabled;
        entry.offset = effectiveOffset(request.entries.size());
        entry.showDropouts = !hideCaptureDropouts->isChecked() && states.value(source.id).showDropouts;
        entry.color = colorFor(source.id).rgb();
        request.entries.append(entry);
    }
    backend->latest[channel].store(request.serial);
    backend->lastInteraction.store(QDateTime::currentMSecsSinceEpoch());
    QMetaObject::invokeMethod(backend, "render", Qt::QueuedConnection, Q_ARG(FrameRequest, request));
}

void MainWindow::on_frameReady(const FrameResult &result)
{
    const qint32 channel = result.channel & 1;
    if (result.serial != serials[channel]) return;   // a newer one is on its way

    if (channel == 0) {
        lastResult = result;
        if (result.image.isNull()) {
            currentImage = QImage();
            imageLabel->setPixmap(QPixmap());
            imageLabel->setText(result.message);
        } else {
            currentImage = result.image;
            imageLabel->setText(QString());
            applyZoom();
            // The first picture of the first source: at its original size, and the window to match
            if (fitPicture) {
                fitPicture = false;
                setZoom(1.0);
                resizeToImage();
            } else if (fitWindow) {
                fitWindow = false;
                resizeToImage();
            }
        }
    }

    if (result.view != "stack") {
        const qint32 at = project.indexOfSource(result.view);
        if (at >= 0 && states.value(result.view).measure) {
            SourceState &state = states[result.view];
            const qint32 own = result.frame - effectiveOffset(at);
            if (own >= 1 && own <= state.measuredWhite.size() && std::isfinite(result.sourceWhiteSnr)) {
                state.measuredWhite[own - 1] = static_cast<float>(result.sourceWhiteSnr);
                if (own <= state.measuredBlack.size() && std::isfinite(result.sourceBlackSnr)) state.measuredBlack[own - 1] = static_cast<float>(result.sourceBlackSnr);
                snrRangeDirty = true;
                scheduleRefresh();
            }
        }
    }

    if (result.view == "stack") {
        lastStackResult = result;
        if (result.frame >= 1 && result.frame <= stackWhite.size()) {
            if (std::isfinite(result.stackWhiteSnr)) stackWhite[result.frame - 1] = static_cast<float>(result.stackWhiteSnr);
            if (std::isfinite(result.stackBlackSnr)) stackBlack[result.frame - 1] = static_cast<float>(result.stackBlackSnr);
            if (std::isfinite(result.stackResidual) && result.frame <= stackResidual.size()) stackResidual[result.frame - 1] = static_cast<float>(result.stackResidual);
        }
        if (stackWindow->isVisible()) showStackPicture(result);
        scheduleRefresh();
    }
    updateStatus();
}

void MainWindow::showStackPicture(const FrameResult &result)
{
    if (result.image.isNull()) {
        stackWindow->setMessage(result.message);
        return;
    }
    const double stackSnr = std::isfinite(result.stackWhiteSnr) ? result.stackWhiteSnr : result.stackBlackSnr;
    const QString snr = std::isfinite(stackSnr) ? tr("  -  %1 dB").arg(stackSnr, 0, 'f', 1) : QString();
    stackWindow->setPicture(shownImage(result.image), tr("Stack, frame %1%2").arg(result.frame).arg(snr));
}

// The dialogs show what the session holds
void MainWindow::syncDialogs()
{
    optionsDialog->setSettings(project.stacking);
}

void MainWindow::openSnrWindow(const QString &id)
{
    // Each source has a window of its own, made the first time it is asked for
    SnrWindow *window = snrWindows.value(id);
    if (!window) {
        window = new SnrWindow(this);
        window->setWindowFlag(Qt::Window, true);
        connect(window, &SnrWindow::frameRequested, this, [this, id](qint32 requested) {
            const qint32 at = project.indexOfSource(id);
            if (at >= 0) setFrame(requested + effectiveOffset(at));
        });
        connect(window, &SnrWindow::measureToggled, this, [this, id](bool on) { setSourceMeasure(id, on); });
        window->move(pos() + QPoint(60 + 30 * snrWindows.size(), 80 + 30 * snrWindows.size()));
        snrWindows.insert(id, window);
    }
    window->show();
    window->raise();
    updateSnrWindow();   // (they only follow what is stacked once they are visible)
    ensureStackSnr();
}

// The SNR windows follow what is stacked and the frame viewed
void MainWindow::updateSnrWindow()
{
    for (auto it = snrWindows.constBegin(); it != snrWindows.constEnd(); ++it) {
        SnrWindow *window = it.value();
        if (!window->isVisible()) continue;
        const qint32 index = project.indexOfSource(it.key());
        if (index < 0) {
            window->hide();
            continue;
        }
        const StackSource &source = project.sources[index];
        window->setAccent(colorFor(source.id));
        window->setMeasure(states.value(source.id).measure, true);
        window->setBridgeGaps(states.value(source.id).measure && measureMode != 1 && !states.value(source.id).measuredFull);
        updateSnrRange();
        window->setRange(snrSameScale && snrRangeAny, snrLow, snrHigh);
        // (along the frames of the file: the stack behind is the part of its timeline that the file covers)
        window->setContent(tr("SNR - source %1 (%2)").arg(index + 1).arg(sourceTitle(source)), sourceCurve(index, true, true),
                           sourceCurve(index, false, true), ownFrames(stackWhite, index), ownFrames(stackBlack, index), frame - effectiveOffset(index));
    }
}

// What the picture shows changed: the stack or a source
void MainWindow::viewChanged()
{
    syncFrameControls();
    updatePlot();
    updateStatus();
    requestFrame();
}

void MainWindow::updatePlot()
{
    updateDropoutPlot();
    // (the graph of a source is along the frames of that source, the one of the stack along the timeline)
    snrPlot->setMarker(frame - viewOffset());
    applySnrRange(snrPlot);
    updateSnrWindow();

    QString view = currentView();
    qint32 index = project.indexOfSource(view);
    QString title;
    if (view == "stack") {
        qint32 on = 0;
        for (const StackSource &source: project.sources) on += (source.enabled && states.value(source.id).loaded) ? 1 : 0;
        if (on >= 2) {
            // The SNR of a stack is in no metadata: it is measured, whatever the box says
            static const char *const modeNames[] = { "rough: one frame in five seconds, and the frames you seek to", "full", "only the frames you seek to" };
            QString label = tr("Stack, measured from the stacked test signals (%1)").arg(tr(modeNames[qBound(0, measureMode, 2)]));
            if (measureMode == 1 && totalFrames > 0 && stackSnrDone < totalFrames) label += tr(" - %1%").arg(stackSnrDone * 100 / totalFrames);
            snrPlot->setData(label, stackWhite, stackBlack);
            snrPlot->setMeasure(true, false, tr("The SNR of a stack is in no metadata: it is always measured from the stacked test signals; the choice beside says how (only when the stack is shown, and again only when something it depends on has changed)."));
            snrPlot->setMeasureMode(measureMode);
            snrPlot->setBridgeGaps(measureMode != 1);
            return;
        }
        // One source on: the stack is that source
        index = referenceIndex();
        title = tr("Stack of one source");
        if (index < 0) {
            snrPlot->setData(QString(), QVector<float>(), QVector<float>());
            snrPlot->setMeasure(false, false);
            return;
        }
    } else if (index < 0) {
        return;
    } else {
        title = tr("Source %1").arg(index + 1);
    }

    const SourceState state = states.value(project.sources[index].id);
    QString label;
    if (!state.measure) {
        label = tr("%1, from its metadata").arg(title);
    } else {
        label = tr("%1, measured from its fields").arg(title);
        if (measureMode == 1 && !state.measuredFull && state.measuredDone < state.info.frames) label += tr(" - %1%").arg(state.info.frames > 0 ? state.measuredDone * 100 / state.info.frames : 0);
    }
    const bool ownAxis = view != "stack";
    snrPlot->setData(label, sourceCurve(index, true, ownAxis), sourceCurve(index, false, ownAxis));
    snrPlot->setMeasure(state.measure, true);
    snrPlot->setMeasureMode(measureMode);
    snrPlot->setBridgeGaps(state.measure && measureMode != 1 && !state.measuredFull);
}

void MainWindow::resizeHeightBy(qint32 delta)
{
    if (!isVisible() || isMaximized() || isFullScreen() || delta == 0) return;
    const qint32 target = height() + delta;
    QTimer::singleShot(0, this, [this, target]() {
        // (the layout has the minimum of what is shown now: it is worked out again before the window is resized, or the old one would keep the window tall)
        layout()->invalidate();
        layout()->activate();
        resize(width(), qMax(minimumSizeHint().height(), target));
    });
}

// View > Dropout graph, and the box of the SNR graph, which are the same: the graph is under the SNR graph when it is ticked, and gone when it is not
void MainWindow::setDropoutGraphVisible(bool visible)
{
    {
        const QSignalBlocker blocker(dropoutGraphAction);
        dropoutGraphAction->setChecked(visible);
    }
    snrPlot->setDropoutsChecked(visible);
    if (visible != !dropoutPlot->isHidden()) resizeHeightBy(visible ? dropoutPlot->sizeHint().height() : -dropoutPlot->height());
    dropoutPlot->setVisible(visible);
    QSettings().setValue("dropoutGraph", visible);
    // (shown while the stack was measured without its dropouts: it is measured again with them)
    if (visible && !stackMeasuredResidual && stackSnrActive) startStackSnr();
    updateDropoutPlot();
}

// The total length of the dropouts of each frame of a source, along the timeline shifted by its offset, or from its own first frame (the graph of its tab)
QVector<float> MainWindow::dropoutCurve(qint32 index, bool ownAxis) const
{
    const StackSource &source = project.sources[index];
    const SourceState state = states.value(source.id);
    if (!state.loaded) return QVector<float>();
    const QVector<float> &own = state.info.dropoutLength;
    if (ownAxis) return own;
    QVector<float> values(totalFrames, std::nanf(""));
    const qint32 offset = effectiveOffset(index);
    for (qint32 at = 1; at <= totalFrames; at++) {
        const qint32 ownFrame = at - offset;
        if (ownFrame >= 1 && ownFrame <= own.size()) values[at - 1] = own[ownFrame - 1];
    }
    return values;
}

// The graph of the dropouts: on the stack, one curve for each source that is on, in its colour; on a tab, that source. The scale is the same for all of them.
// The scale of the graphs of the dropouts, the same for all of them (the main one and those of the sources): from 0 to the longest dropouts of any frame of any
// source, so that nothing is cut off
void MainWindow::updateDropoutRange()
{
    if (!dropoutRangeDirty) return;
    dropoutRangeDirty = false;
    float largest = 0;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded) continue;
        for (const float value: state.info.dropoutLength) {
            if (!std::isfinite(value)) continue;
            largest = qMax(largest, value);
        }
    }
    dropoutHigh = largest;
}

void MainWindow::openDropoutWindow(const QString &id)
{
    DropoutWindow *window = dropoutWindows.value(id);
    if (!window) {
        window = new DropoutWindow(this);
        window->setWindowFlag(Qt::Window, true);
        connect(window, &DropoutWindow::frameRequested, this, [this, id](qint32 requested) {
            const qint32 at = project.indexOfSource(id);
            if (at >= 0) setFrame(requested + effectiveOffset(at));
        });
        window->move(pos() + QPoint(80 + 30 * dropoutWindows.size(), 100 + 30 * dropoutWindows.size()));
        dropoutWindows.insert(id, window);
    }
    window->show();
    window->raise();
    updateDropoutWindows();
}

// The window of a source: its dropouts along the timeline, and what the stack is left with when that was measured
void MainWindow::updateDropoutWindows()
{
    updateDropoutRange();
    for (auto it = dropoutWindows.constBegin(); it != dropoutWindows.constEnd(); ++it) {
        DropoutWindow *window = it.value();
        if (!window->isVisible()) continue;
        const qint32 index = project.indexOfSource(it.key());
        if (index < 0) {
            window->hide();
            continue;
        }
        const StackSource &source = project.sources[index];
        window->setAccent(colorFor(source.id));
        QVector<SnrCurve> curves;
        curves.append({ dropoutCurve(index, true), colorFor(source.id), false, true, sourceTitle(source) });
        if (stackMeasuredResidual) curves.append({ ownFrames(stackResidual, index), dropoutResidualColour(), true, true, tr("Residual (stack)"), measureMode != 1 });
        window->setRange(true, 0, qMax(1.0, dropoutHigh));
        window->setContent(tr("Dropouts - source %1 (%2)").arg(index + 1).arg(sourceTitle(source)), curves, frame - effectiveOffset(index));
    }
}

// The graph of the dropouts: on the stack, one curve for each source that is on, in its colour; on a tab, that source. The scale is the same for all of them.
void MainWindow::updateDropoutPlot()
{
    updateDropoutWindows();
    if (!dropoutPlot || !dropoutGraphAction || !dropoutGraphAction->isChecked()) return;
    updateDropoutRange();
    dropoutPlot->setMarker(frame - viewOffset());
    const QString view = currentView();
    QVector<SnrCurve> curves;
    if (view == "stack") {
        for (qint32 index = 0; index < project.sources.size(); index++) {
            const StackSource &source = project.sources[index];
            if (!source.enabled || !states.value(source.id).loaded) continue;
            curves.append({ dropoutCurve(index, false), colorFor(source.id), false, true, sourceTitle(source) });
        }
        // What the stack is left with, measured with its SNR
        curves.append({ stackResidual, dropoutResidualColour(), false, true, tr("Residual (stack)"), measureMode != 1 });
    } else {
        const qint32 index = project.indexOfSource(view);
        if (index >= 0 && states.value(view).loaded) curves.append({ dropoutCurve(index, true), colorFor(view), false, true, sourceTitle(project.sources[index]) });
    }
    dropoutPlot->setRange(true, 0, qMax(1.0, dropoutHigh));
    dropoutPlot->setData(tr("Dropouts of the captures: total length per frame, in samples (the frames marked as a dropout as a whole are left out)"), curves);
}

QVector<float> MainWindow::ownFrames(const QVector<float> &timeline, qint32 index) const
{
    const SourceState state = states.value(project.sources[index].id);
    QVector<float> values(state.loaded ? state.info.frames : 0, std::nanf(""));
    const qint32 offset = effectiveOffset(index);
    for (qint32 own = 1; own <= values.size(); own++) {
        const qint32 at = own + offset;
        if (at >= 1 && at <= timeline.size()) values[own - 1] = timeline[at - 1];
    }
    return values;
}

// The SNR of a source along the timeline, shifted by its offset
QVector<float> MainWindow::sourceCurve(qint32 index, bool white, bool ownAxis) const
{
    QVector<float> values(ownAxis ? 0 : totalFrames, std::nanf(""));
    const StackSource &source = project.sources[index];
    const SourceState state = states.value(source.id);
    if (!state.loaded) return values;
    // (measured from the fields if the box says so, else read from the metadata)
    const bool measured = state.measure && !state.measuredWhite.isEmpty();
    const QVector<float> &own = measured ? (white ? state.measuredWhite : state.measuredBlack) : (white ? state.info.whiteSnr : state.info.blackSnr);
    if (ownAxis) return own.mid(0, state.info.frames);
    const qint32 offset = effectiveOffset(index);
    for (qint32 at = 1; at <= totalFrames; at++) {
        const qint32 ownFrame = at - offset;
        if (ownFrame >= 1 && ownFrame <= own.size()) values[at - 1] = own[ownFrame - 1];
    }
    return values;
}

// The range of dB that holds the SNR of every source (as it is shown: from the metadata, or measured) and of the stack, white and black, whether the
// curves are ticked or not: the same scale for every graph
void MainWindow::updateSnrRange()
{
    if (!snrRangeDirty) return;
    snrRangeDirty = false;
    snrRangeAny = false;
    const auto take = [this](const QVector<float> &values) {
        for (const float value: values) {
            if (!std::isfinite(value)) continue;
            if (!snrRangeAny) {
                snrLow = snrHigh = value;
                snrRangeAny = true;
            }
            snrLow = qMin<double>(snrLow, value);
            snrHigh = qMax<double>(snrHigh, value);
        }
    };
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded) continue;
        const bool measured = state.measure && !state.measuredWhite.isEmpty();
        take(measured ? state.measuredWhite : state.info.whiteSnr);
        take(measured ? state.measuredBlack : state.info.blackSnr);
    }
    take(stackWhite);
    take(stackBlack);
}

void MainWindow::applySnrRange(SnrGraph *graph)
{
    updateSnrRange();
    graph->setRange(snrSameScale && snrRangeAny, snrLow, snrHigh);
}

// The source that the box "Measure" of the graph under the picture is for: the one that is shown, or, when the stack is shown and has only one source
// on, that one; none for a stack of several (which is always measured)
QString MainWindow::measureTargetId() const
{
    const QString view = currentView();
    if (view != "stack") return project.indexOfSource(view) >= 0 ? view : QString();
    qint32 on = 0;
    for (const StackSource &source: project.sources) on += (source.enabled && states.value(source.id).loaded) ? 1 : 0;
    if (on >= 2) return QString();
    const qint32 reference = referenceIndex();
    return reference >= 0 ? project.sources[reference].id : QString();
}

// The box "Measure" of a source: its SNR is measured from its fields (in the background, one source at a time) instead of read from its metadata
void MainWindow::setSourceMeasure(const QString &id, bool on)
{
    if (!states.contains(id)) return;
    snrRangeDirty = true;
    SourceState &state = states[id];
    if (state.measure == on) return;
    state.measure = on;
    if (on) {
        // (measured before, from the same file: it is not measured again)
        const auto cached = snrCache.constFind(sourceCacheKey(id));
        if (!state.measuredFull && cached != snrCache.constEnd() && cached->white.size() == state.info.frames) {
            state.measuredWhite = cached->white;
            state.measuredBlack = cached->black;
            state.measuredDone = state.info.frames;
            state.measuredFull = true;
        }
        // (what was measured stays; the frames not measured are empty. Rough and full measure by themselves, seek only the frames that are gone to)
        if (state.measuredWhite.size() != state.info.frames) {
            state.measuredWhite = QVector<float>(state.info.frames, std::nanf(""));
            state.measuredBlack = QVector<float>(state.info.frames, std::nanf(""));
            state.measuredDone = 0;
            state.measuredFull = false;
        }
        if (!state.measuredFull && measureMode != 2 && measuringId != id && !measureQueue.contains(id)) measureQueue.append(id);
        startNextMeasure();
    } else {
        measureQueue.removeAll(id);
        if (measuringId == id) {
            backend->sourceSnrGeneration.store(++sourceSnrJob);    // (the one that was going stops)
            measuringId.clear();
            state.measuredDone = 0;
            state.measuredFull = false;
            state.measuredWhite.clear();
            state.measuredBlack.clear();
            startNextMeasure();
        }
    }
    updatePlot();
}

void MainWindow::startNextMeasure()
{
    if (!measuringId.isEmpty()) return;
    while (!measureQueue.isEmpty()) {
        const QString id = measureQueue.takeFirst();
        if (!states.contains(id) || !states.value(id).loaded || !states.value(id).measure || measureMode == 2) continue;
        measuringId = id;
        const quint64 job = ++sourceSnrJob;
        backend->sourceSnrGeneration.store(job);
        // Rough: one frame in five seconds of video; full: all of them
        qint32 framesPerSecond = timecodeFramesPerSecond(states.value(id).info.system);
        sourceJobFull = measureMode == 1;
        QMetaObject::invokeMethod(backend, "computeSourceSnr", Qt::QueuedConnection, Q_ARG(QString, id), Q_ARG(qint32, 1), Q_ARG(quint64, job),
                                  Q_ARG(qint32, measureMode == 0 ? 5 * framesPerSecond : 1));
        return;
    }
}

void MainWindow::on_sourceSnrReady(const QString &id, quint64 generation, const QVector<StackSnrPoint> &points, bool finished)
{
    if (generation != sourceSnrJob || id != measuringId || !states.contains(id)) return;   // of an earlier job
    snrRangeDirty = true;
    SourceState &state = states[id];
    for (const StackSnrPoint &point: points) {
        if (point.frame < 1 || point.frame > state.measuredWhite.size()) continue;
        state.measuredWhite[point.frame - 1] = point.white;
        state.measuredBlack[point.frame - 1] = point.black;
        state.measuredDone = qMax(state.measuredDone, point.frame);
    }
    if (finished) {
        // (only a measure of every frame is kept: a rough one has gaps)
        if (sourceJobFull) {
            state.measuredDone = state.info.frames;
            state.measuredFull = true;
            storeInCache(sourceCacheKey(id), state.measuredWhite, state.measuredBlack);
        }
        measuringId.clear();
        startNextMeasure();
    }
    scheduleRefresh();
}

QString MainWindow::sourceCacheKey(const QString &id) const
{
    const qint32 index = project.indexOfSource(id);
    return index < 0 ? QString() : project.sources[index].measureKey();
}

// The stack: the sources that are on, each with its file and where it begins on the timeline, and the options that change what is stacked
QString MainWindow::stackCacheKey(bool withResidual) const
{
    return project.stackMeasureKey(withResidual);
}

void MainWindow::storeInCache(const QString &key, const QVector<float> &white, const QVector<float> &black, const QVector<float> &residual)
{
    if (key.isEmpty()) return;
    // (a few measures at most: the oldest are let go of when there are many)
    if (snrCache.size() > 24) snrCache.clear();
    snrCache.insert(key, { white, black, residual });
}

// What is stacked changed: what was measured of the stack is forgotten and the measure in progress stops. The measure of the stack reads the test signals of every
// field of every source, which is a lot of reading from the disk: it begins a moment after the last change (an offset that is being turned, sources that are
// being added), and only when the stack is shown (or the SNR window of a source, which has the stack behind its curve). What was measured is kept: the same files, offsets
// and options are not measured again
void MainWindow::startStackSnr()
{
    snrRangeDirty = true;
    stackWhite = QVector<float>(totalFrames, std::nanf(""));
    stackBlack = QVector<float>(totalFrames, std::nanf(""));
    stackResidual = QVector<float>(totalFrames, std::nanf(""));
    stackMeasuredResidual = false;
    stackSnrDone = 0;
    stackSnrActive = false;
    backend->snrGeneration.store(++snrJob);
    if (!stackSnrTimer) {
        stackSnrTimer = new QTimer(this);
        stackSnrTimer->setSingleShot(true);
        connect(stackSnrTimer, &QTimer::timeout, this, &MainWindow::ensureStackSnr);
    }
    stackSnrTimer->start(800);
}

void MainWindow::ensureStackSnr()
{
    if (stackSnrActive || totalFrames < 1 || referenceIndex() < 0) return;
    bool wanted = project.showStack;
    for (auto it = snrWindows.constBegin(); it != snrWindows.constEnd(); ++it) wanted = wanted || it.value()->isVisible();
    if (!wanted) return;
    stackSnrActive = true;

    // With the dropout graph shown the measure reads whole fields and measures the dropouts the stack is left with, as well as the SNR
    const bool withResidual = dropoutGraphAction && dropoutGraphAction->isChecked();
    stackMeasuredResidual = withResidual;

    // Measured before, from the same files, offsets and options: nothing to do (a measure with the dropouts has the SNR too)
    auto cached = snrCache.constFind(stackCacheKey(withResidual));
    if (cached == snrCache.constEnd() && !withResidual) cached = snrCache.constFind(stackCacheKey(true));
    if (cached != snrCache.constEnd() && cached->white.size() == totalFrames) {
        stackWhite = cached->white;
        stackBlack = cached->black;
        if (cached->residual.size() == totalFrames) {
            stackResidual = cached->residual;
            stackMeasuredResidual = true;
        }
        stackSnrDone = totalFrames;
        snrRangeDirty = true;
        updateTabTexts();
        updatePlot();
        updateSnrWindow();
        return;
    }

    // Seek: nothing is measured on its own, the frames that are seeked to give theirs (the picture of the stack measures its own frame)
    if (measureMode == 2) return;

    FrameRequest request;
    request.settings = project.stacking;
    request.measureResidual = withResidual;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        FrameRequest::Entry entry;
        entry.id = project.sources[index].id;
        entry.enabled = project.sources[index].enabled;
        entry.offset = effectiveOffset(index);
        request.entries.append(entry);
    }
    stackJobFull = measureMode == 1;
    // Rough: one frame in five seconds of video; full: all of them
    qint32 framesPerSecond = 30;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (state.loaded) {
            framesPerSecond = timecodeFramesPerSecond(state.info.system);
            break;
        }
    }
    request.sampleStep = measureMode == 0 ? 5 * framesPerSecond : 1;
    QMetaObject::invokeMethod(backend, "computeStackSnr", Qt::QueuedConnection, Q_ARG(FrameRequest, request),
                              Q_ARG(qint32, 1), Q_ARG(quint64, snrJob));
}

// The plots are redrawn at most every so often: measures come in by chunks, and pictures by the frame, and the plots are of every frame of the disc
void MainWindow::scheduleRefresh()
{
    if (!plotRefreshTimer) {
        plotRefreshTimer = new QTimer(this);
        plotRefreshTimer->setSingleShot(true);
        connect(plotRefreshTimer, &QTimer::timeout, this, [this]() {
            updateTabTexts();
            updatePlot();
        });
    }
    if (!plotRefreshTimer->isActive()) plotRefreshTimer->start(400);
}

void MainWindow::on_stackSnrReady(quint64 generation, const QVector<StackSnrPoint> &points, bool finished)
{
    if (generation != snrJob) return;   // of an earlier job
    snrRangeDirty = true;
    for (const StackSnrPoint &point: points) {
        if (point.frame < 1 || point.frame > stackWhite.size()) continue;
        stackWhite[point.frame - 1] = point.white;
        stackBlack[point.frame - 1] = point.black;
        if (point.frame <= stackResidual.size()) stackResidual[point.frame - 1] = point.residual;
        stackSnrDone = qMax(stackSnrDone, point.frame);
    }
    if (finished) {
        stackSnrDone = totalFrames;
        // (only a measure of every frame is kept: a rough one has gaps, which are not the stack's)
        if (stackJobFull) storeInCache(stackCacheKey(stackMeasuredResidual), stackWhite, stackBlack, stackMeasuredResidual ? stackResidual : QVector<float>());
    }
    scheduleRefresh();
}

// A number of the disc as it is read: the picture of a CAV disc, or the time of a CLV one (hours:minutes:seconds, and the picture of the second)
QString MainWindow::discFormat(qint32 disc) const
{
    // (before the first number of the disc: the frames of a lead-in)
    if (disc < 0) return tr("before the first number");
    qint32 type = 0;
    VideoSystem system = NTSC;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded || state.info.discType == 0) continue;
        type = state.info.discType;
        system = state.info.system;
        break;
    }
    if (type == 1) return DiscNumbering::clvText(disc, system);
    return type == 2 ? tr("picture %1").arg(DiscNumbering::cavText(disc)) : tr("frame %1").arg(disc);
}

void MainWindow::updateDiscLabel()
{
    if (!discLabel) return;
    if (totalFrames < 1) {
        discLabel->clear();
        return;
    }
    QString text;
    const QString view = currentView();
    const qint32 index = project.indexOfSource(view);
    if (view != "stack" && index >= 0) {
        // The source that is shown: where it is in its file, and on the disc
        const SourceState state = states.value(view);
        const qint32 own = frame - effectiveOffset(index);
        if (!state.loaded) text = QString();
        else if (own < 1 || own > state.info.frames) text = tr("no frame of this source here");
        else {
            text = tr("file frame %1").arg(own);
            if (state.followMap && state.info.originDisc != DiscNumbering::NoNumber) text += QStringLiteral(" - ") + discFormat(state.info.originDisc + own - 1);
        }
    } else if (discAtStart != DiscNumbering::NoNumber) {
        text = discFormat(discAtStart + frame - 1);
    }
    discLabel->setText(text);
}

// What the process stacks: the offsets are those of the timeline (the alignment by the disc and the offsets set), relative to the first source that is on, which is
// the reference of ld-disc-stacker
StackProject MainWindow::projectForProcessing() const
{
    StackProject copy = project;
    const qint32 reference = referenceIndex();
    if (reference < 0) return copy;
    const qint32 base = effectiveOffset(reference);
    for (qint32 index = 0; index < copy.sources.size(); index++) {
        if (states.value(copy.sources[index].id).loaded) copy.sources[index].offset = effectiveOffset(index) - base;
    }
    return copy;
}

void MainWindow::updateStatus()
{
    updateDiscLabel();
    // What the sources are: the video system, and TBC or CVBS
    QString format;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded) continue;
        format = tr("%1 %2").arg(state.info.system == PAL ? "PAL" : (state.info.system == NTSC ? "NTSC" : "PAL-M"), state.info.cvbs ? tr("CVBS") : tr("TBC"));
        break;
    }
    statusFormat->setText(format);

    qint32 on = 0, loaded = 0;
    for (const StackSource &source: project.sources) {
        if (states.value(source.id).loaded) loaded++;
        if (source.enabled && states.value(source.id).loaded) on++;
    }

    if (project.sources.isEmpty()) {
        modeCombo->setItemText(0, tr("Auto"));
        statusSources->setText(tr("No sources"));
        statusMode->clear();
        statusShowing->clear();
        return;
    }

    const bool isStack = currentView() == "stack";
    if (lastStackResult.view == "stack" && lastStackResult.frame == frame && lastStackResult.contributing > 0) {
        statusSources->setText(tr("%1 of %2 sources contribute to this frame").arg(lastStackResult.contributing).arg(project.sources.size()));
    } else {
        statusSources->setText(tr("%1 of %2 sources on").arg(on).arg(project.sources.size()));
    }

    qint32 mode = project.stacking.mode;
    QString modeText = tr(MODE_NAMES[qBound(0, mode + 1, 5)]);
    // (auto goes by how many sources there are: the list says which mode that is now)
    QString autoText = tr("Auto");
    if (on > 0) {
        const qint32 used = StackerCore::autoMode(on);
        autoText = tr("Auto (%1)").arg(tr(MODE_NAMES[qBound(0, used + 1, 5)]));
        if (mode == -1) modeText = tr("Auto: %1").arg(tr(MODE_NAMES[qBound(0, used + 1, 5)]));
    }
    modeCombo->setItemText(0, autoText);
    statusMode->setText(tr("Mode: %1").arg(modeText));

    const qint32 viewIndex = project.indexOfSource(currentView());
    const QString where = fieldView && !fieldSplit ? tr("frame %1, field %2").arg(frame).arg(fieldIndex + 1) : tr("frame %1").arg(frame);
    statusShowing->setText(tr("Showing: %1 - %2").arg(isStack ? tr("stack") : tr("source %1").arg(viewIndex + 1)).arg(where));
}

// ---------------------------------------------------------------------------------------------
// Zoom and pan

void MainWindow::setZoom(double newZoom)
{
    zoom = qBound(0.25, newZoom, 6.0);
    {
        const QSignalBlocker blocker(zoomSpin);
        zoomSpin->setValue(qRound(zoom * 100));
    }
    applyZoom();
    if (scaleWindowWithZoom) resizeToImage();
}

// One step up or down the usual zooms, which are the ones that go through 100%
void MainWindow::stepZoom(qint32 direction)
{
    static const double steps[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0 };
    constexpr qint32 count = sizeof(steps) / sizeof(steps[0]);
    if (direction > 0) {
        for (qint32 i = 0; i < count; i++) {
            if (steps[i] > zoom + 0.001) { setZoom(steps[i]); return; }
        }
    } else {
        for (qint32 i = count - 1; i >= 0; i--) {
            if (steps[i] < zoom - 0.001) { setZoom(steps[i]); return; }
        }
    }
}

void MainWindow::applyZoom()
{
    if (currentImage.isNull()) return;
    const QSize size(qRound((currentImage.width() + aspectAdjustment()) * zoom), qRound(currentImage.height() * zoom));
    const Qt::TransformationMode mode = zoomMethod == 0 ? Qt::FastTransformation : Qt::SmoothTransformation;
    imageLabel->setPixmap(QPixmap::fromImage(enhanceDialog->apply(currentImage)).scaled(size, Qt::IgnoreAspectRatio, mode));
    imageLabel->setMinimumSize(size);
}

// Dragging the picture pans it; Ctrl and the wheel zoom
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == scrollArea->viewport()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                panning = true;
                panStart = mouse->globalPosition().toPoint();
                panScroll = QPoint(scrollArea->horizontalScrollBar()->value(), scrollArea->verticalScrollBar()->value());
                scrollArea->viewport()->setCursor(Qt::ClosedHandCursor);
            }
            break;
        }
        case QEvent::MouseMove:
            if (panning) {
                const QPoint delta = static_cast<QMouseEvent *>(event)->globalPosition().toPoint() - panStart;
                scrollArea->horizontalScrollBar()->setValue(panScroll.x() - delta.x());
                scrollArea->verticalScrollBar()->setValue(panScroll.y() - delta.y());
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            panning = false;
            scrollArea->viewport()->setCursor(Qt::OpenHandCursor);
            break;
        case QEvent::Wheel: {
            QWheelEvent *wheel = static_cast<QWheelEvent *>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                stepZoom(wheel->angleDelta().y() > 0 ? 1 : -1);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// Files dropped on the window: a session, or sources to add. Only what a stack can be made of is taken (a TBC, CVBS or LDX capture, not the
// chroma file of a pair, which goes with its luma); the other files are left, and the status bar says which
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl &url: event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty() && (StackProject::isSession(path) || TbcFiles::isStackSourceName(path))) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList files, ignored;
    QString session;
    for (const QUrl &url: event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (path.isEmpty()) continue;
        if (StackProject::isSession(path)) session = path;
        else if (TbcFiles::isStackSourceName(path)) files.append(path);
        else ignored.append(QFileInfo(path).fileName());
    }
    if (files.isEmpty() && session.isEmpty()) return;
    event->acceptProposedAction();
    if (!session.isEmpty()) loadSession(session);
    if (!files.isEmpty()) addSources(files);
    if (!ignored.isEmpty()) statusBar()->showMessage(tr("Not added (not a TBC, CVBS or LDX capture): %1").arg(ignored.join(", ")), 10000);
}

// Resize the window so that the scroll area shows the whole picture, without scroll bars. What is around the
// picture (margins, buttons, menus, status bar) depends on the style and fonts, so it is measured: the window
// grows or shrinks by the difference between the size the picture wants and the one the scroll area has.
void MainWindow::resizeToImage(qint32 passesLeft)
{
    if (isFullScreen() || isMaximized() || currentImage.isNull()) return;
    if (layout()) layout()->activate();

    const qint32 frame = 2 * scrollArea->frameWidth();
    const QSize wanted = imageLabel->minimumSize() + QSize(frame, frame);
    const QSize difference = wanted - scrollArea->size();
    if (difference.isNull()) return;
    resize(size() + difference);

    // The resize takes effect, and the layout is redone, in the event loop: look again then
    if (passesLeft > 0) QTimer::singleShot(0, this, [this, passesLeft]() { resizeToImage(passesLeft - 1); });
}

// View > Theme and View > Style, kept in the settings; what keeps colours of its own is redrawn when they change
void MainWindow::setupAppearanceMenus(QMenu *menu)
{
    connect(appearance, &AppearanceMenus::themeModeStored, this, [](qint32 mode) { QSettings().setValue("themeMode", mode); });
    connect(appearance, &AppearanceMenus::customPaletteStored, this, [](const QString &palette) { QSettings().setValue("customPalette", palette); });
    connect(appearance, &AppearanceMenus::styleNameStored, this, [](const QString &name) { QSettings().setValue("styleName", name); });
    connect(appearance, &AppearanceMenus::changed, this, [this]() {
        rebuildSourceList();
        for (QWidget *widget: QApplication::allWidgets()) widget->update();
        applyZoom();
        requestFrame();
    });
    connect(appearance, &AppearanceMenus::customizeRequested, this, [this](QWidget *parent) { appearance->customize(parent, QApplication::palette()); });
    appearance->install(menu);
}

// How much wider (or narrower) than its samples a picture is shown, as ld-analyse does: the 4:3 or 16:9 of the
// video system and the first source that is loaded, or none with square pixels
qint32 MainWindow::aspectAdjustment() const
{
    if (!displayAspect) return 0;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded) continue;
        return AspectRatio::widthAdjustment(state.info.system, state.info.widescreen);
    }
    return 0;
}

void MainWindow::updateAspectButton()
{
    QString text = tr("SAR 1:1");
    if (displayAspect) {
        bool widescreen = false;
        for (const StackSource &source: project.sources) {
            const SourceState state = states.value(source.id);
            if (state.loaded) { widescreen = state.info.widescreen; break; }
        }
        text = widescreen ? tr("DAR 16:9") : tr("DAR 4:3");
    }
    aspectButton->setText(text);
    aspectButton->setToolTip(tr("Aspect ratio of the picture: the display aspect ratio of the video, or square pixels (SAR 1:1)"));
}

// A picture as it is shown: at the display aspect ratio, for the windows that scale it themselves
QImage MainWindow::shownImage(const QImage &image) const
{
    const qint32 adjustment = aspectAdjustment();
    QImage shown = enhanceDialog->apply(image);
    if (adjustment == 0 || shown.isNull()) return shown;
    return shown.scaled(shown.width() + adjustment, shown.height(), Qt::IgnoreAspectRatio,
                        zoomMethod == 0 ? Qt::FastTransformation : Qt::SmoothTransformation);
}
