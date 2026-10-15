/******************************************************************************
 * efmchain.cpp
 * ld-decode-tools TBC library - the EFM of an LDX file through the stages of the EFM decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "efmchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>

namespace EfmChain {

namespace {

// The tool of a stage, beside this program
QString toolFile(const QString &name)
{
    const QString directory = QCoreApplication::applicationDirPath();
    for (const QString &candidate: { name, name + ".exe" }) {
        const QString path = QDir(directory).filePath(candidate);
        if (QFileInfo::exists(path)) return path;
    }
    return QString();
}

bool runStage(const QString &tool, const QStringList &arguments, QString &error)
{
    const QString program = toolFile(tool);
    if (program.isEmpty()) {
        error = QObject::tr("%1 is not beside this program: it is needed to decode the EFM of an LDX file").arg(tool);
        return false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(program, arguments);
    if (!process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QObject::tr("%1 failed on the EFM of the LDX file").arg(tool);
        return false;
    }
    return true;
}

} // namespace

bool decode(const QString &ldxName, Stage upTo, bool noTimecodes, QTemporaryDir &directory, QString &resultFile, QString &error)
{
    if (!directory.isValid()) {
        error = QObject::tr("Could not make a temporary directory for the stages of the EFM decoder");
        return false;
    }
    const QString f2 = directory.filePath("efm.f2s");
    qInfo().noquote() << "Decoding the EFM of" << ldxName << "to F2 sections";
    QStringList f2Arguments { ldxName, f2 };
    if (noTimecodes) f2Arguments.prepend("--no-timecodes");
    if (!runStage("efm-decoder-f2", f2Arguments, error)) return false;
    resultFile = f2;
    if (upTo == Stage::F2Sections) return true;

    const QString d24 = directory.filePath("efm.d24");
    qInfo() << "Decoding the F2 sections to Data24 sections";
    if (!runStage("efm-decoder-d24", { f2, d24 }, error)) return false;
    resultFile = d24;
    return true;
}

} // namespace EfmChain
