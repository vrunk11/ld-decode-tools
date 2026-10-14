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

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
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

    backend = new FrameBackend;
    backend->moveToThread(&backendThread);
    connect(&backendThread, &QThread::finished, backend, &QObject::deleteLater);
    connect(backend, &FrameBackend::sourceLoaded, this, &MainWindow::on_sourceLoaded);
    connect(backend, &FrameBackend::frameReady, this, &MainWindow::on_frameReady);
    backendThread.start();

    buildUi();
    buildMenus();

    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    if (!settings.contains("geometry")) resize(1200, 800);

    zoom = 2.0;
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
    connect(sourceList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        // The row picked is the tab shown (not when the list sets its own current row as it is filled)
        if (!item || !sourceList->hasFocus()) return;
        const QString id = item->data(Qt::UserRole).toString();
        const qint32 index = project.indexOfSource(id);
        if (index >= 0 && tabBar->currentIndex() != index + 1) tabBar->setCurrentIndex(index + 1);
    });
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

    snrPlot = new SnrPlot;
    snrPlot->setFixedHeight(76);
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
    chromaButton->setEnabled(false);
    chromaButton->setToolTip(tr("Chroma decoding of the pictures: not available yet"));
    optionsLayout->addWidget(modeCombo);
    optionsLayout->addWidget(chromaButton);
    optionsLayout->addStretch(1);
    QToolButton *zoomOut = makeButton("-", tr("Zoom out"), true);
    QToolButton *zoomIn = makeButton("+", tr("Zoom in"), true);
    zoomLabel = new QLabel;
    zoomLabel->setMinimumWidth(44);
    zoomLabel->setAlignment(Qt::AlignCenter);
    zoomMethodCombo = new QComboBox;
    zoomMethodCombo->addItem(tr("Line doubling"));
    zoomMethodCombo->addItem(tr("Smooth"));
    zoomMethodCombo->setToolTip(tr("How the picture is enlarged: line doubling keeps the pixels as they are"));
    optionsLayout->addWidget(zoomOut);
    optionsLayout->addWidget(zoomLabel);
    optionsLayout->addWidget(zoomIn);
    optionsLayout->addWidget(zoomMethodCombo);
    mainLayout->addWidget(options);

    QSplitter *splitter = new QSplitter;
    splitter->addWidget(side);
    splitter->addWidget(main);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 260, 900 });
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
        if (index < 0) return;
        project.selected = index == 0 ? QStringLiteral("stack") : project.sources.value(index - 1).id;
        if (index > 0) {
            // The row follows the tab
            for (qint32 row = 0; row < sourceList->count(); row++) {
                if (sourceList->item(row)->data(Qt::UserRole).toString() == project.selected) {
                    const QSignalBlocker blocker(sourceList);
                    sourceList->setCurrentRow(row);
                }
            }
        }
        updatePlot();
        updateStatus();
        requestFrame();
    });
    connect(frameSpin, &QSpinBox::valueChanged, this, &MainWindow::setFrame);
    connect(frameSlider, &QSlider::valueChanged, this, &MainWindow::setFrame);
    connect(firstButton, &QToolButton::clicked, this, [this]() { setFrame(1); });
    connect(previousButton, &QToolButton::clicked, this, [this]() { setFrame(frame - 1); });
    connect(nextButton, &QToolButton::clicked, this, [this]() { setFrame(frame + 1); });
    connect(lastButton, &QToolButton::clicked, this, [this]() { setFrame(totalFrames); });
    connect(snrPlot, &SnrPlot::frameRequested, this, &MainWindow::setFrame);
    connect(modeCombo, &QComboBox::currentIndexChanged, this, [this](qint32 index) {
        project.stacking.mode = index - 1;
        sourcesChanged();
    });
    connect(zoomIn, &QToolButton::clicked, this, [this]() { setZoom(zoom * 1.25); });
    connect(zoomOut, &QToolButton::clicked, this, [this]() { setZoom(zoom / 1.25); });
    connect(zoomMethodCombo, &QComboBox::currentIndexChanged, this, [this]() { applyZoom(); });

    new QShortcut(Qt::Key_Left, this, [this]() { setFrame(frame - 1); });
    new QShortcut(Qt::Key_Right, this, [this]() { setFrame(frame + 1); });
    for (qint32 digit = 0; digit <= 9; digit++) {
        new QShortcut(QKeySequence(Qt::Key_0 + digit), this, [this, digit]() {
            if (digit < tabBar->count()) tabBar->setCurrentIndex(digit);
        });
    }

    rebuildTabs();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open session..."), QKeySequence::Open, this, &MainWindow::openSession);
    file->addAction(tr("&Add source..."), this, &MainWindow::addSourcesDialog);
    file->addSeparator();
    file->addAction(tr("&Export session..."), QKeySequence::Save, this, &MainWindow::exportSession);
    QAction *process = file->addAction(tr("&Process..."));
    process->setEnabled(false);
    process->setToolTip(tr("Stacks the sources to a file: not available yet"));
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *stackOutput = view->addAction(tr("Stack output"));
    stackOutput->setEnabled(false);
    stackOutput->setToolTip(tr("A window with the final stack: not available yet"));
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
    stackSnr.clear();
    currentImage = QImage();

    project = loaded;
    sessionFile = fileName;
    for (const StackSource &source: project.sources) startSource(source);

    {
        const QSignalBlocker blocker(modeCombo);
        modeCombo->setCurrentIndex(qBound(0, project.stacking.mode + 1, modeCombo->count() - 1));
    }
    setZoom(project.zoom);
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
        QHBoxLayout *layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 3, 4, 3);
        layout->setSpacing(6);

        QCheckBox *enabled = new QCheckBox;
        enabled->setChecked(source.enabled);
        enabled->setToolTip(tr("In the stack"));
        layout->addWidget(enabled);

        QLabel *dot = new QLabel;
        dot->setPixmap(dotIcon(colorFor(id)).pixmap(12, 12));
        layout->addWidget(dot);

        QString detail;
        if (state.failed) detail = tr("could not be loaded");
        else if (!state.loaded) detail = tr("loading...");
        else detail = tr("%1 frames - %2").arg(state.info.frames).arg(state.info.system == PAL ? "PAL" : (state.info.system == NTSC ? "NTSC" : "PAL-M"));
        QLabel *name = new QLabel(QString("%1<br><small>%2</small>").arg(sourceTitle(source).toHtmlEscaped(), detail.toHtmlEscaped()));
        name->setToolTip(source.file);
        if (state.failed) name->setForegroundRole(QPalette::PlaceholderText);
        name->setEnabled(source.enabled);
        layout->addWidget(name, 1);

        QSpinBox *offset = new QSpinBox;
        offset->setRange(-1000000, 1000000);
        offset->setValue(source.offset);
        offset->setPrefix(source.offset > 0 ? "+" : "");
        offset->setToolTip(tr("Offset in frames: the source is shifted by this many frames to align it with the others"));
        offset->setFixedWidth(70);
        layout->addWidget(offset);

        connect(enabled, &QCheckBox::toggled, this, [this, id, name](bool on) {
            const qint32 at = project.indexOfSource(id);
            if (at < 0) return;
            project.sources[at].enabled = on;
            name->setEnabled(on);
            sourcesChanged();
        });
        connect(offset, &QSpinBox::valueChanged, this, [this, id, offset](qint32 value) {
            const qint32 at = project.indexOfSource(id);
            if (at < 0) return;
            project.sources[at].offset = value;
            offset->setPrefix(value > 0 ? "+" : "");
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

    tabBar->addTab(dotIcon(STACK_COLOR), tr("Stack"));
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        tabBar->addTab(dotIcon(colorFor(source.id)), QString::number(index + 1));
        tabBar->setTabToolTip(index + 1, source.file);
    }

    qint32 current = 0;
    const qint32 selectedIndex = project.indexOfSource(project.selected);
    if (selectedIndex >= 0) current = selectedIndex + 1; else project.selected = "stack";
    tabBar->setCurrentIndex(current);
    updateTabTexts();
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
    if (tabBar->count() != project.sources.size() + 1) return;

    const QSignalBlocker blocker(tabBar);
    const auto stackValue = stackSnr.constFind(frame);
    tabBar->setTabText(0, stackValue != stackSnr.constEnd() ? tr("Stack  %1 dB").arg(*stackValue, 0, 'f', 1) : tr("Stack"));

    // The source with the best SNR here
    qint32 best = -1;
    float bestValue = 0;
    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known;
        const float value = sourceSnr(states.value(source.id), frame, source.offset, known);
        if (known && source.enabled && (best < 0 || value > bestValue)) { best = index; bestValue = value; }
    }

    for (qint32 index = 0; index < project.sources.size(); index++) {
        const StackSource &source = project.sources[index];
        bool known;
        const float value = sourceSnr(states.value(source.id), frame, source.offset, known);
        tabBar->setTabText(index + 1, known ? tr("%1  %2 dB").arg(index + 1).arg(value, 0, 'f', 1) : QString::number(index + 1));
        tabBar->setTabTextColor(index + 1, index == best ? QColor("#3b6d11") : palette().color(QPalette::WindowText));
    }
}

