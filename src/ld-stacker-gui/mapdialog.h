/******************************************************************************
 * mapdialog.h
 * ld-stacker-gui - mapping sources with ld-discmap
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef MAPDIALOG_H
#define MAPDIALOG_H

#include <QDialog>
#include <QHash>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

// Maps TBC captures with ld-discmap, one after the other, and says how it goes: each is written as a copy beside it (name_map.tbc, with its
// metadata and audio) or, to overwrite, written beside it under a temporary name and put in the place of the original when it is done (so that a
// mapping that fails leaves the capture as it was).
//
// A capture that is a track of an LDX file (file.ldx#2) is taken out into a folder beside the file, mapped there, and put back by ld-container: as a track of
// its own that says it is the mapping of the capture (--mapped-from), or, to overwrite, in the place of the track (which is removed). The file is written
// again under a temporary name and put in the place of the original when it is done.
class MapDialog : public QDialog
{
    Q_OBJECT

public:
    struct Job {
        QString id;        // the source
        QString title;     // what the person calls it
        QString input;     // the TBC file to map
    };

    // Map: ld-discmap. UpdateSnr: ld-process-vits measures the test signals of every field again and writes the SNR in the metadata of the capture (the
    // metadata file in place, with the backup the tool makes; for a track of an LDX file, a new block of metadata in the file)
    enum Task { Map, UpdateSnr };
    MapDialog(const QVector<Job> &jobs, bool overwrite, QWidget *parent = nullptr, Task task = Map);

    // Begins the mapping (once the signals are connected)
    void start();

    // The sources that were mapped, and whether all of them were
    QStringList mappedIds() const { return done; }
    // The name of the track that replaced a capture of an LDX file that was overwritten (file.ldx#5), empty for any other
    QString replacedReference(const QString &id) const { return replaced.value(id); }
    bool succeeded() const { return allDone; }

    // The files that go with a TBC file and that ld-discmap writes with it: its metadata (the one that is there, db or json) and its audio
    static QStringList companions(const QString &tbcFile);
    // The file a job writes: the mapping (name_map.tbc), or the temporary file when the capture is overwritten
    static QString outputFor(const QString &input, bool overwrite);
    // Whether a job is a track of an LDX file
    static bool isLdxJob(const Job &job);

signals:
    void jobStarted(const QString &id);
    void jobProgress(const QString &id, qint32 percent);     // 0 to 100, -1 while it cannot be told yet
    void jobFinished(const QString &id, bool ok);
    void allFinished(bool success);

private:
    void runNext();
    void finish(bool success, const QString &message);
    void log(const QString &text);
    bool replaceOriginal(const Job &job, QString &error);
    void removeOutput(const Job &job);
    QString toolPath(const QString &name = QStringLiteral("ld-discmap")) const;

    // What a job runs: one program after the other (ld-discmap alone for a .tbc file; for a track of an LDX file, ld-container takes its files out, then ld-discmap,
    // then ld-container puts the mapping in the file)
    struct Stage {
        QString program;
        QStringList arguments;
        QString output;     // the file that has to be there when it is done, if any
        bool mapping = false;
    };
    bool plan(const Job &job, QString &error);
    bool planLdx(const Job &job, QString &error);
    bool planSnr(const Job &job, QString &error);
    bool finishSnr(const Job &job, QString &error);
    void startStage();
    bool finishLdx(const Job &job, QString &error);
    void cleanUp(const Job &job);
    QString workFolder(const Job &job) const;
    QVector<Stage> stages;
    qint32 stage = 0;
    QString stageOutput;
    QString ldxFile;           // the file that is rewritten (an LDX job)
    QString ldxResult;         // what is written (an LDX job)
    qint32 ldxTrack = 0;       // the track that was added, once known
    QHash<QString, QString> replaced;

    QVector<Job> jobs;
    bool overwrite;
    Task task = Map;
    qint32 current = -1;
    QStringList done;
    bool allDone = false;
    bool cancelled = false;
    bool finished = false;
    QProcess *process = nullptr;
    QTimer *progressTimer = nullptr;
    qint32 metadataPercent = -1;
    bool unmappableSeen = false;
    void reportProgress();
    QLabel *summary = nullptr;
    QPlainTextEdit *logView = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *closeButton = nullptr;
};

#endif // MAPDIALOG_H
