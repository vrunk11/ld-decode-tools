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

    // Colour theme and widget style, as in ld-analyse: View > Theme and View > Style, kept in the settings.
    // --force-dark-theme overrides the theme for this session without changing the setting.
    {
        QSettings settings;
        customPalette = settings.value("customPalette").toString();
        const qint32 savedTheme = settings.value("themeMode", static_cast<qint32>(ThemeMode::Auto)).toInt();
        themeMode = (savedTheme >= static_cast<qint32>(ThemeMode::Auto) && savedTheme <= static_cast<qint32>(ThemeMode::DarkGrey))
            ? static_cast<ThemeMode>(savedTheme) : ThemeMode::Auto;
        if (themeMode == ThemeMode::Custom && customPalette.isEmpty()) themeMode = ThemeMode::Auto;
        if (qApp->property("forceDarkTheme").toBool()) themeMode = ThemeMode::Dark;

        // A saved style that is not installed here falls back to Auto
        styleName = settings.value("styleName").toString();
        if (!styleName.isEmpty() && !availableStyles().contains(styleName, Qt::CaseInsensitive)) styleName.clear();
        isDarkTheme = applyAppearance(styleName, themeMode, customPalette);
    }

    backend = new FrameBackend;
    backend->moveToThread(&backendThread);
    connect(&backendThread, &QThread::finished, backend, &QObject::deleteLater);
    connect(backend, &FrameBackend::sourceLoaded, this, &MainWindow::on_sourceLoaded);
    connect(backend, &FrameBackend::frameReady, this, &MainWindow::on_frameReady);
    connect(backend, &FrameBackend::stackSnrReady, this, &MainWindow::on_stackSnrReady);
    backendThread.start();

    buildUi();
    buildMenus();

    snrWindow = new SnrWindow(this);
    snrWindow->setWindowFlag(Qt::Window, true);
    connect(snrWindow, &SnrWindow::frameRequested, this, &MainWindow::setFrame);
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

    // A session file, or the sources to start with
    QStringList files;
    QString session;
    for (const QString &argument: arguments) {
        if (argument.endsWith(".stack", Qt::CaseInsensitive)) session = argument; else files.append(argument);
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
    sourceList->setSpacing(2);
    sourceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    side->setMinimumWidth(300);
    sideLayout->addWidget(sourceList, 1);
    QPushButton *addButton = new QPushButton(tr("Add source..."));
    sideLayout->addWidget(addButton);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addSourcesDialog);

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
    snrPlot->setFixedHeight(100);
    mainLayout->addWidget(snrPlot);

    // Navigation
    QWidget *navigation = new QWidget;
    QHBoxLayout *navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(8, 6, 8, 2);
    const auto makeButton = [this](const QString &text, const QString &tip, bool repeat) {
        QToolButton *button = new QToolButton;
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRepeat(repeat);
        button->setAutoRepeatDelay(500);
        button->setAutoRepeatInterval(30);
        return button;
    };
    firstButton = makeButton("|<", tr("First frame"), false);
    previousButton = makeButton("<", tr("Previous frame"), true);
    nextButton = makeButton(">", tr("Next frame"), true);
    lastButton = makeButton(">|", tr("Last frame"), false);
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
    mainLayout->addWidget(navigation);

    // Options
    QWidget *options = new QWidget;
    QHBoxLayout *optionsLayout = new QHBoxLayout(options);
    optionsLayout->setContentsMargins(8, 2, 8, 6);
    modeCombo = new QComboBox;
    for (const char *name: MODE_NAMES) modeCombo->addItem(tr(name));
    modeCombo->setToolTip(tr("Stacking mode"));
    chromaButton = new QPushButton(tr("Chroma"));
    chromaButton->setCheckable(true);
    chromaButton->setToolTip(tr("Decode the pictures to colour (with a 2D decoder)"));
    optionsLayout->addWidget(modeCombo);
    // The stack is a button, on or off: on, the picture is the stack, off it is the source of the tab chosen,
    // so that the two are compared by switching it
    stackButton = new QPushButton(tr("Stack"));
    stackButton->setIcon(dotIcon(STACK_COLOR));
    stackButton->setCheckable(true);
    stackButton->setChecked(true);
    stackButton->setMinimumWidth(120);
    // Clearly on or off, whatever the theme
    const QString toggleStyle = "QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); }";
    stackButton->setStyleSheet(toggleStyle);
    chromaButton->setStyleSheet(toggleStyle);
    stackButton->setToolTip(tr("On: the picture is the stack. Off: it is the source of the tab chosen. (key 0 or S)"));
    optionsLayout->addWidget(stackButton);
    optionsLayout->addWidget(chromaButton);
    chromaSettingsButton = new QPushButton(tr("Chroma settings..."));
    chromaSettingsButton->setToolTip(tr("The configuration of the chroma decoder"));
    optionsLayout->addWidget(chromaSettingsButton);
    enhanceButton = new QPushButton(tr("Enhance..."));
    enhanceButton->setToolTip(tr("Brightness, contrast and gamma of the pictures shown, to bring out the defects"));
    optionsLayout->addWidget(enhanceButton);
    optionsLayout->addStretch(1);
    QToolButton *zoomOut = makeButton("-", tr("Zoom out"), true);
    QToolButton *zoomIn = makeButton("+", tr("Zoom in"), true);
    zoomSpin = new QSpinBox;
    zoomSpin->setRange(25, 600);
    zoomSpin->setSingleStep(25);
    zoomSpin->setSuffix("%");
    zoomSpin->setKeyboardTracking(false);
    zoomSpin->setToolTip(tr("Zoom: type a value, or use the + and - buttons"));
    zoomMethodCombo = new QComboBox;
    zoomMethodCombo->addItem(tr("Line doubling"));
    zoomMethodCombo->addItem(tr("Smooth"));
    zoomMethodCombo->setToolTip(tr("How the picture is enlarged: line doubling keeps the pixels as they are"));
    optionsLayout->addWidget(zoomOut);
    optionsLayout->addWidget(zoomSpin);
    optionsLayout->addWidget(zoomIn);
    optionsLayout->addWidget(zoomMethodCombo);
    originalSizeButton = new QPushButton(tr("Original size"));
    originalSizeButton->setToolTip(tr("Zoom to 100% and resize the window to show the whole picture"));
    optionsLayout->addWidget(originalSizeButton);
    mainLayout->addWidget(options);

    QSplitter *splitter = new QSplitter;
    splitter->addWidget(side);
    splitter->addWidget(main);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 340, 880 });
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    // The status bar: what contributes, and how
    statusSources = new QLabel;
    statusMode = new QLabel;
    statusShowing = new QLabel;
    statusBar()->addWidget(statusSources);
    statusBar()->addWidget(statusMode);
    statusBar()->addPermanentWidget(statusShowing);

    // Wiring
    connect(tabBar, &QTabBar::currentChanged, this, [this](qint32 index) {
        if (index < 0 || index >= project.sources.size()) return;
        // Choosing a source shows it: the stack is off
        project.selected = project.sources[index].id;
        project.showStack = false;
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
        project.showStack = on;
        viewChanged();
    });
    connect(frameSpin, &QSpinBox::valueChanged, this, &MainWindow::setFrame);
    connect(frameSlider, &QSlider::valueChanged, this, &MainWindow::setFrame);
    connect(firstButton, &QToolButton::clicked, this, [this]() { setFrame(1); });
    connect(previousButton, &QToolButton::clicked, this, [this]() { setFrame(frame - 1); });
    connect(nextButton, &QToolButton::clicked, this, [this]() { setFrame(frame + 1); });
    connect(lastButton, &QToolButton::clicked, this, [this]() { setFrame(totalFrames); });
    connect(snrPlot, &SnrGraph::frameRequested, this, &MainWindow::setFrame);
    connect(modeCombo, &QComboBox::currentIndexChanged, this, [this](qint32 index) {
        project.stacking.mode = index - 1;
        sourcesChanged();
    });
    connect(chromaSettingsButton, &QPushButton::clicked, this, [this]() { chromaDialog->show(); chromaDialog->raise(); });
    connect(enhanceButton, &QPushButton::clicked, this, [this]() { enhanceDialog->show(); enhanceDialog->raise(); });
    connect(chromaButton, &QPushButton::toggled, this, [this](bool on) {
        project.chroma = on;
        requestFrame();
    });
    connect(zoomIn, &QToolButton::clicked, this, [this]() { stepZoom(1); });
    connect(zoomOut, &QToolButton::clicked, this, [this]() { stepZoom(-1); });
    connect(originalSizeButton, &QPushButton::clicked, this, [this]() {
        setZoom(1.0);
        resizeToImage();
    });
    connect(zoomSpin, &QSpinBox::valueChanged, this, [this](qint32 percent) { setZoom(percent / 100.0); });
    connect(zoomMethodCombo, &QComboBox::currentIndexChanged, this, [this]() { applyZoom(); });

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
    file->addAction(tr("&Open session..."), QKeySequence::Open, this, &MainWindow::openSession);
    file->addAction(tr("&Add source..."), this, &MainWindow::addSourcesDialog);
    file->addSeparator();
    file->addAction(tr("&Export session..."), QKeySequence::Save, this, &MainWindow::exportSession);
    file->addAction(tr("&Process..."), this, &MainWindow::processSession);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Stacking options..."), this, [this]() { optionsDialog->show(); optionsDialog->raise(); });
    edit->addAction(tr("&Chroma decoder..."), this, [this]() { chromaDialog->show(); chromaDialog->raise(); });
    edit->addAction(tr("&Enhance..."), this, [this]() { enhanceDialog->show(); enhanceDialog->raise(); });

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
    return source.label.isEmpty() ? QFileInfo(source.file).fileName() : source.label;
}

