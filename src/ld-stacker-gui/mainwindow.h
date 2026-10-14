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

private:
    struct SourceState {
        SourceInfo info;
        bool loaded = false;
        bool failed = false;
        QColor color;
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
    QVector<float> sourceCurve(qint32 index, bool white) const;
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
    float sourceSnr(const SourceState &state, qint32 frame, qint32 offset, bool &known) const;

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
    QComboBox *modeCombo = nullptr;
    qint32 zoomMethod = 0;          // 0 line doubling, 1 smooth
    QSpinBox *zoomSpin = nullptr;
    QPushButton *chromaButton = nullptr;
    QToolButton *originalSizeButton = nullptr;
    QPushButton *aspectButton = nullptr;
    QComboBox *viewCombo = nullptr;   // Frame, Split, Field 1, Field 2
    bool displayAspect = true;
    bool fieldView = false;         // one field of the frame, not the frame
    bool fieldSplit = false;        // in field view: both fields one above the other
    qint32 fieldIndex = 0;          // in field view, which field of the frame: 0 the first, 1 the second
    bool fitFirstSource = false;    // the next source to be loaded is the first one imported
    bool fitPicture = false;        // and its first picture is to be shown at its original size
    QLabel *statusSources = nullptr;
    QLabel *statusMode = nullptr;
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
