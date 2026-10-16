/******************************************************************************
 * efmdecoding.cpp
 * ld-analyse - the EFM of a capture decoded to audio by a program, as it is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "efmdecoding.h"

#include <QCoreApplication>
#include <QFileInfo>

EfmDecoding::EfmDecoding(Backend backend, QObject *parent) : QObject(parent), which(backend) {}

EfmDecoding::~EfmDecoding()
{
    stop();
}

QString EfmDecoding::backendName(Backend backend)
{
    return backend == Backend::Suite ? tr("efm-decoder suite") : tr("ld-process-efm");
}

// (the programs are beside this one)
QString EfmDecoding::program(const QString &name) const
{
    QString path = QCoreApplication::applicationDirPath() + "/" + name;
#ifdef Q_OS_WIN
    path += ".exe";
#endif
    return QFileInfo::exists(path) ? path : QString();
}

bool EfmDecoding::start(const QString &efmFile, const Options &options, QString &error)
{
    stop();
    QStringList names;
    if (which == Backend::Suite) names = { "efm-decoder-f2", "efm-decoder-d24", "efm-decoder-audio" };
    else names = { "ld-process-efm" };
    for (const QString &name: names) {
        if (program(name).isEmpty()) {
            error = tr("%1 is not beside ld-analyse").arg(name);
            return false;
        }
    }

    cache = std::make_unique<QTemporaryFile>();
    if (!cache->open()) {
        error = tr("Could not make a file for the decoded audio");
        return false;
    }
    written = 0;
    stopping = false;
    lastError.clear();

    // The stages, each given the standard output of the one before; the last is read here
    std::vector<QStringList> arguments;
    if (which == Backend::Suite) {
        QStringList f2;
        if (options.noTimecodes) f2 << "--no-timecodes";
        f2 << efmFile << "-";
        QStringList audio { "--no-wav-header" };
        if (!options.conceal) audio << "--no-audio-concealment";
        if (options.pad) audio << "--zero-pad";
        audio << "-" << "-";
        arguments = { f2, { "-", "-" }, audio };
    } else {
        QStringList process;
        if (!options.conceal) process << "--pass-through";
        if (options.noTimecodes) process << "--time";
        if (options.pad) process << "--pad";
        process << efmFile << "-";
        arguments = { process };
    }

    for (size_t i = 0; i < names.size(); i++) {
        auto process = std::make_unique<QProcess>();
        process->setStandardErrorFile(QProcess::nullDevice());
        processes.push_back(std::move(process));
    }
    for (size_t i = 0; i + 1 < processes.size(); i++) processes[i]->setStandardOutputProcess(processes[i + 1].get());
    QProcess *last = processes.back().get();
    connect(last, &QProcess::readyReadStandardOutput, this, &EfmDecoding::readOutput);
    remaining = static_cast<qint32>(processes.size());
    for (size_t i = 0; i < processes.size(); i++) {
        connect(processes[i].get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (!stopping && (status != QProcess::NormalExit || code != 0) && lastError.isEmpty()) {
                lastError = tr("a decoder stopped with an error (%1)").arg(code);
            }
            if (--remaining == 0) processFinished();
        });
    }
    running = true;
    // (the first is started first: the pipe to the next is made when it starts)
    for (size_t i = 0; i < processes.size(); i++) processes[i]->start(program(names[i]), arguments[i]);
    for (size_t i = 0; i < processes.size(); i++) {
        if (!processes[i]->waitForStarted(5000)) {
            error = tr("%1 could not be started").arg(names[i]);
            stop();
            return false;
        }
    }
    return true;
}

void EfmDecoding::readOutput()
{
    QProcess *last = processes.empty() ? nullptr : processes.back().get();
    if (!last || !cache) return;
    const QByteArray data = last->readAllStandardOutput();
    if (data.isEmpty()) return;
    cache->write(data);
    cache->flush();
    written += data.size();
    emit grew();
}

void EfmDecoding::processFinished()
{
    readOutput();
    running = false;
    const bool ok = lastError.isEmpty() && written > 0;
    const QString message = ok ? tr("%1: %2 seconds of audio").arg(backendName(which)).arg(seconds(), 0, 'f', 1)
                               : (lastError.isEmpty() ? tr("%1 found no audio in the EFM").arg(backendName(which)) : lastError);
    emit finished(ok, message);
}

void EfmDecoding::stop()
{
    if (processes.empty()) return;
    stopping = true;
    for (auto &process: processes) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(2000);
    }
    processes.clear();
    running = false;
}