void MainWindow::addSourcesDialog()
{
    QSettings settings;
    const QString directory = settings.value("sourceDirectory").toString();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add sources"), directory,
                                                            tr("TBC files (*.tbc);;All files (*)"));
    if (files.isEmpty()) return;
    settings.setValue("sourceDirectory", QFileInfo(files.first()).absolutePath());
    addSources(files);
}

void MainWindow::addSources(const QStringList &files)
{
    for (const QString &file: files) {
        StackSource source;
        source.file = QFileInfo(file).absoluteFilePath();
        source.id = project.newSourceId();
        project.sources.append(source);
        startSource(source);
    }
    rebuildSourceList();
    rebuildTabs();
}

void MainWindow::startSource(const StackSource &source)
{
    colorFor(source.id);
    states[source.id].loaded = false;
    states[source.id].failed = false;
    QMetaObject::invokeMethod(backend, "addSource", Qt::QueuedConnection, Q_ARG(QString, source.id),
                              Q_ARG(QString, source.file), Q_ARG(bool, project.stacking.reverseFields));
}

void MainWindow::removeSource(const QString &id)
{
    const qint32 index = project.indexOfSource(id);
    if (index < 0) return;
    project.sources.remove(index);
    states.remove(id);
    QMetaObject::invokeMethod(backend, "removeSource", Qt::QueuedConnection, Q_ARG(QString, id));
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

    // What was loaded is let go of
    for (const StackSource &source: project.sources) {
        QMetaObject::invokeMethod(backend, "removeSource", Qt::QueuedConnection, Q_ARG(QString, source.id));
    }
    states.clear();
    colorCounter = 0;
    stackWhite.clear();
    stackBlack.clear();
    stackSnrDone = 0;
    currentImage = QImage();

    project = loaded;
    sessionFile = fileName;
    for (const StackSource &source: project.sources) startSource(source);

    {
        const QSignalBlocker blocker(modeCombo);
        modeCombo->setCurrentIndex(qBound(0, project.stacking.mode + 1, modeCombo->count() - 1));
    }
    setZoom(project.zoom);
    syncDialogs();
    {
        const QSignalBlocker blocker(chromaButton);
        chromaButton->setChecked(project.chroma);
    }
    setWindowTitle(tr("ld-stacker-gui - %1").arg(QFileInfo(fileName).fileName()));

    rebuildSourceList();
    rebuildTabs();
    sourcesChanged();
}