// How many frames the timeline has: as far as any source on goes
void MainWindow::updateTimeline()
{
    qint32 total = 0;
    for (const StackSource &source: project.sources) {
        const SourceState state = states.value(source.id);
        if (!state.loaded || !source.enabled) continue;
        total = qMax(total, state.info.frames + source.offset);
    }
    if (total == 0) {
        // None on: the sources that are loaded give the length
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
    stackSnr.clear();
    updateTimeline();
    updateTabTexts();
    updatePlot();
    updateStatus();
    requestFrame();
}

// ---------------------------------------------------------------------------------------------
// Frames

QString MainWindow::currentView() const
{
    return project.selected.isEmpty() ? QStringLiteral("stack") : project.selected;
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
        return;
    }

    FrameRequest request;
    request.serial = ++serial;
    request.frame = frame;
    request.view = currentView();
    request.settings = project.stacking;
    for (const StackSource &source: project.sources) {
        FrameRequest::Entry entry;
        entry.id = source.id;
        entry.enabled = source.enabled;
        entry.offset = source.offset;
        request.entries.append(entry);
    }
    backend->latest.store(request.serial);
    QMetaObject::invokeMethod(backend, "render", Qt::QueuedConnection, Q_ARG(FrameRequest, request));
}

void MainWindow::on_frameReady(const FrameResult &result)
{
    if (result.serial != serial) return;   // a newer one is on its way
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

    if (result.view == "stack" && std::isfinite(result.stackWhiteSnr)) {
        stackSnr.insert(result.frame, static_cast<float>(result.stackWhiteSnr));
        updateTabTexts();
        updatePlot();
    }
    updateStatus();
}

