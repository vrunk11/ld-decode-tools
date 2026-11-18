/******************************************************************************
 * processdialog.h
 * ldx-packer - what to ask before writing, and the window that shows the work going on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PROCESSDIALOG_H
#define PROCESSDIALOG_H

#include "document.h"

#include <QDialog>
#include <QProcess>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;

// Where to write (a copy, or over the file that was opened) and whether to check it afterwards.
class ProcessDialog : public QDialog
{
    Q_OBJECT

public:
    ProcessDialog(const QString &archive, QWidget *parent = nullptr);

    QString output() const;
    bool overwrites() const;
    bool verifies() const;

    void accept() override;

private:
    void chooseOutput();
    void updateOutput();

    QString m_archive;
    QRadioButton *m_copy = nullptr;
    QRadioButton *m_overwrite = nullptr;
    QLineEdit *m_path = nullptr;
    QCheckBox *m_verify = nullptr;
};

// Runs the steps of a plan one after the other (ld-container does the work, so that it can be stopped), and shows what they say.
class ProgressDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProgressDialog(Plan plan, QWidget *parent = nullptr);
    ~ProgressDialog() override;

    // Starts the work; the dialog is shown by exec().
    void start();
    // The program that does the work, instead of the one beside this program (the tests give theirs).
    static void setToolDirectory(const QString &directory);
    QString error() const { return m_error; }
    // The last thing the work said (what a check ends with: the summary).
    QString lastLine() const { return m_tail; }

private:
    void runNext();
    void readOutput();
    void appendLog(const QString &line);
    void finished(int code, QProcess::ExitStatus status);
    void fail(const QString &why);
    void cancel();

    Plan m_plan;
    int m_current = -1;
    QProcess *m_process = nullptr;
    QString m_error;
    QString m_pending;
    QString m_said;
    QString m_tail;
    bool m_cancelled = false;
    QLabel *m_status = nullptr;
    QProgressBar *m_bar = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_cancel = nullptr;
};

#endif // PROCESSDIALOG_H