void MainWindow::openSession()
{
    QSettings settings;
    const QString fileName = QFileDialog::getOpenFileName(this, tr("Open session"), settings.value("sessionDirectory").toString(),
                                                          tr("Stack sessions (*.stack);;All files (*)"));
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
    ProcessDialog dialog(project, suggested, this);
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
    if (project.outputFile.isEmpty() && !project.sources.isEmpty()) {
        // Where the stack would go: beside the session, named after it
        project.outputFile = QFileInfo(fileName).absoluteDir().absoluteFilePath(QFileInfo(fileName).completeBaseName() + ".tbc");
    }

    QString error;
    if (!project.save(fileName, error)) {
        QMessageBox::warning(this, tr("Export session"), error);
        return;
    }
    sessionFile = fileName;
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
        chromaDialog->setSystem(info.system, true);
    }

    SourceState &state = states[info.id];
    state.info = info;
    state.loaded = info.ok;
    state.failed = !info.ok;
    if (!info.ok) statusBar()->showMessage(info.error, 10000);

    rebuildSourceList();
    updateTabTexts();
    sourcesChanged();
}

// The sources shown as rows: switch, colour, name, offset
void MainWindow::rebuildSourceList()
{
    const QString selected = sourceList->currentItem() ? sourceList->currentItem()->data(Qt::UserRole).toString() : project.selected;
    const QSignalBlocker blocker(sourceList);
    sourceList->clear();

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        const SourceState state = states.value(source.id);
        const QString id = source.id;

        QWidget *row = new QWidget;
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
        else detail = tr("%1 frames - %2").arg(state.info.frames).arg(state.info.system == PAL ? "PAL" : (state.info.system == NTSC ? "NTSC" : "PAL-M"));
        QLabel *name = new QLabel(QString("%1<br><small>%2</small>").arg(sourceTitle(source).toHtmlEscaped(), detail.toHtmlEscaped()));
        name->setToolTip(source.file);
        if (state.failed) name->setForegroundRole(QPalette::PlaceholderText);
        name->setEnabled(source.enabled);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        top->addWidget(name, 1);

        QToolButton *snrButton = new QToolButton;
        snrButton->setText(tr("SNR"));
        snrButton->setToolTip(tr("The SNR of this source over the disc, in a window"));
        top->addWidget(snrButton);
        connect(snrButton, &QToolButton::clicked, this, [this, id]() { openSnrWindow(id); });
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
        offset->setToolTip(tr("Offset in frames: the source is shifted by this many frames to align it with the others"));
        offset->setMinimumWidth(90);
        if (index == referenceIndex()) {
            offset->setEnabled(false);
            offset->setToolTip(tr("The first source that is on is the reference: the others are lined up with it, and it has no offset"));
        }
        bottom->addWidget(offset);
        bottom->addStretch(1);
        layout->addLayout(bottom);

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
        item->setSizeHint(row->sizeHint());
        sourceList->addItem(item);
        sourceList->setItemWidget(item, row);
        if (id == selected) sourceList->setCurrentItem(item);
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
        tabBar->setTabToolTip(index, source.file);
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

qint32 MainWindow::effectiveOffset(qint32 index) const
{
    return index == referenceIndex() ? 0 : project.sources[index].offset;
}

float MainWindow::sourceSnr(const SourceState &state, qint32 atFrame, qint32 offset, bool &known) const
{
    known = false;
    if (!state.loaded) return 0;
    const qint32 own = atFrame - offset;
    if (own < 1 || own > state.info.whiteSnr.size()) return 0;
    const float value = state.info.whiteSnr[own - 1];
    known = std::isfinite(value);
    return known ? value : 0;
}

// Each tab says what the SNR of its picture is at this frame
void MainWindow::updateTabTexts()
{
    if (tabBar->count() != project.sources.size()) return;

    const QSignalBlocker blocker(tabBar);
    const bool stackKnown = frame >= 1 && frame <= stackWhite.size() && std::isfinite(stackWhite[frame - 1]);
    stackButton->setText(stackKnown ? tr("Stack  %1 dB").arg(stackWhite[frame - 1], 0, 'f', 1) : tr("Stack"));

    // The source with the best SNR here
    qint32 best = -1;
    float bestValue = 0;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known;
        const float value = sourceSnr(states.value(source.id), frame, effectiveOffset(index), known);
        if (known && source.enabled && (best < 0 || value > bestValue)) { best = index; bestValue = value; }
    }

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known;
        const float value = sourceSnr(states.value(source.id), frame, effectiveOffset(index), known);
        tabBar->setTabText(index, known ? tr("%1  %2 dB").arg(index + 1).arg(value, 0, 'f', 1) : QString::number(index + 1));
        tabBar->setTabTextColor(index, index == best ? QColor("#3b6d11") : palette().color(QPalette::WindowText));
    }
}