void MainWindow::updatePlot()
{
    snrPlot->setMarker(frame);

    const QString view = currentView();
    if (view == "stack") {
        snrPlot->setSparseSeries(stackSnr, totalFrames, STACK_COLOR, tr("Stack: white SNR, measured as the frames are viewed"));
        return;
    }

    const qint32 index = project.indexOfSource(view);
    if (index < 0) return;
    const StackSource &source = project.sources[index];
    const SourceState state = states.value(source.id);
    QVector<float> values(totalFrames, std::nanf(""));
    for (qint32 at = 1; at <= totalFrames; at++) {
        bool known;
        const float value = sourceSnr(state, at, source.offset, known);
        if (known) values[at - 1] = value;
    }
    snrPlot->setSeries(values, colorFor(source.id), tr("Source %1: white SNR (metadata)").arg(index + 1));
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
    if (isStack && lastResult.view == "stack" && lastResult.frame == frame && lastResult.contributing > 0) {
        statusSources->setText(tr("%1 of %2 sources contribute to this frame").arg(lastResult.contributing).arg(project.sources.size()));
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
    zoomLabel->setText(QString("%1%").arg(qRound(zoom * 100)));
    applyZoom();
}

void MainWindow::applyZoom()
{
    if (currentImage.isNull()) return;
    const QSize size(qRound(currentImage.width() * zoom), qRound(currentImage.height() * zoom));
    const Qt::TransformationMode mode = zoomMethodCombo->currentIndex() == 0 ? Qt::FastTransformation : Qt::SmoothTransformation;
    imageLabel->setPixmap(QPixmap::fromImage(currentImage).scaled(size, Qt::IgnoreAspectRatio, mode));
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
                setZoom(zoom * (wheel->angleDelta().y() > 0 ? 1.25 : 0.8));
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
