/******************************************************************************
 * mainwindow.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QColor>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QThread>
#include <QToolButton>

#include "framebackend.h"
#include "optionsdialogs.h"
#include "snrplot.h"
#include "sourceinfopanel.h"
#include "mapdialog.h"
#include <QPointer>
#include <QSplitter>
#include <QProgressBar>
#include "toolwindows.h"
#include "stackproject.h"
#include "theme.h"
#include "themeeditordialog.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // arguments: a .stack session file, or the sources (TBC files) to start with
    explicit MainWindow(const QStringList &arguments, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void openSession();
    void exportSession();
    void processSession();
    void addSourcesDialog();
    void on_sourceLoaded(const SourceInfo &info);
    void on_frameReady(const FrameResult &result);
    void on_stackSnrReady(quint64 generation, const QVector<StackSnrPoint> &points, bool finished);
    void on_sourceSnrReady(const QString &id, quint64 generation, const QVector<StackSnrPoint> &points, bool finished);

private:
    struct SourceState {
        SourceInfo info;
        bool loaded = false;
        bool failed = false;
        QColor color;
        // The SNR of the source measured from its fields (a value per frame of the source, NaN where it is not done yet) instead of the one of its
        // metadata, when the box of the graphs is ticked for it
        // Follow map (on by default): the source shows and stacks its mapping (ld-discmap) when there is one; off, the capture it was made from
        bool followMap = true;
        // The mapping of the source by ld-discmap (shown as a bar in its row): -2 not mapping, -1 mapping with no count yet, 0 to 100; and whether it failed
        qint32 mapProgress = -2;
        bool mapFailed = false;
        bool measure = false;
        QVector<float> measuredWhite, measuredBlack;
        qint32 measuredDone = 0;
    };

    void buildUi();
    void buildMenus();
    void loadSession(const QString &fileName);
    void addSources(const QStringList &files);
    void startSource(const StackSource &source);
    void removeSource(const QString &id);

    void rebuildSourceList();
    void rebuildTabs();
    void updateTabTexts();
    void updateTimeline();
    // The frame counter and the slider go by the frames of what is shown: the stack, from the first frame of the timeline to the last, a source, from its first frame
    // to its last (its own duration). frame stays the frame of the timeline; viewOffset is where the first frame of what is shown is on it.
    qint32 viewOffset() const;
    qint32 viewFrames() const;
    void syncFrameControls();
    // Where each source is on the timeline: by the numbers of the disc that its VBI gives (the first frame of the earliest source is the first of the timeline), plus
    // its own offset. discAtStart is the number of the disc of the first frame of the timeline (-1: not known).
    QVector<qint32> alignedOffsets;
    qint32 discAtStart = -1;
    void updateDiscLabel();
    QString discFormat(qint32 disc) const;
    StackProject projectForProcessing() const;
    void updatePlot();
    void viewChanged();
    qint32 aspectAdjustment() const;
    void updateAspectButton();
    QImage shownImage(const QImage &image) const;
    void setupAppearanceMenus(QMenu *menu);
    void updateAppearance();
    void customizeTheme();
    void resizeToImage(qint32 passesLeft = 3);
    void startStackSnr();
    // The SNR of a source measured from its fields, one source at a time in the background, when its box is ticked
    void setSourceMeasure(const QString &id, bool on);
    void startNextMeasure();
    // Follow map: the version of a source that is shown and stacked, and the switch from one to the other
    void resolveFollowMap(const QString &id);
    // Mapping (ld-discmap): the Stack button asks for the sources that follow the map and have none; the Map button of the panel does one source
    QVector<qint32> sourcesToMap(bool followingOnly) const;
    bool confirmMappingBeforeStack();
    bool startMapping(const QVector<qint32> &indexes, bool overwrite, bool thenStack, MapDialog::Task task = MapDialog::Map, const QString &inputOverride = QString());
    void mappingFinished(const QStringList &ids, const QStringList &reload, bool overwrite, bool thenStack, bool success);
    void updateMapBar(const QString &id);
    void updateMappingControls();
    QHash<QString, QPointer<QProgressBar>> mapBars;
    QPointer<MapDialog> mapDialog;
    bool mappingBusy = false;
    void mapSelectedSource();
    void updateSnrOfSelectedSource();
    void updateFollowEnabled();
    QHash<QString, QPointer<QCheckBox>> followBoxes;
    void switchSource(const QString &id, const QString &reference);
    QString measureTargetId() const;
    // One vertical scale for all the SNR graphs, so that they can be compared: it takes in what every source and the stack have
    bool snrSameScale = true;
    bool snrRangeDirty = true;
    bool snrRangeAny = false;
    double snrLow = 0, snrHigh = 0;
    void updateSnrRange();
    void applySnrRange(SnrGraph *graph);
    quint64 sourceSnrJob = 0;
    QString measuringId;
    QStringList measureQueue;
    // ownAxis: from the first frame of the source (its own duration), not along the timeline
    QVector<float> sourceCurve(qint32 index, bool white, bool ownAxis = false) const;
    void showStackPicture(const FrameResult &result);
    void syncDialogs();
    void updateSnrWindow();
    void openSnrWindow(const QString &id);
    void sendRequest(const QString &view, qint32 channel);
    void updateStatus();
    void sourcesChanged();      // what is stacked changed: the stack must be worked out again

    void setFrame(qint32 frame);
    void updateViewCombo();
    void viewModeChanged();
    void requestFrame();
    void applyZoom();
    void setZoom(double zoom);
    void stepZoom(qint32 direction);
    QString currentView() const;
    QColor colorFor(const QString &id);
    QString sourceTitle(const StackSource &source) const;
    qint32 referenceIndex() const;                 // the first source that is on and loaded: the others line up with it
    qint32 effectiveOffset(qint32 index) const;    // its offset, 0 for the reference
    float sourceSnr(const SourceState &state, qint32 frame, qint32 offset, bool &known, bool *black = nullptr) const;

    StackProject project;
    QString sessionFile;
    QMap<QString, SourceState> states;
    qint32 colorCounter = 0;

    QThread backendThread;
    FrameBackend *backend = nullptr;
    quint64 serials[2] = { 0, 0 };

    qint32 frame = 1;
    qint32 totalFrames = 0;
    double zoom = 1.0;
    QImage currentImage;
    FrameResult lastResult;       // the last picture of the main view
    FrameResult lastStackResult;  // the last stack, wherever it was asked for
    QMap<QString, SnrWindow *> snrWindows;   // a window for each source whose SNR was asked for
    StackOutputWindow *stackWindow = nullptr;
    StackingOptionsDialog *optionsDialog = nullptr;
    EnhanceDialog *enhanceDialog = nullptr;
    ChromaConfigDialog *chromaDialog = nullptr;
    QAction *stackOutputAction = nullptr;
    QVector<float> stackWhite;    // the SNR of the stack per frame, NaN until measured
    QVector<float> stackBlack;
    qint32 stackSnrDone = 0;      // how many frames of it have been measured
    quint64 snrJob = 0;

    QListWidget *sourceList = nullptr;
    SourceInfoPanel *infoPanel = nullptr;     // what is known of the source that is chosen, between the list and the picture
    QSplitter *mainSplitter = nullptr;
    qint32 infoPanelWidth = 300;          // (the room that the panel takes when it is shown: the window grows and shrinks by it)
    QPushButton *infoButton = nullptr;
    void updateInfoPanel();
    QTabBar *tabBar = nullptr;
    QPushButton *stackButton = nullptr;
    QScrollArea *scrollArea = nullptr;
    QLabel *imageLabel = nullptr;
    SnrGraph *snrPlot = nullptr;
    QToolButton *firstButton = nullptr;
    QToolButton *previousButton = nullptr;
    QToolButton *nextButton = nullptr;
    QToolButton *lastButton = nullptr;
    QSpinBox *frameSpin = nullptr;
    QSlider *frameSlider = nullptr;
    QLabel *totalLabel = nullptr;
    QLabel *discLabel = nullptr;           // where the picture is on the disc (the picture number, or the time) and in the file
    QComboBox *modeCombo = nullptr;
    qint32 zoomMethod = 0;          // 0 line doubling, 1 smooth
    QSpinBox *zoomSpin = nullptr;
    QPushButton *chromaButton = nullptr;
    QToolButton *originalSizeButton = nullptr;
    QPushButton *aspectButton = nullptr;
    QComboBox *viewCombo = nullptr;   // Frame, Split, Field 1, Field 2
    bool displayAspect = true;
    bool tintRows = true;           // the rows of the sources are tinted with the colour of the source
    bool fieldView = false;         // one field of the frame, not the frame
    bool fieldSplit = false;        // in field view: both fields one above the other
    qint32 fieldIndex = 0;          // in field view, which field of the frame: 0 the first, 1 the second
    bool fitFirstSource = false;    // the next source to be loaded is the first one imported
    bool fitPicture = false;        // and its first picture is to be shown at its original size
    bool fitWindow = false;         // a session was opened: the window is to fit its picture at its own zoom
    QLabel *statusSources = nullptr;
    QLabel *statusMode = nullptr;
    QLabel *statusFormat = nullptr;
    QLabel *statusShowing = nullptr;

    ThemeMode themeMode = ThemeMode::Auto;
    QString styleName;
    QString customPalette;
    QAction *customThemeAction = nullptr;
    bool isDarkTheme = false;

    bool panning = false;
    QPoint panStart;
    QPoint panScroll;
};

#endif // MAINWINDOW_H
