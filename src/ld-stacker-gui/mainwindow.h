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
    void showStackPicture(const FrameResult &result);
    void syncDialogs();
    void updateSnrWindow();
    void openSnrWindow(const QString &id);
    void sendRequest(const QString &view, qint32 channel);
    void updateStatus();
    void sourcesChanged();      // what is stacked changed: the stack must be worked out again

    void setFrame(qint32 frame);
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
    SnrWindow *snrWindow = nullptr;
    QString snrWindowSource;
    StackOutputWindow *stackWindow = nullptr;
    StackingOptionsDialog *optionsDialog = nullptr;
    EnhanceDialog *enhanceDialog = nullptr;
    ChromaConfigDialog *chromaDialog = nullptr;
    QAction *stackOutputAction = nullptr;
    QMap<qint32, float> stackSnr;

    QListWidget *sourceList = nullptr;
    QTabBar *tabBar = nullptr;
    QToolButton *stackButton = nullptr;
    QScrollArea *scrollArea = nullptr;
    QLabel *imageLabel = nullptr;
    SnrPlot *snrPlot = nullptr;
    QToolButton *firstButton = nullptr;
    QToolButton *previousButton = nullptr;
    QToolButton *nextButton = nullptr;
    QToolButton *lastButton = nullptr;
    QSpinBox *frameSpin = nullptr;
    QSlider *frameSlider = nullptr;
    QLabel *totalLabel = nullptr;
    QComboBox *modeCombo = nullptr;
    QComboBox *zoomMethodCombo = nullptr;
    QSpinBox *zoomSpin = nullptr;
    QPushButton *chromaButton = nullptr;
    QPushButton *chromaSettingsButton = nullptr;
    QPushButton *enhanceButton = nullptr;
    QLabel *statusSources = nullptr;
    QLabel *statusMode = nullptr;
    QLabel *statusShowing = nullptr;

    bool panning = false;
    QPoint panStart;
    QPoint panScroll;
};

#endif // MAINWINDOW_H