// How many frames the timeline has: as far as any source on goes
void MainWindow::updateTimeline()
{
    qint32 total = 0;
    const qint32 reference = referenceIndex();
    if (reference >= 0) {
        total = states.value(project.sources[reference].id).info.frames;
    } else {
        // None on: the longest of those that are loaded
        for (const StackSource &source: project.sources) {
            const SourceState state = states.value(source.id);
            if (state.loaded) total = qMax(total, state.info.frames + source.offset);
        }
    }
    totalFrames = total;

    const QSignalBlocker spinBlocker(frameSpin);
    const QSignalBlocker sliderBlocker(frameSlider);
    frameSpin->setRange(1, qMax(1, total));
    frameSlider->setRange(1, qMax(1, total));
    frame = qBound(1, frame, qMax(1, total));
    frameSpin->setValue(frame);
    frameSlider->setValue(frame);
    totalLabel->setText(tr("of %1").arg(total));

    const bool enabled = total > 0;
    for (QWidget *widget: { static_cast<QWidget *>(firstButton), static_cast<QWidget *>(previousButton),
                            static_cast<QWidget *>(nextButton), static_cast<QWidget *>(lastButton),
                            static_cast<QWidget *>(frameSpin), static_cast<QWidget *>(frameSlider) }) {
        widget->setEnabled(enabled);
    }
}

