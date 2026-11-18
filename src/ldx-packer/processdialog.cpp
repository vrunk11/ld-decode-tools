/******************************************************************************
 * processdialog.cpp
 * ldx-packer - what to ask before writing, and the window that shows the work going on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "processdialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>

ProcessDialog::ProcessDialog(const QString &archive, QWidget *parent) : QDialog(parent), m_archive(archive)
{
    setWindowTitle(archive.isEmpty() ? tr("Create LDX") : tr("Process"));
    QSettings settings;
    auto *layout = new QVBoxLayout(this);

    if (!archive.isEmpty()) {
        auto *where = new QLabel(tr("The list is written as:"), this);
        m_copy = new QRadioButton(tr("A copy (the opened file stays as it is)"), this);
        m_overwrite = new QRadioButton(tr("Overwrite %1").arg(QFileInfo(archive).fileName()), this);
        m_overwrite->setObjectName(QStringLiteral("overwrite"));
        m_overwrite->setToolTip(tr("The file is written again beside itself and then takes its place. When only new metadata were changed they are added in the file itself, with no copy: it is quick, and nothing else is moved."));
        m_copy->setChecked(true);
        layout->addWidget(where);
        layout->addWidget(m_copy);
        layout->addWidget(m_overwrite);
    }
    auto *pathRow = new QHBoxLayout;
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("outputPath"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    pathRow->addWidget(new QLabel(tr("File"), this));
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);
    connect(browse, &QPushButton::clicked, this, &ProcessDialog::chooseOutput);
    if (m_overwrite) connect(m_overwrite, &QRadioButton::toggled, this, &ProcessDialog::updateOutput);

    m_verify = new QCheckBox(tr("Check the file once it is written (every block is read back)"), this);
    m_verify->setObjectName(QStringLiteral("verifyAfter"));
    m_verify->setChecked(settings.value(QStringLiteral("packVerify"), true).toBool());
    layout->addWidget(m_verify);


    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(archive.isEmpty() ? tr("Create") : tr("Process"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ProcessDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (archive.isEmpty()) {
        m_path->setText(QDir::cleanPath(settings.value(QStringLiteral("lastDir")).toString() + QStringLiteral("/new.ldx")));
    } else {
        updateOutput();
    }
}

void ProcessDialog::updateOutput()
{
    if (m_archive.isEmpty()) return;
    const QFileInfo info(m_archive);
    const bool over = overwrites();
    m_path->setEnabled(!over);
    m_path->setText(over ? m_archive : info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral("-edited.ldx"));
}

bool ProcessDialog::overwrites() const
{
    return m_overwrite && m_overwrite->isChecked();
}


bool ProcessDialog::verifies() const
{
    return m_verify->isChecked();
}

QString ProcessDialog::output() const
{
    return m_path->text().trimmed();
}

void ProcessDialog::chooseOutput()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("Write the .ldx file"), m_path->text(), tr("LDX files (*.ldx)"));
    if (file.isEmpty()) return;
    m_path->setText(file.endsWith(QStringLiteral(".ldx"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".ldx"));
    if (m_copy) m_copy->setChecked(true);
}

void ProcessDialog::accept()
{
    if (output().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose the file to write."));
        return;
    }
    if (!overwrites() && QFileInfo::exists(output())) {
        const auto answer = QMessageBox::question(this, windowTitle(), tr("%1 exists. Replace it?").arg(QFileInfo(output()).fileName()));
        if (answer != QMessageBox::Yes) return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("packVerify"), m_verify->isChecked());
    settings.setValue(QStringLiteral("lastDir"), QFileInfo(output()).absolutePath());
    QDialog::accept();
}

namespace {

QString g_toolDirectory;

QString toolPath(const QString &name)
{
    return QStandardPaths::findExecutable(name, { g_toolDirectory.isEmpty() ? QCoreApplication::applicationDirPath() : g_toolDirectory });
}

}

void ProgressDialog::setToolDirectory(const QString &directory)
{
    g_toolDirectory = directory;
}

ProgressDialog::ProgressDialog(Plan plan, QWidget *parent) : QDialog(parent), m_plan(std::move(plan))
{
    setWindowTitle(tr("Writing %1").arg(QFileInfo(m_plan.output).fileName()));
    setMinimumWidth(560);
    auto *layout = new QVBoxLayout(this);
    m_status = new QLabel(tr("Starting…"), this);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(180);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    // (nothing to show until the work says something: just the bar, as long as it has nothing else to say)
    m_log->hide();
    m_cancel = new QPushButton(tr("Cancel"), this);
    auto *row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(m_cancel);
    layout->addWidget(m_status);
    layout->addWidget(m_bar);
    layout->addWidget(m_log, 1);
    layout->addLayout(row);
    connect(m_cancel, &QPushButton::clicked, this, &ProgressDialog::cancel);
}

ProgressDialog::~ProgressDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(5000);
    }
    // (the files made on the way go with their folder when the plan does)
}

void ProgressDialog::start()
{
    if (!m_plan.directory || !m_plan.directory->isValid()) {
        m_error = tr("The folder of %1 cannot be written to.").arg(QDir::toNativeSeparators(m_plan.output));
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
        return;
    }
    m_bar->setRange(0, static_cast<int>(m_plan.steps.size()));
    m_bar->setValue(0);
    runNext();
}

void ProgressDialog::runNext()
{
    ++m_current;
    m_bar->setValue(m_current);
    if (m_current >= m_plan.steps.size()) {
        accept();
        return;
    }
    const Step &step = m_plan.steps.at(m_current);
    m_status->setText(tr("%1 (step %2 of %3)").arg(step.description).arg(m_current + 1).arg(m_plan.steps.size()));
    appendLog(QStringLiteral("== ") + step.description);
    if (step.action) {
        QString why;
        if (!step.action(&why)) {
            fail(why);
            return;
        }
        runNext();
        return;
    }
    const QString program = toolPath(step.program);
    if (program.isEmpty()) {
        fail(tr("%1 was not found beside this program.").arg(step.program));
        return;
    }
    m_said.clear();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    // (what ld-container writes in the history of the file says that ldx-packer asked for it)
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LDX_HISTORY_VIA"), QStringLiteral("ldx-packer"));
    m_process->setProcessEnvironment(environment);
    connect(m_process, &QProcess::readyRead, this, &ProgressDialog::readOutput);
    connect(m_process, &QProcess::finished, this, &ProgressDialog::finished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && !m_cancelled) fail(tr("The program could not be started: %1").arg(m_process->errorString()));
    });
    m_process->start(program, step.arguments());
}

// What the work says goes in a log that is shown when it has something in it, and the window grows to take it.
void ProgressDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
    if (m_log->isHidden()) {
        m_log->show();
        QMetaObject::invokeMethod(this, [this] { resize(width(), std::max(height(), sizeHint().height())); }, Qt::QueuedConnection);
    }
}

void ProgressDialog::readOutput()
{
    m_pending += QString::fromLocal8Bit(m_process->readAll());
    for (;;) {
        const int cut = m_pending.indexOf(QRegularExpression(QStringLiteral("[\\r\\n]")));
        if (cut < 0) break;
        const QString line = m_pending.left(cut);
        m_pending.remove(0, cut + 1);
        if (line.trimmed().isEmpty()) continue;
        appendLog(line);
        m_said += line + QLatin1Char('\n');
        m_tail = line;
    }
}

void ProgressDialog::finished(int code, QProcess::ExitStatus status)
{
    readOutput();
    if (!m_pending.trimmed().isEmpty()) {
        appendLog(m_pending.trimmed());
        m_said += m_pending;
        m_tail = m_pending.trimmed();
        m_pending.clear();
    }
    m_process->deleteLater();
    m_process = nullptr;
    if (m_cancelled) return;
    if (status != QProcess::NormalExit || code != 0) {
        fail(m_tail.isEmpty() ? tr("The work stopped (exit code %1).").arg(code) : m_tail);
        return;
    }
    const Step &step = m_plan.steps.at(m_current);
    if (step.after) step.after(m_said);
    runNext();
}

void ProgressDialog::fail(const QString &why)
{
    m_error = why;
    reject();
}

void ProgressDialog::cancel()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        reject();
        return;
    }
    m_cancelled = true;
    m_cancel->setEnabled(false);
    m_status->setText(tr("Stopping…"));
    m_process->disconnect(this);
    m_process->kill();
    m_process->waitForFinished(5000);
    reject();
}
