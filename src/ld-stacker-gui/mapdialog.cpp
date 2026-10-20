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

QStringList MapDialog::companions(const QString &tbcFile)
{
    QStringList files;
    for (const QString &suffix: { QStringLiteral(".db"), QStringLiteral(".json") }) {
        if (QFileInfo::exists(tbcFile + suffix)) files << tbcFile + suffix;
    }
    if (QFileInfo::exists(audioOf(tbcFile))) files << audioOf(tbcFile);
    return files;
}

QString MapDialog::outputFor(const QString &input, bool overwrite)
{
    return overwrite ? temporaryFor(input) : TbcFiles::mappedFileName(input);
}

MapDialog::MapDialog(const QVector<Job> &newJobs, bool overwriteOriginals, QWidget *parent)
    : QDialog(parent), jobs(newJobs), overwrite(overwriteOriginals)
{
    setWindowTitle(tr("Map sources"));
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
        const bool placed = isLdxJob(job) ? finishLdx(job, error) : (!overwrite || replaceOriginal(job, error));
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
        finish(true, overwrite ? tr("Done: the sources are mapped, in place of the captures") : tr("Done: the mapping of each source is written beside it (name_map, or a track of its own in an LDX file)"));
        return;
    }
    const Job &job = jobs[current];
    summary->setText(tr("Mapping %1 of %2: %3").arg(current + 1).arg(jobs.size()).arg(job.title));
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
    // (what a mapping that was not finished left is of no use, and ld-discmap would not know what to do with it)
    if (next.mapping && !isLdxJob(job)) removeOutput(job);
    log(QStringLiteral("%1 %2").arg(QFileInfo(next.program).completeBaseName(), next.arguments.join(QLatin1Char(' '))));
    metadataPercent = -1;
    stageOutput.clear();
    process->start(next.program, next.arguments);
}

// What a job runs
bool MapDialog::plan(const Job &job, QString &error)
{
    stages.clear();
    if (isLdxJob(job)) return planLdx(job, error);
    // The metadata format of the capture is that of its mapping (ld-discmap writes what it reads)
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(job.input);
    const QString format = metadataFile.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ? QStringLiteral("json") : QStringLiteral("db");
    Stage mapping;
    mapping.program = toolPath();
    mapping.arguments = { QStringLiteral("--meta"), format, job.input, outputFor(job.input, overwrite) };
    // (a capture with no audio file beside it is mapped without audio, instead of ld-discmap stopping on it)
    if (!QFileInfo::exists(audioOf(job.input))) mapping.arguments.prepend(QStringLiteral("--no-audio"));
    mapping.output = outputFor(job.input, overwrite);
    mapping.mapping = true;
    stages.append(mapping);
    return true;
}

// A track of an LDX file: its video, metadata and audio are taken out into a folder beside the file, ld-discmap maps them there, and ld-container puts the
// mapping in the file under a temporary name (as a track of its own that says it is the mapping, or, to overwrite, in the place of the track)
bool MapDialog::planLdx(const Job &job, QString &error)
{
    const QString container = toolPath(QStringLiteral("ld-container"));
    if (container.isEmpty()) {
        error = tr("ld-container was not found beside ld-stacker-gui");
        return false;
    }
    QString path;
    qint32 requested;
    LdxCapture::split(job.input, path, requested);
    qint32 video, metadata, audio;
    if (!LdxCapture::captureTracks(job.input, video, metadata, audio) || LdxCapture::isChromaTrack(job.input) || LdxCapture::isCvbs(job.input)) {
        error = tr("%1 is not a TBC capture of an LDX file that can be mapped").arg(job.title);
        return false;
    }
    if (metadata == 0) {
        error = tr("%1 has no metadata in its file: ld-discmap needs them").arg(job.title);
        return false;
    }
    QString name = QFileInfo(path).fileName();
    {
        QVector<qint32> tracks;
        QStringList names;
        QString ignored;
        if (LdxCapture::listCaptures(path, tracks, names, ignored)) {
            const qint32 at = tracks.indexOf(video);
            if (at >= 0 && !names[at].isEmpty()) name = names[at];
        }
    }

    const QString folder = workFolder(job);
    QDir(folder).removeRecursively();
    if (!QDir().mkpath(folder)) {
        error = tr("%1 could not be mapped: the folder %2 could not be made").arg(job.title, QDir::toNativeSeparators(folder));
        return false;
    }
    ldxFile = path;
    ldxResult = folder + QStringLiteral("/result.ldx");
    const QString capture = folder + QStringLiteral("/capture.tbc");
    const QString mapped = folder + QStringLiteral("/capture_map.tbc");
    const auto extract = [&](qint32 id, const QString &file) {
        Stage take;
        take.program = container;
        take.arguments = { QStringLiteral("extract"), path, QStringLiteral("--id"), QString::number(id), QStringLiteral("-o"), file };
        take.output = file;
        stages.append(take);
    };
    extract(video, capture);
    extract(metadata, capture + QStringLiteral(".json"));
    if (audio != 0) extract(audio, folder + QStringLiteral("/capture.pcm"));

    Stage mapping;
    mapping.program = toolPath();
    mapping.arguments = { QStringLiteral("--meta"), QStringLiteral("json"), capture, mapped };
    if (audio == 0) mapping.arguments.prepend(QStringLiteral("--no-audio"));
    mapping.output = mapped;
    mapping.mapping = true;
    stages.append(mapping);

    QString base = path;
    if (overwrite) {
        // The track goes, and the mapping comes in with the name it had
        Stage remove;
        remove.program = container;
        remove.arguments = { QStringLiteral("remove"), path, QStringLiteral("--id"), QString::number(video), QStringLiteral("-o"), folder + QStringLiteral("/removed.ldx") };
        remove.output = folder + QStringLiteral("/removed.ldx");
        stages.append(remove);
        base = remove.output;
    }
    Stage add;
    add.program = container;
    add.arguments = { QStringLiteral("add"), base, mapped, QStringLiteral("-o"), ldxResult, QStringLiteral("--name"), overwrite ? name : tr("%1 (mapped)").arg(name) };
    if (!overwrite) add.arguments << QStringLiteral("--mapped-from") << QString::number(video);
    if (audio != 0) add.arguments << QStringLiteral("--pcm") << folder + QStringLiteral("/capture_map.pcm");
    add.output = ldxResult;
    stages.append(add);
    return true;
}

