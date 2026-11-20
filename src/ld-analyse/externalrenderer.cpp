/******************************************************************************
 * externalrenderer.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "externalrenderer.h"
#include "i420.h"

#include <algorithm>

ExternalRenderer::ExternalRenderer(QObject *parent)
    : QObject(parent)
{
    commandLine = defaultCommand();
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &ExternalRenderer::send);
}

ExternalRenderer::~ExternalRenderer()
{
    stop();
}

QString ExternalRenderer::defaultCommand()
{
    const QString arguments = " - --demux=rawvideo --rawvid-width={width} --rawvid-height={height} --rawvid-fps={fps} --rawvid-chroma=I420 --no-audio --aspect-ratio={aspect}";
#ifdef Q_OS_WIN
    return "\"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\"" + arguments;
#elif defined(Q_OS_MACOS)
    return "/Applications/VLC.app/Contents/MacOS/VLC" + arguments;
#else
    return "vlc" + arguments;
#endif
}

void ExternalRenderer::setCommand(const QString &command)
{
    if (command == commandLine) return;
    commandLine = command;
    // (a program that runs is closed, and the next picture starts the new one)
    if (isRunning()) stop();
}

void ExternalRenderer::setEnabled(bool enabled)
{
    if (enabled == enabledNow) return;
    enabledNow = enabled;
    if (!enabled) stop();
}

void ExternalRenderer::setVariable(const QString &name, const QString &value)
{
    if (variables.value(name) == value && variables.contains(name)) return;
    variables.insert(name, value);
    // (a program that runs with the old value is closed: the next picture starts it with the new one)
    if (isRunning() && commandLine.contains("{" + name + "}")) stop();
}

void ExternalRenderer::setImage(const QImage &image, double framesPerSecond)
{
    if (!enabledNow || image.isNull()) return;
    // The program is started for a size of picture: for another one (a view that is not the same size) it is started again
    if (isRunning() && (image.width() != pictureWidth || image.height() != pictureHeight)) stop();
    if (!isRunning()) start(image, framesPerSecond);
    if (!isRunning()) return;

    latest = toI420(image, width, height);
    // (the rate of the video may have changed: another capture, or a field view)
    if (framesPerSecond > 0.0 && framesPerSecond != rate) {
        rate = framesPerSecond;
        timer.setInterval(qMax(1, qRound(1000.0 / rate)));
    }
}

void ExternalRenderer::start(const QImage &image, double framesPerSecond)
{
    stop();
    pictureWidth = image.width();
    pictureHeight = image.height();
    width = (pictureWidth + 1) & ~1;
    height = (pictureHeight + 1) & ~1;
    rate = framesPerSecond > 0.0 ? framesPerSecond : 25.0;

    QString expanded = commandLine;
    expanded.replace("{width}", QString::number(width));
    expanded.replace("{height}", QString::number(height));
    expanded.replace("{fps}", QString::number(rate, 'f', 3));
    if (embeds()) expanded.replace("{winid}", QString::number(static_cast<qulonglong>(host->winId())));
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) expanded.replace("{" + it.key() + "}", it.value());
    QStringList words = QProcess::splitCommand(expanded);
    if (words.isEmpty()) {
        enabledNow = false;
        emit stopped(tr("The command of the external renderer is empty"));
        return;
    }
    const QString program = words.takeFirst();

    process = new QProcess(this);
    // What the program says is of no use here, and a pipe that is not read would hold it up
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::errorOccurred, this, [this, program](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        enabledNow = false;
        timer.stop();
        emit stopped(tr("The external renderer could not be started: %1").arg(program));
    });
    connect(process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        // (its window was closed)
        if (!enabledNow) return;
        enabledNow = false;
        timer.stop();
        emit stopped(tr("The external renderer ended"));
    });
    process->start(program, words);
    timer.setInterval(qMax(1, qRound(1000.0 / rate)));
    timer.start();
}

void ExternalRenderer::stop()
{
    timer.stop();
    latest.clear();
    if (!process) return;
    QProcess *closing = process;
    process = nullptr;
    closing->disconnect(this);
    closing->closeWriteChannel();
    if (closing->state() != QProcess::NotRunning && !closing->waitForFinished(1500)) {
        closing->terminate();
        if (!closing->waitForFinished(1500)) {
            closing->kill();
            closing->waitForFinished(2000);
        }
    }
    delete closing;
}

// One picture to the program. A program that does not keep up is not waited for: what it has not read is dropped, not queued
void ExternalRenderer::send()
{
    if (!process || latest.isEmpty() || process->state() != QProcess::Running) return;
    if (process->bytesToWrite() > 3 * static_cast<qint64>(latest.size())) {
        ++dropped;
        return;
    }
    process->write(latest);
}