// What is stacked changed (a source on or off, an offset, the mode): the stack is worked out again
void MainWindow::sourcesChanged()
{
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

void MainWindow::setFrame(qint32 newFrame)
{
    if (totalFrames < 1) return;
    newFrame = qBound(1, newFrame, totalFrames);
    if (newFrame == frame && frameSpin->value() == frame && frameSlider->value() == frame) return;
    frame = newFrame;

    {
        const QSignalBlocker spinBlocker(frameSpin);
        const QSignalBlocker sliderBlocker(frameSlider);
        frameSpin->setValue(frame);
        frameSlider->setValue(frame);
    }
    snrPlot->setMarker(frame);
    updateTabTexts();
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
    request.settings = project.stacking;
    for (const StackSource &source: project.sources) {
        FrameRequest::Entry entry;
        entry.id = source.id;
        entry.enabled = source.enabled;
        entry.offset = effectiveOffset(request.entries.size());
        request.entries.append(entry);
    }
    backend->latest[channel].store(request.serial);
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
        }
    }

    if (result.view == "stack") {
        lastStackResult = result;
        if (result.frame >= 1 && result.frame <= stackWhite.size()) {
            if (std::isfinite(result.stackWhiteSnr)) stackWhite[result.frame - 1] = static_cast<float>(result.stackWhiteSnr);
            if (std::isfinite(result.stackBlackSnr)) stackBlack[result.frame - 1] = static_cast<float>(result.stackBlackSnr);
        }
        if (stackWindow->isVisible()) showStackPicture(result);
        updateTabTexts();
        updatePlot();
    }
    updateStatus();
}

