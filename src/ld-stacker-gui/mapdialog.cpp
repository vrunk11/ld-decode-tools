/******************************************************************************
 * mapdialog.cpp
 * ld-stacker-gui - mapping sources with ld-discmap
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mapdialog.h"

#include "ldxcapture.h"
#include "lddecodemetadata.h"
#include "tbcfiles.h"
#include "framemap.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVBoxLayout>

namespace {

// The temporary name of the capture that is being overwritten: beside it, and ending in .tbc so that ld-discmap names its other files from it
QString temporaryFor(const QString &input)
{
    return input.left(input.length() - 4) + QStringLiteral(".mapping-tmp.tbc");
}

// <name>.tbc -> <name>.pcm (the analogue audio ld-discmap reads beside the capture and writes beside the mapping)
QString audioOf(const QString &tbcFile)
{
    return tbcFile.left(tbcFile.length() - 4) + QStringLiteral(".pcm");
}

} // namespace

MapDialog::MapDialog(const QVector<Job> &newJobs, bool overwriteOriginals, QWidget *parent, Task newTask)
    : QDialog(parent), jobs(newJobs), overwrite(overwriteOriginals), task(newTask)
{
    setWindowTitle(task == UpdateSnr ? tr("Update the SNR") : tr("Map sources"));
    resize(640, 360);
    QVBoxLayout *layout = new QVBoxLayout(this);
    summary = new QLabel;
    summary->setWordWrap(true);
    layout->addWidget(summary);
    progress = new QProgressBar;
    progress->setRange(0, jobs.size());
    progress->setFormat(tr("%v of %m"));
    layout->addWidget(progress);
    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(5000);
    layout->addWidget(logView, 1);
    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    cancelButton = new QPushButton(tr("Cancel"));
    closeButton = new QPushButton(tr("Close"));
    closeButton->setEnabled(false);
    buttons->addWidget(cancelButton);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, [this]() { allDone ? accept() : reject(); });
    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        if (finished) return;
        cancelled = true;
        if (process && process->state() != QProcess::NotRunning) process->kill();
        else finish(false, tr("Cancelled"));
    });

    process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, this, [this]() {
        const QString text = QString::fromLocal8Bit(process->readAll());
        log(text);
        stageOutput += text;
        if (text.contains(QLatin1String("unmappable frames: they are kept"))) unmappableSeen = true;
        // ("Created metadata for frame 120 of 731": the last part, which is the last few per cent)
        static const QRegularExpression count(QStringLiteral("Created metadata for frame (\\d+) of (\\d+)"));
        QRegularExpressionMatchIterator matches = count.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const qint32 total = match.captured(2).toInt();
            if (total > 0) metadataPercent = 94 + static_cast<qint32>(6.0 * match.captured(1).toInt() / total);
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (current >= 0 && current < jobs.size()) emit jobFinished(jobs[current].id, false);
            finish(false, tr("ld-discmap could not be started"));
        }
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (finished || current < 0) return;
        const Job &job = jobs[current];
        if (cancelled) {
            progressTimer->stop();
            cleanUp(job);
            emit jobFinished(job.id, false);
            finish(false, tr("Cancelled"));
            return;
        }
        const Stage &ran = stages[stage];
        if (status != QProcess::NormalExit || exitCode != 0 || (!ran.output.isEmpty() && !QFileInfo::exists(ran.output))) {
            progressTimer->stop();
            cleanUp(job);
            emit jobFinished(job.id, false);
            finish(false, tr("%1 did not finish for %2 (exit code %3)").arg(QFileInfo(ran.program).completeBaseName(), job.title).arg(exitCode));
            return;
        }
        // (ld-container says which track it gave the mapping)
        static const QRegularExpression added(QStringLiteral("as track (\\d+)"));
        const QRegularExpressionMatch match = added.match(stageOutput);
        if (match.hasMatch()) ldxTrack = match.captured(1).toInt();
        if (stage + 1 < stages.size()) {
            stage++;
            startStage();
            return;
        }
        progressTimer->stop();
        QString error;
        const bool placed = task == UpdateSnr ? finishSnr(job, error) : finishMap(job, error);
        if (!placed) {
            cleanUp(job);
            emit jobFinished(job.id, false);
            finish(false, error);
            return;
        }
        if (unmappableSeen) log(tr("%1: some frames could not be numbered: they were kept, given a number and marked as dropouts (the stack does not use them)").arg(job.title));
        done << job.id;
        progress->setValue(done.size());
        emit jobProgress(job.id, 100);
        emit jobFinished(job.id, true);
        runNext();
    });

    // The progress: the size of what is written, against the size of the capture (ld-discmap says nothing while it writes the video), then its count
    // of the frames whose metadata it writes
    progressTimer = new QTimer(this);
    progressTimer->setInterval(400);
    connect(progressTimer, &QTimer::timeout, this, &MapDialog::reportProgress);
}

void MapDialog::start()
{
    if (toolPath().isEmpty()) {
        finish(false, tr("ld-discmap was not found beside ld-stacker-gui"));
        return;
    }
    runNext();
}

void MapDialog::reportProgress()
{
    if (current < 0 || current >= jobs.size() || finished) return;
    const Job &job = jobs[current];
    qint32 percent = -1;
    // (only ld-discmap says how far it is; the other programs of a job go back and forth)
    if (stage >= stages.size() || !stages[stage].mapping) {
        emit jobProgress(job.id, -1);
        return;
    }
    if (metadataPercent >= 0) {
        percent = metadataPercent;
    } else {
        const qint64 written = QFileInfo(stages[stage].output).size();
        const qint64 total = QFileInfo(stages[stage].arguments.value(stages[stage].arguments.size() - 2)).size();
        if (written > 0 && total > 0) percent = static_cast<qint32>(qMin<qint64>(94, written * 94 / total));
    }
    emit jobProgress(job.id, percent);
}

QString MapDialog::toolPath(const QString &name) const
{
    return QStandardPaths::findExecutable(name, { QCoreApplication::applicationDirPath() });
}

bool MapDialog::isLdxJob(const Job &job)
{
    return LdxCapture::isLdxName(job.input);
}

// The folder beside an LDX file where the capture is taken out, mapped and put together again
QString MapDialog::workFolder(const Job &job) const
{
    QString path;
    qint32 track;
    LdxCapture::split(job.input, path, track);
    const QFileInfo info(path);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".mapping-tmp");
}

void MapDialog::log(const QString &text)
{
    logView->appendPlainText(text.trimmed());
}

void MapDialog::runNext()
{
    current++;
    if (current >= jobs.size()) {
        if (task == UpdateSnr) {
            finish(true, tr("Done: the SNR is written in the metadata"));
            return;
        }
        finish(true, tr("Done: the map of each source is in the session"));
        return;
    }
    const Job &job = jobs[current];
    summary->setText((task == UpdateSnr ? tr("Measuring the SNR %1 of %2: %3") : tr("Mapping %1 of %2: %3")).arg(current + 1).arg(jobs.size()).arg(job.title));
    QString error;
    if (!plan(job, error)) {
        emit jobFinished(job.id, false);
        finish(false, error);
        return;
    }
    stage = 0;
    ldxTrack = 0;
    unmappableSeen = false;
    emit jobStarted(job.id);
    emit jobProgress(job.id, -1);
    progressTimer->start();
    startStage();
}

void MapDialog::startStage()
{
    const Job &job = jobs[current];
    const Stage &next = stages[stage];
    log(QStringLiteral("%1 %2").arg(QFileInfo(next.program).completeBaseName(), next.arguments.join(QLatin1Char(' '))));
    metadataPercent = -1;
    stageOutput.clear();
    process->start(next.program, next.arguments);
}

// What a job runs
bool MapDialog::plan(const Job &job, QString &error)
{
    stages.clear();
    if (task == UpdateSnr) return planSnr(job, error);

    // The map of the capture: ld-discmap works it out and writes it as JSON, and nothing else (the video is not written again). A capture of an LDX file
    // is read from the file by ld-discmap as it is.
    const QString tool = toolPath();
    if (tool.isEmpty()) {
        error = tr("ld-discmap was not found beside ld-stacker-gui");
        return false;
    }
    QTemporaryFile holder(QDir::tempPath() + QStringLiteral("/ld-stacker-map-XXXXXX.json"));
    holder.setAutoRemove(false);
    if (!holder.open()) {
        error = tr("%1 could not be mapped: a file for the map could not be made").arg(job.title);
        return false;
    }
    mapFile = holder.fileName();
    holder.close();
    QFile::remove(mapFile);

    Stage mapping;
    mapping.program = tool;
    mapping.arguments = { QStringLiteral("--maponly"), QStringLiteral("--map-file"), mapFile };
    if (!isLdxJob(job)) {
        // The metadata format of the capture (ld-discmap reads what is there)
        const QString metadataFile = LdDecodeMetaData::findMetadataFile(job.input);
        mapping.arguments << QStringLiteral("--meta") << (metadataFile.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ? QStringLiteral("json") : QStringLiteral("db"));
        // (a capture with no audio file beside it is mapped without audio, instead of ld-discmap stopping on it)
        if (!QFileInfo::exists(audioOf(job.input))) mapping.arguments.prepend(QStringLiteral("--no-audio"));
    }
    mapping.arguments << job.input;
    mapping.output = mapFile;
    stages.append(mapping);
    return true;
}

// ld-discmap is done: the map it wrote is kept for the source
bool MapDialog::finishMap(const Job &job, QString &error)
{
    QFile file(mapFile);
    if (!file.open(QIODevice::ReadOnly)) {
        error = tr("The map of %1 could not be read back").arg(job.title);
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    QFile::remove(mapFile);
    if (bytes.isEmpty()) {
        error = tr("ld-discmap wrote no map for %1").arg(job.title);
        return false;
    }
    maps.insert(job.id, bytes);

    if (saveInSources) {
        FrameMap map;
        QString saveError;
        if (!FrameMap::fromJsonBytes(bytes, map, saveError) || !LdDecodeMetaData::saveDiscMap(job.input, &map, saveError)) {
            log(tr("The map of %1 could not be kept in its metadata (%2): it stays in the session").arg(job.title, saveError));
        } else {
            saved << job.id;
            log(tr("The map of %1 is kept in its metadata").arg(job.title));
        }
    }
    return true;
}

// The SNR of a capture measured again by ld-process-vits, which writes it in the metadata (a track of an LDX file is taken out first, and its new metadata are
// put back in the file as a block)
bool MapDialog::planSnr(const Job &job, QString &error)
{
    const QString vits = toolPath(QStringLiteral("ld-process-vits"));
    if (vits.isEmpty()) {
        error = tr("ld-process-vits was not found beside ld-stacker-gui");
        return false;
    }
    Stage measure;
    measure.program = vits;
    if (!isLdxJob(job)) {
        const QString metadataFile = LdDecodeMetaData::findMetadataFile(job.input);
        measure.arguments = { QStringLiteral("--meta"), metadataFile.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ? QStringLiteral("json") : QStringLiteral("db"), job.input };
        stages.append(measure);
        return true;
    }
    const QString container = toolPath(QStringLiteral("ld-container"));
    if (container.isEmpty()) {
        error = tr("ld-container was not found beside ld-stacker-gui");
        return false;
    }
    qint32 video, metadata, audio;
    if (!LdxCapture::captureTracks(job.input, video, metadata, audio) || metadata == 0 || LdxCapture::isChromaTrack(job.input) || LdxCapture::isCvbs(job.input)) {
        error = tr("%1 is not a TBC capture of an LDX file whose SNR can be measured").arg(job.title);
        return false;
    }
    QString path;
    qint32 requested;
    LdxCapture::split(job.input, path, requested);
    const QString folder = workFolder(job);
    QDir(folder).removeRecursively();
    if (!QDir().mkpath(folder)) {
        error = tr("%1: the folder %2 could not be made").arg(job.title, QDir::toNativeSeparators(folder));
        return false;
    }
    const QString capture = folder + QStringLiteral("/capture.tbc");
    for (const QPair<qint32, QString> &take: { qMakePair(video, capture), qMakePair(metadata, capture + QStringLiteral(".json")) }) {
        Stage extract;
        extract.program = container;
        extract.arguments = { QStringLiteral("extract"), path, QStringLiteral("--id"), QString::number(take.first), QStringLiteral("-o"), take.second };
        extract.output = take.second;
        stages.append(extract);
    }
    measure.arguments = { QStringLiteral("--meta"), QStringLiteral("json"), capture };
    stages.append(measure);
    return true;
}

bool MapDialog::finishSnr(const Job &job, QString &error)
{
    if (!isLdxJob(job)) {
        log(tr("The SNR of %1 is written in its metadata").arg(job.title));
        return true;
    }
    QFile file(workFolder(job) + QStringLiteral("/capture.tbc.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        error = tr("%1: the new metadata could not be read").arg(job.title);
        return false;
    }
    const QByteArray json = file.readAll();
    file.close();
    QString writeError;
    if (!LdxCapture::writeMetadata(job.input, json, writeError)) {
        error = tr("%1: the metadata could not be written in the file (%2)").arg(job.title, writeError);
        return false;
    }
    QDir(workFolder(job)).removeRecursively();
    log(tr("The SNR of %1 is written in the metadata of the file").arg(job.title));
    return true;
}

// What is left of a job that did not finish
void MapDialog::cleanUp(const Job &job)
{
    if (task == Map) {
        if (!mapFile.isEmpty()) QFile::remove(mapFile);
    } else if (task == UpdateSnr) {
        if (isLdxJob(job)) QDir(workFolder(job)).removeRecursively();
    }
}

void MapDialog::finish(bool success, const QString &message)
{
    finished = true;
    allDone = success;
    summary->setText(message);
    log(message);
    cancelButton->setEnabled(false);
    closeButton->setEnabled(true);
    if (progressTimer) progressTimer->stop();
    emit allFinished(success);
}