// The file with the mapping is put in the place of the LDX file (which is set aside first, so that a failure puts it back)
bool MapDialog::finishLdx(const Job &job, QString &error)
{
    const QString aside = ldxFile + QStringLiteral(".bak");
    QFile::remove(aside);
    if (!QFile::rename(ldxFile, aside)) {
        error = tr("%1 could not be mapped: %2 could not be set aside (is it open in another program?)").arg(job.title, QDir::toNativeSeparators(ldxFile));
        return false;
    }
    if (!QFile::rename(ldxResult, ldxFile)) {
        QFile::rename(aside, ldxFile);
        error = tr("%1 could not be mapped: %2 could not be written").arg(job.title, QDir::toNativeSeparators(ldxFile));
        return false;
    }
    QFile::remove(aside);
    QDir(workFolder(job)).removeRecursively();
    if (overwrite) {
        replaced.insert(job.id, LdxCapture::trackName(ldxFile, ldxTrack));
        log(tr("%1 is replaced by its mapping (track %2)").arg(job.title).arg(ldxTrack));
    } else {
        log(tr("The mapping of %1 is the track %2 of the file").arg(job.title).arg(ldxTrack));
    }
    return true;
}

// What is left of a job that did not finish
void MapDialog::cleanUp(const Job &job)
{
    if (isLdxJob(job)) QDir(workFolder(job)).removeRecursively();
    else removeOutput(job);
}

// What ld-discmap wrote for a job, or the start of it: the mapping and its metadata and audio
void MapDialog::removeOutput(const Job &job)
{
    const QString output = outputFor(job.input, overwrite);
    QFile::remove(output);
    for (const QString &file: companions(output)) QFile::remove(file);
}

// The mapping is put in the place of the capture: the capture and its files are set aside first, so that a failure puts them back
bool MapDialog::replaceOriginal(const Job &job, QString &error)
{
    const QString mapped = temporaryFor(job.input);
    // (the files of the mapping, and the files of the capture that they take the place of: the metadata of the other format, if there is some, stays)
    QStringList mappedFiles = companions(mapped);
    mappedFiles.prepend(mapped);
    QStringList originals;
    for (const QString &file: mappedFiles) {
        QString original = file;
        original.replace(QStringLiteral(".mapping-tmp"), QString());
        if (QFileInfo::exists(original)) originals << original;
    }

    QStringList setAside;
    const auto restore = [&]() {
        for (const QString &file: setAside) {
            QFile::remove(file.left(file.length() - 4));
            QFile::rename(file, file.left(file.length() - 4));
        }
    };
    for (const QString &file: originals) {
        const QString aside = file + QStringLiteral(".bak");
        QFile::remove(aside);
        if (!QFile::rename(file, aside)) {
            restore();
            error = tr("%1 could not be replaced: %2 could not be set aside").arg(job.title, QDir::toNativeSeparators(file));
            return false;
        }
        setAside << aside;
    }
    QStringList placed;
    for (const QString &file: mappedFiles) {
        // <name>.mapping-tmp.tbc -> <name>.tbc, and the same for its metadata (.tbc.db or .tbc.json) and its audio (.pcm)
        QString target = file;
        target.replace(QStringLiteral(".mapping-tmp"), QString());
        if (!QFile::rename(file, target)) {
            for (const QString &done: placed) QFile::remove(done);
            restore();
            error = tr("%1 could not be replaced: %2 could not be written").arg(job.title, QDir::toNativeSeparators(target));
            return false;
        }
        placed << target;
    }
    // The capture that was replaced is gone, and the mapping is in its place
    for (const QString &file: setAside) QFile::remove(file);
    log(tr("%1 is replaced by its mapping").arg(job.title));
    return true;
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
