/******************************************************************************
 * processdialog.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PROCESSDIALOG_H
#define PROCESSDIALOG_H

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <functional>
#include <memory>

#include "stackproject.h"

// Stacks the sources to a file: asks where, and in which video and metadata format, runs ld-disc-stacker on
// the session, then the other tools (ld-tbc-to-cvbs, ld-cvbs-to-tbc, ld-sqlite-to-json, ld-json-converter)
// to get the format asked for. An .ldx file can be asked for too: the stack as a capture in it, or the sources as tracks with the session that stacks them.
class ProcessDialog : public QDialog
{
    Q_OBJECT

public:
    ProcessDialog(const StackProject &project, const QString &suggestedOutput, QWidget *parent = nullptr);

    // Where the stack was written, once it is done
    QString outputFile() const { return finalVideo; }

private slots:
    void browse();
    void start();
    void stepFinished(int exitCode, QProcess::ExitStatus status);
    void cancel();

private:
    struct Step {
        QString description;
        QString program;
        QStringList arguments;
        std::function<void()> after;     // when the step succeeded
        QString stdoutFile;              // if the standard output of the program goes to a file (an LDX stream), and not to the log
    };

    void log(const QString &text);
    void setRunning(bool running);
    void runNextStep();
    void formatChanged();
    QString formatSuffix() const;
    void startLdx(const QString &output);
    void finish(bool success, const QString &message);
    QString toolPath(const QString &name) const;
    static bool isCvbs(const QString &file);
    static QString metadataFile(const QString &video, bool json);

    StackProject project;
    QLineEdit *outputEdit = nullptr;
    QComboBox *formatCombo = nullptr;
    QComboBox *metadataCombo = nullptr;
    QComboBox *codecCombo = nullptr;
    QLabel *summary = nullptr;
    QPlainTextEdit *logView = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *closeButton = nullptr;

    std::unique_ptr<QTemporaryDir> sessionDirectory;
    QProcess *process = nullptr;
    QList<Step> steps;
    qint32 currentStep = -1;
    bool cancelled = false;
    QString finalVideo;
    QStringList created;                 // what this run made, to take back if it does not finish
    QStringList temporary;               // what is only a step on the way
};

#endif // PROCESSDIALOG_H
