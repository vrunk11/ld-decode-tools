/******************************************************************************
 * mainwindow.h
 * ldx-packer - the window: the list of captures and the panel with what is known of the one picked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "document.h"

#include <QMainWindow>
#include <QSet>

class AppearanceMenus;
class CoverWidget;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();

    Document *document() { return &m_doc; }
    // Open an .ldx file (the list starts from its captures) or add captures to the list. False when something could not be read.
    bool openArchive(const QString &path);
    bool addCaptures(const QStringList &paths);
    // The capture whose information is shown (-1: none).
    void pick(int index);
    // Pick a track under a capture (its place in the tracks that go with it, -1: the notes)
    void pickTrack(int capture, int row);
    int picked() const { return m_picked; }
    bool saveScreenshot(const QString &path);
    // Write the list: asks where (a copy or over the file) unless `output` is given; the work runs in ld-container.
    bool process(const QString &output = QString(), bool verifyAfter = true);
    // Give the tracks of the opened file back as separate files, in a folder (asked unless `folder` is given).
    bool unpack(const QString &folder = QString());
    // Check that every block of the opened file reads back the same. Says what it found, unless quiet; true when the file is sound.
    bool verify(bool quiet = false);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void rebuildList();
    void refreshInfo();
    void refreshSummary();
    void showInfo(bool shown);
    void syncCodec();
    void newList();
    void chooseArchive();
    void chooseCaptures();
    bool confirmDiscard();
    void editName(const QString &text);
    void editNotes();
    void addCompanion();
    void editMetadata();
    void editRfSource();
    void editParameters(bool video);
    void chooseSession();
    void refreshSession();
    void refreshCover();
    void chooseCover();
    void chooseCoverSource();
    void chooseCoverLink();
    void refreshTrackEditor();
    void removeCompanion();
    void removeTrack(int capture, int row);
    void showHistory();
    void move(int index, int delta);
    void removeCapture(int index);
    // Runs ld-container with the arguments in a window that shows what it says; false (and says why) when it fails.
    bool runTool(const QString &title, const QStringList &arguments, QString *lastLine = nullptr, bool quiet = false);

    Document m_doc;
    AppearanceMenus *m_appearance = nullptr;
    int m_picked = -1;
    int m_pickedTrack = -2;             // the track picked under the capture (its place in the tracks that go with it, -1: the notes, -2: none)
    QSet<QString> m_collapsed;          // the captures whose tracks are not shown under them
    bool m_editing = false;
    bool m_rebuildQueued = false;
    bool m_infoShown = true;

    QPushButton *m_processButton = nullptr;
    QPushButton *m_updateButton = nullptr;
    QComboBox *m_codec = nullptr;
    QComboBox *m_layoutBox = nullptr;
    QPushButton *m_infoButton = nullptr;
    QWidget *m_listHost = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QScrollArea *m_listScroll = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_hint = nullptr;
    QAction *m_infoAction = nullptr;
    QAction *m_unpackAction = nullptr;
    QAction *m_verifyAction = nullptr;
    QAction *m_historyAction = nullptr;
    QPushButton *m_verifyAll = nullptr;

    QWidget *m_info = nullptr;
    QLabel *m_infoTitle = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_facts = nullptr;
    QComboBox *m_aspect = nullptr;
    QLabel *m_aspectWarning = nullptr;
    QPushButton *m_aspectFix = nullptr;
    QString m_aspectFixTo;
    QComboBox *m_system = nullptr;
    // What a person says of the capture (see refreshInfo)
    QComboBox *m_medium = nullptr;
    QComboBox *m_face = nullptr;
    QLineEdit *m_reference = nullptr;
    QSpinBox *m_copy = nullptr;
    QSpinBox *m_captureNumber = nullptr;
    bool m_refreshingSaid = false;
    QDoubleSpinBox *m_videoRate = nullptr;
    QFrame *m_trackBox = nullptr;
    QLabel *m_trackInfo = nullptr;
    QLineEdit *m_trackName = nullptr;
    QLabel *m_trackRateLabel = nullptr;
    QDoubleSpinBox *m_trackRate = nullptr;
    QLabel *m_trackWarning = nullptr;
    QPushButton *m_trackFix = nullptr;
    QListWidget *m_companions = nullptr;
    QPushButton *m_addCompanion = nullptr;
    QPushButton *m_removeCompanion = nullptr;
    QPushButton *m_metadata = nullptr;
    QPushButton *m_rfSource = nullptr;
    QPushButton *m_videoParameters = nullptr;
    QPushButton *m_trackParameters = nullptr;
    QFrame *m_sessionBox = nullptr;
    QFrame *m_coverBox = nullptr;
    CoverWidget *m_coverImage = nullptr;
    QPushButton *m_coverSource = nullptr;
    QPushButton *m_coverLink = nullptr;
    QLabel *m_coverText = nullptr;
    QPushButton *m_coverSet = nullptr;
    QPushButton *m_coverDrop = nullptr;
    QLabel *m_sessionText = nullptr;
    QPushButton *m_sessionSet = nullptr;
    QPushButton *m_sessionDrop = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QPushButton *m_deleteNotes = nullptr;
};

#endif // MAINWINDOW_H