void MainWindow::showStackPicture(const FrameResult &result)
{
    if (result.image.isNull()) {
        stackWindow->setMessage(result.message);
        return;
    }
    const QString snr = std::isfinite(result.stackWhiteSnr) ? tr("  -  %1 dB").arg(result.stackWhiteSnr, 0, 'f', 1) : QString();
    stackWindow->setPicture(enhanceDialog->apply(result.image), tr("Stack, frame %1%2").arg(result.frame).arg(snr));
}

// The dialogs show what the session holds
void MainWindow::syncDialogs()
{
    optionsDialog->setSettings(project.stacking);
}

void MainWindow::openSnrWindow(const QString &id)
{
    snrWindowSource = id;
    snrWindow->show();
    snrWindow->raise();
    updateSnrWindow();   // (it only follows what is stacked once it is visible)
}

// The SNR window follows what is stacked and the frame viewed
void MainWindow::updateSnrWindow()
{
    if (snrWindowSource.isEmpty() || !snrWindow->isVisible()) return;
    const qint32 index = project.indexOfSource(snrWindowSource);
    if (index < 0) {
        snrWindow->hide();
        return;
    }
    const StackSource &source = project.sources[index];
    snrWindow->setContent(tr("SNR - source %1 (%2)").arg(index + 1).arg(sourceTitle(source)), sourceCurve(index, true),
                          sourceCurve(index, false), stackWhite, stackBlack, frame);
}

// What the picture shows changed: the stack or a source
void MainWindow::viewChanged()
{
    updatePlot();
    updateStatus();
    requestFrame();
}

void MainWindow::updatePlot()
{
    snrPlot->setMarker(frame);
    updateSnrWindow();

    const QString view = currentView();
    if (view == "stack") {
        QString label = tr("Stack, measured from the stacked test signals");
        if (totalFrames > 0 && stackSnrDone < totalFrames) label += tr(" - %1%").arg(stackSnrDone * 100 / totalFrames);
        snrPlot->setData(label, stackWhite, stackBlack);
        return;
    }

    const qint32 index = project.indexOfSource(view);
    if (index < 0) return;
    snrPlot->setData(tr("Source %1, from its metadata").arg(index + 1), sourceCurve(index, true), sourceCurve(index, false));
}

// The SNR of a source along the timeline, shifted by its offset
QVector<float> MainWindow::sourceCurve(qint32 index, bool white) const
{
    QVector<float> values(totalFrames, std::nanf(""));
    const StackSource &source = project.sources[index];
    const SourceState state = states.value(source.id);
    if (!state.loaded) return values;
    const QVector<float> &own = white ? state.info.whiteSnr : state.info.blackSnr;
    const qint32 offset = effectiveOffset(index);
    for (qint32 at = 1; at <= totalFrames; at++) {
        const qint32 ownFrame = at - offset;
        if (ownFrame >= 1 && ownFrame <= own.size()) values[at - 1] = own[ownFrame - 1];
    }
    return values;
}

// Start measuring the SNR of the stack over the whole timeline, in the background (and stop any earlier one)
void MainWindow::startStackSnr()
{
    stackWhite = QVector<float>(totalFrames, std::nanf(""));
    stackBlack = QVector<float>(totalFrames, std::nanf(""));
    stackSnrDone = 0;

    const quint64 job = ++snrJob;
    backend->snrGeneration.store(job);
    if (totalFrames < 1 || referenceIndex() < 0) return;

    FrameRequest request;
    request.settings = project.stacking;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        FrameRequest::Entry entry;
        entry.id = project.sources[index].id;
        entry.enabled = project.sources[index].enabled;
        entry.offset = effectiveOffset(index);
        request.entries.append(entry);
    }
    QMetaObject::invokeMethod(backend, "computeStackSnr", Qt::QueuedConnection, Q_ARG(FrameRequest, request),
                              Q_ARG(qint32, 1), Q_ARG(quint64, job));
}

