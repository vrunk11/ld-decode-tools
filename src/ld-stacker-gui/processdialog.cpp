/******************************************************************************
 * processdialog.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "processdialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "cvbs.h"
#include "lddecodemetadata.h"

ProcessDialog::ProcessDialog(const StackProject &stackProject, const QString &suggestedOutput, QWidget *parent)
    : QDialog(parent), project(stackProject)
{
    setWindowTitle(tr("Process"));
    resize(640, 460);

    QVBoxLayout *layout = new QVBoxLayout(this);

    qint32 enabled = 0;
    for (const StackSource &source: project.sources) if (source.enabled) enabled++;
    summary = new QLabel(tr("Stacks the %1 sources that are on, lined up by their offsets, to a file.").arg(enabled));
    summary->setWordWrap(true);
    layout->addWidget(summary);

    QFormLayout *form = new QFormLayout;
    QHBoxLayout *outputRow = new QHBoxLayout;
    outputEdit = new QLineEdit(suggestedOutput);
    QPushButton *browseButton = new QPushButton(tr("Browse..."));
    outputRow->addWidget(outputEdit, 1);
    outputRow->addWidget(browseButton);
    form->addRow(tr("Save as:"), outputRow);

    metadataCombo = new QComboBox;
    metadataCombo->addItem(tr("SQLite (.db)"), "db");
    metadataCombo->addItem(tr("JSON (.json)"), "json");
    metadataCombo->setCurrentIndex(project.metadataFormat == "json" ? 1 : 0);
    form->addRow(tr("Metadata:"), metadataCombo);

    codecCombo = new QComboBox;
    codecCombo->addItem(tr("Not compressed"), "raw");
    codecCombo->addItem(tr("Lossless (FLAC for TBC and audio, FFV1 for CVBS)"), "auto");
    codecCombo->setToolTip(tr("For an .ldx file: how the video and the audio are kept in it"));
    form->addRow(tr("In an .ldx file:"), codecCombo);

    QLabel *formatNote = new QLabel(tr("The video is in the format the file name gives: .tbc, or .cvbs. A format other than that of "
                                       "the sources is made with ld-tbc-to-cvbs or ld-cvbs-to-tbc after the stacking, and a metadata "
                                       "format other than the one the stacking writes with ld-sqlite-to-json or ld-json-converter."));
    formatNote->setWordWrap(true);
    formatNote->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), formatNote);
    layout->addLayout(form);

    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(5000);
    layout->addWidget(logView, 1);

    progress = new QProgressBar;
    progress->setRange(0, 1);
    progress->setValue(0);
    progress->setTextVisible(false);
    layout->addWidget(progress);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    startButton = new QPushButton(tr("Start"));
    startButton->setDefault(true);
    cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setEnabled(false);
    closeButton = new QPushButton(tr("Close"));
    buttons->addWidget(startButton);
    buttons->addWidget(cancelButton);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(browseButton, &QPushButton::clicked, this, &ProcessDialog::browse);
    connect(startButton, &QPushButton::clicked, this, &ProcessDialog::start);
    connect(cancelButton, &QPushButton::clicked, this, &ProcessDialog::cancel);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
}

bool ProcessDialog::isCvbs(const QString &file)
{
    // (a .stack source has the format of its first source)
    return Cvbs::isDataFileName(StackProject::referenceFile(file));
}

QString ProcessDialog::metadataFile(const QString &video, bool json)
{
    return LdDecodeMetaData::metadataFileName(video, json ? LdDecodeMetaData::MetadataFormat::Json
                                                          : LdDecodeMetaData::MetadataFormat::Sqlite);
}

QString ProcessDialog::toolPath(const QString &name) const
{
    return QStandardPaths::findExecutable(name, { QCoreApplication::applicationDirPath() });
}

void ProcessDialog::log(const QString &text)
{
    logView->appendPlainText(text);
}

void ProcessDialog::setRunning(bool running)
{
    startButton->setEnabled(!running);
    cancelButton->setEnabled(running);
    outputEdit->setEnabled(!running);
    metadataCombo->setEnabled(!running);
    codecCombo->setEnabled(!running);
    progress->setRange(0, running ? 0 : 1);
    progress->setValue(0);
}

void ProcessDialog::browse()
{
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save the stack as"), outputEdit->text(),
                                                        tr("TBC (*.tbc);;CVBS (*.cvbs);;LDX (*.ldx)"), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!chosen.isEmpty()) outputEdit->setText(chosen);
}

void ProcessDialog::start()
{
    // The sources that are on
    QStringList inputs;
    for (const StackSource &source: project.sources) if (source.enabled) inputs.append(source.reference());
    if (inputs.size() < 2) {
        QMessageBox::warning(this, tr("Process"), tr("At least 2 sources must be on to stack them."));
        return;
    }

    // They share one container: TBC or CVBS
    const bool inputCvbs = isCvbs(inputs.first());
    for (const QString &input: inputs) {
        if (isCvbs(input) != inputCvbs) {
            QMessageBox::warning(this, tr("Process"), tr("The sources are not all of one format (TBC or CVBS): they cannot be stacked together."));
            return;
        }
    }

    // Where it goes, and in which video format
    finalVideo = QDir::cleanPath(QFileInfo(outputEdit->text().trimmed()).absoluteFilePath());
    const QString suffix = QFileInfo(finalVideo).suffix().toLower();
    if (outputEdit->text().trimmed().isEmpty() || (suffix != "tbc" && suffix != "cvbs" && suffix != "ldx")) {
        QMessageBox::warning(this, tr("Process"), tr("Name the file to save with the extension of the format: .tbc, .cvbs or .ldx."));
        return;
    }
    const bool toLdx = suffix == "ldx";
    const bool wantCvbs = toLdx ? inputCvbs : suffix == "cvbs";
    const bool wantJson = metadataCombo->currentData().toString() == "json";
    for (const QString &input: inputs) {
        if (QFileInfo(input).absoluteFilePath() == QFileInfo(finalVideo).absoluteFilePath()) {
            QMessageBox::warning(this, tr("Process"), tr("The file to save is one of the sources."));
            return;
        }
    }
    if (QFileInfo::exists(finalVideo)) {
        QMessageBox::warning(this, tr("Process"), tr("%1 already exists: the stacking does not overwrite files. Choose another name.").arg(finalVideo));
        return;
    }

    // The tools needed
    const bool convertVideo = wantCvbs != inputCvbs;
    QStringList needed = { "ld-disc-stacker" };
    if (toLdx) needed.append("ld-container");
    if (convertVideo) needed.append(inputCvbs ? "ld-cvbs-to-tbc" : "ld-tbc-to-cvbs");
    for (const QString &tool: needed) {
        if (toolPath(tool).isEmpty()) {
            QMessageBox::warning(this, tr("Process"), tr("%1 was not found beside this program.").arg(tool));
            return;
        }
    }

    if (toLdx) {
        startLdx(finalVideo);
        return;
    }

    // The session the stacking is given
    sessionDirectory = std::make_unique<QTemporaryDir>();
    if (!sessionDirectory->isValid()) {
        QMessageBox::warning(this, tr("Process"), tr("Could not make a folder for the session."));
        return;
    }
    const QString sessionFile = sessionDirectory->filePath("session.stack");

    // Stacking writes to the final file, or, when the video format changes after, to one on the way
    const QString inputSuffix = inputCvbs ? ".cvbs" : ".tbc";
    const QString stacked = convertVideo ? QFileInfo(finalVideo).absoluteDir().absoluteFilePath(QFileInfo(finalVideo).completeBaseName() + ".stacking" + inputSuffix)
                                         : finalVideo;
    if (convertVideo && QFileInfo::exists(stacked)) {
        QMessageBox::warning(this, tr("Process"), tr("%1 is in the way: remove it first.").arg(stacked));
        return;
    }

    StackProject session = project;
    session.outputFile = stacked;
    QString error;
    if (!session.save(sessionFile, error)) {
        QMessageBox::warning(this, tr("Process"), error);
        return;
    }

    // The steps
    steps.clear();
    created.clear();
    temporary.clear();
    cancelled = false;

    const QString stackerMetadataDb = metadataFile(stacked, false);
    const QString stackerMetadataJson = metadataFile(stacked, true);
    const bool hadDb = QFileInfo::exists(stackerMetadataDb), hadJson = QFileInfo::exists(stackerMetadataJson);

    steps.append({ tr("Stacking %1 sources").arg(inputs.size()), toolPath("ld-disc-stacker"), { sessionFile }, [=]() {
        created.append(stacked);
        if (!hadDb && QFileInfo::exists(stackerMetadataDb)) created.append(stackerMetadataDb);
        if (!hadJson && QFileInfo::exists(stackerMetadataJson)) created.append(stackerMetadataJson);
        if (convertVideo) {
            temporary.append(stacked);
            temporary.append(stackerMetadataDb);
            temporary.append(stackerMetadataJson);
            // (a CVBS file has the files of decode-orc beside it)
            if (inputCvbs) {
                temporary.append(Cvbs::metaFileName(stacked));
                temporary.append(Cvbs::dropoutsFileName(stacked));
            }
        }
    } });

    if (convertVideo) {
        // The metadata the stacking wrote is the one the conversion reads
        const QString stackedFormat = QFileInfo::exists(stackerMetadataDb) || !hadDb ? QString("db") : QString("json");
        Q_UNUSED(stackedFormat);
        QStringList arguments;
        if (inputCvbs) arguments << "--meta" << (wantJson ? "json" : "db");
        arguments << stacked << finalVideo;
        steps.append({ inputCvbs ? tr("Converting the video from CVBS to TBC") : tr("Converting the video from TBC to CVBS"),
                       toolPath(inputCvbs ? "ld-cvbs-to-tbc" : "ld-tbc-to-cvbs"), arguments, [=]() {
            created.append(finalVideo);
            for (const QString &extra: { metadataFile(finalVideo, false), metadataFile(finalVideo, true) }) {
                if (QFileInfo::exists(extra)) created.append(extra);
            }
        } });
    }

    // The metadata in the format asked for: after all this it is in one of the two formats, and
    // if that is not the one wanted it is converted and the other one is not kept
    steps.append({ tr("Putting the metadata in the format asked for"), QString(), {}, nullptr });

    log(tr("Saving to %1").arg(finalVideo));
    setRunning(true);
    currentStep = -1;
    runNextStep();
}

// An .ldx file: asks what it holds. The sources as tracks of the file, with the session that stacks them (the file is read as the stack of its sources, and
// nothing is stacked now); or the stack itself as a capture, which ld-disc-stacker sends as an LDX stream to a file and ld-container writes again with its index.
void ProcessDialog::startLdx(const QString &output)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Save as an LDX file"));
    box.setText(tr("What does the LDX file hold?"));
    box.setInformativeText(tr("The sources and the session: the sources are brought into the file as tracks (with their metadata and audio), with the session (.stack) that stacks them. "
                              "The tools read the file as the stack of its sources, and nothing is stacked now.\n\n"
                              "Only the stack: the sources are stacked now, and the file holds the stacked capture."));
    QPushButton *withSources = box.addButton(tr("The sources and the session"), QMessageBox::AcceptRole);
    QPushButton *onlyStack = box.addButton(tr("Only the stack"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(onlyStack);
    box.exec();
    if (box.clickedButton() != withSources && box.clickedButton() != onlyStack) return;
    const bool sourcesAndSession = box.clickedButton() == withSources;
    const QString codec = codecCombo->currentData().toString();

    sessionDirectory = std::make_unique<QTemporaryDir>();
    if (!sessionDirectory->isValid()) {
        QMessageBox::warning(this, tr("Process"), tr("Could not make a folder for the session."));
        return;
    }
    steps.clear();
    created.clear();
    temporary.clear();
    cancelled = false;
    QString error;

    if (sourcesAndSession) {
        // The first source that is a file of its own is the one the LDX file begins with (a track 1); the others are brought in by the session
        qint32 first = -1;
        for (qint32 i = 0; i < project.sources.size() && first < 0; i++) {
            const StackSource &source = project.sources[i];
            const QString suffix = QFileInfo(source.file).suffix().toLower();
            if (source.track < 1 && (suffix == "tbc" || suffix == "cvbs")) first = i;
        }
        if (first < 0) {
            QMessageBox::warning(this, tr("Process"), tr("None of the sources is a .tbc or .cvbs file to put in the LDX file (the tracks of other LDX files stay links)."));
            return;
        }
        const auto audioOf = [](const StackSource &source) {
            const QFileInfo info(source.file);
            const QString pcm = info.dir().filePath(info.completeBaseName() + ".pcm");
            return info.suffix().toLower() == "cvbs" || !QFileInfo::exists(pcm) ? QString() : pcm;
        };
        const StackSource &begin = project.sources[first];
        const QString base = sessionDirectory->filePath("base.ldx");
        QStringList pack = { "pack", begin.file, "-o", base, "--name", begin.label.isEmpty() ? QFileInfo(begin.file).fileName() : begin.label, "--codec", codec };
        if (!audioOf(begin).isEmpty()) pack << "--pcm" << audioOf(begin);
        steps.append({ tr("Putting %1 in the file as track 1").arg(QFileInfo(begin.file).fileName()), toolPath("ld-container"), pack, nullptr });

        StackProject session = project;
        session.outputFile.clear();
        session.sources[first].file.clear();
        session.sources[first].track = 1;
        const QString sessionFile = sessionDirectory->filePath("session.stack");
        if (!session.save(sessionFile, error)) {
            QMessageBox::warning(this, tr("Process"), error);
            return;
        }
        steps.append({ tr("Bringing the other sources in, with the session"), toolPath("ld-container"),
                       { "stack", base, sessionFile, "-o", output, "--embed", "--codec", codec }, [=]() { created.append(output); } });
    } else {
        StackProject session = project;
        const QString stream = output + QStringLiteral(".stacking");
        if (QFileInfo::exists(stream)) {
            QMessageBox::warning(this, tr("Process"), tr("%1 is in the way: remove it first.").arg(stream));
            return;
        }
        session.outputFile = output;
        const QString sessionFile = sessionDirectory->filePath("session.stack");
        if (!session.save(sessionFile, error)) {
            QMessageBox::warning(this, tr("Process"), error);
            return;
        }
        steps.append({ tr("Stacking %1 sources").arg(project.sources.size()), toolPath("ld-disc-stacker"), { sessionFile, "-" }, nullptr, stream });
        steps.append({ tr("Writing the LDX file"), toolPath("ld-container"), { "recompress", stream, "--codec", codec, "-o", output }, [=]() { created.append(output); } });
        // (what is on the way is taken away if the stacking does not finish)
        temporary.append(stream);
    }

    log(tr("Saving to %1").arg(output));
    setRunning(true);
    currentStep = -1;
    runNextStep();
}

void ProcessDialog::runNextStep()
{
    currentStep++;
    if (currentStep >= steps.size()) {
        // Done: take away what was only on the way
        for (const QString &file: temporary) {
            if (QFileInfo::exists(file)) QFile::remove(file);
        }
        finish(true, tr("Done: %1").arg(finalVideo));
        return;
    }

    Step &step = steps[currentStep];
    log(QString());
    log(QString("== %1").arg(step.description));

    if (step.program.isEmpty()) {
        // The last step: the metadata of the final file in the format wanted
        const bool wantJson = metadataCombo->currentData().toString() == "json";
        const QString db = metadataFile(finalVideo, false);
        const QString json = metadataFile(finalVideo, true);
        const bool haveDb = QFileInfo::exists(db), haveJson = QFileInfo::exists(json);

        if (!haveDb && !haveJson) {
            finish(false, tr("The stacking wrote no metadata for %1").arg(finalVideo));
            return;
        }
        if ((wantJson && haveJson) || (!wantJson && haveDb)) {
            log(tr("The metadata is in that format already."));
            // An older file of the other format that this run made would now be out of date
            const QString other = wantJson ? db : json;
            if (created.contains(other) && QFileInfo::exists(other)) {
                QFile::remove(other);
                log(tr("Removed %1, which this run made in the other format.").arg(other));
            }
            runNextStep();
            return;
        }

        const QString tool = wantJson ? "ld-sqlite-to-json" : "ld-json-converter";
        if (toolPath(tool).isEmpty()) {
            finish(false, tr("%1 was not found beside this program.").arg(tool));
            return;
        }
        step.program = toolPath(tool);
        const QString from = wantJson ? db : json;
        const QString to = wantJson ? json : db;
        step.arguments = wantJson ? QStringList { "--input-sqlite", from, "--output-json", to }
                                  : QStringList { "--input-json", from, "--output-sqlite", to };
        step.after = [=]() {
            created.append(to);
            // What this run made in the other format would be out of date beside the new one
            if (created.contains(from)) QFile::remove(from);
        };
    }

    process = new QProcess(this);
    if (step.stdoutFile.isEmpty()) {
        process->setProcessChannelMode(QProcess::MergedChannels);
        connect(process, &QProcess::readyRead, this, [this]() {
            const QString text = QString::fromLocal8Bit(process->readAll()).trimmed();
            if (!text.isEmpty()) log(text);
        });
    } else {
        // (an LDX stream goes to the file, and what the program says goes to the log)
        process->setStandardOutputFile(step.stdoutFile);
        connect(process, &QProcess::readyReadStandardError, this, [this]() {
            const QString text = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            if (!text.isEmpty()) log(text);
        });
    }
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !cancelled) finish(false, tr("%1 could not be started").arg(steps[currentStep].program));
    });
    connect(process, &QProcess::finished, this, &ProcessDialog::stepFinished);
    process->start(step.program, step.arguments);
}

void ProcessDialog::stepFinished(int exitCode, QProcess::ExitStatus status)
{
    if (process) {
        process->deleteLater();
        process = nullptr;
    }
    if (cancelled) return;

    if (status != QProcess::NormalExit || exitCode != 0) {
        finish(false, tr("%1 failed (exit code %2)").arg(steps[currentStep].description).arg(exitCode));
        return;
    }
    if (steps[currentStep].after) steps[currentStep].after();
    runNextStep();
}

void ProcessDialog::cancel()
{
    cancelled = true;
    if (process) {
        process->kill();
        process->waitForFinished(3000);
        process->deleteLater();
        process = nullptr;
    }
    finish(false, tr("Cancelled"));
}

void ProcessDialog::finish(bool success, const QString &message)
{
    if (!success) {
        // A stack that did not finish is not left behind
        for (const QString &file: created + temporary) {
            if (QFileInfo::exists(file)) QFile::remove(file);
        }
    }
    log(QString());
    log(message);
    setRunning(false);
    if (success) {
        progress->setRange(0, 1);
        progress->setValue(1);
    }
}
