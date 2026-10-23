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

// Maps TBC captures with ld-discmap, one after the other, and says how it goes: the map of each (the frames in the order of the disc, and where each
// number comes from) is read from what ld-discmap writes, and given back with mapFor(); no copy of the capture is made. (The Update SNR task still
// measures with ld-process-vits, in place.)
//
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
    // The map that was worked out for a source (a FrameMap as JSON): nothing is written beside the capture, the map is kept in the session
    QByteArray mapFor(const QString &id) const { return maps.value(id); }
    // Keep the map of each source in the metadata of the source as well (its .tbc.db, its .tbc.json, or its LDX file), as soon as it is worked out; the sources
    // where that was done are in savedIds() (the others, if it failed, keep the map in the session only)
    void setSaveInSources(bool save) { saveInSources = save; }
    QStringList savedIds() const { return saved; }

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
    bool planSnr(const Job &job, QString &error);
    bool finishSnr(const Job &job, QString &error);
    void startStage();
    void cleanUp(const Job &job);
    QString workFolder(const Job &job) const;
    QVector<Stage> stages;
    qint32 stage = 0;
    QString stageOutput;
    QString ldxFile;           // the file that is rewritten (an LDX job)
    QString ldxResult;         // what is written (an LDX job)
    qint32 ldxTrack = 0;       // the track that was added, once known
    QHash<QString, QString> replaced;
    QHash<QString, QByteArray> maps;
    bool saveInSources = false;
    QStringList saved;
    QString mapFile;           // where ld-discmap writes the map of the job that runs
    bool finishMap(const Job &job, QString &error);

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