void MainWindow::on_stackSnrReady(quint64 generation, const QVector<StackSnrPoint> &points, bool finished)
{
    if (generation != snrJob) return;   // of an earlier job
    for (const StackSnrPoint &point: points) {
        if (point.frame < 1 || point.frame > stackWhite.size()) continue;
        stackWhite[point.frame - 1] = point.white;
        stackBlack[point.frame - 1] = point.black;
        stackSnrDone = qMax(stackSnrDone, point.frame);
    }
    if (finished) stackSnrDone = totalFrames;
    updateTabTexts();
    updatePlot();
}

void MainWindow::updateStatus()
{
    qint32 on = 0, loaded = 0;
    for (const StackSource &source: project.sources) {
        if (states.value(source.id).loaded) loaded++;
        if (source.enabled && states.value(source.id).loaded) on++;
    }

    if (project.sources.isEmpty()) {
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
    if (mode == -1 && on > 0) {
        static const char *const resolved[] = { "Mean", "Median", "Smart mean", "Smart neighbour", "Neighbour" };
        const qint32 used = on <= 2 ? 0 : (on <= 4 ? 2 : 3);
        modeText = tr("Auto: %1").arg(tr(resolved[used]));
    }
    statusMode->setText(tr("Mode: %1").arg(modeText));

    const qint32 viewIndex = project.indexOfSource(currentView());
    statusShowing->setText(tr("Showing: %1 - frame %2").arg(isStack ? tr("stack") : tr("source %1").arg(viewIndex + 1)).arg(frame));
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
    const QSize size(qRound(currentImage.width() * zoom), qRound(currentImage.height() * zoom));
    const Qt::TransformationMode mode = zoomMethodCombo->currentIndex() == 0 ? Qt::FastTransformation : Qt::SmoothTransformation;
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

// Files dropped on the window: a session, or sources to add
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList files;
    QString session;
    for (const QUrl &url: event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (path.isEmpty()) continue;
        if (path.endsWith(".stack", Qt::CaseInsensitive)) session = path; else files.append(path);
    }
    if (!session.isEmpty()) loadSession(session);
    if (!files.isEmpty()) addSources(files);
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

// View > Theme and View > Style, as in ld-analyse, with the current choices checked
void MainWindow::setupAppearanceMenus(QMenu *menu)
{
    QSettings settings;

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

    for (const auto &entry: themeEntries) {
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
            QSettings().setValue("themeMode", static_cast<qint32>(mode));
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

    for (const QString &name: names) {
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
            QSettings().setValue("styleName", name);
            updateAppearance();
        });
    }

    menu->addMenu(themeMenu);
    menu->addMenu(styleMenu);
}

// Apply the theme and style now; what keeps colours of its own is redrawn
void MainWindow::updateAppearance()
{
    isDarkTheme = applyAppearance(styleName, themeMode, customPalette);
    for (QWidget *widget: QApplication::allWidgets()) widget->update();
    applyZoom();
}

// View > Theme > Customize...: edit the custom theme with a live preview
void MainWindow::customizeTheme()
{
    const ThemeMode previousMode = themeMode;
    const QString previousPalette = customPalette;

    ThemeEditorDialog editor(QApplication::palette(), this);
    connect(&editor, &ThemeEditorDialog::previewChanged, this, [this](const QString &palette) {
        isDarkTheme = applyAppearance(styleName, ThemeMode::Custom, palette);
    });

    QSettings settings;
    if (editor.exec() == QDialog::Accepted) {
        customPalette = editor.palette();
        themeMode = ThemeMode::Custom;
        settings.setValue("customPalette", customPalette);
        settings.setValue("themeMode", static_cast<qint32>(themeMode));
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
