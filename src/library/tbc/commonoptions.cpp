/******************************************************************************
 * commonoptions.cpp
 * ld-decode-tools - command line options that more than one tool has
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "commonoptions.h"

#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QDebug>

ThreadsOption::ThreadsOption()
    : option(QStringList() << "t" << "threads",
             QCoreApplication::translate("main", "Specify the number of concurrent threads (default is the number of logical CPUs)"),
             QCoreApplication::translate("main", "number"))
{
}

void ThreadsOption::addTo(QCommandLineParser &parser)
{
    parser.addOption(option);
}

bool ThreadsOption::process(const QCommandLineParser &parser, qint32 &threads) const
{
    threads = QThread::idealThreadCount();
    if (parser.isSet(option)) {
        threads = parser.value(option).toInt();

        if (threads < 1) {
            // Quit with error
            qCritical("Specified number of threads must be greater than zero");
            return false;
        }
    }
    return true;
}

ReverseOption::ReverseOption()
    : option(QStringList() << "r" << "reverse",
             QCoreApplication::translate("main", "Reverse the field order to second/first (default first/second)"))
{
}

void ReverseOption::addTo(QCommandLineParser &parser)
{
    parser.addOption(option);
}

IgnoreMapOption::IgnoreMapOption()
    : option("ignore-map",
             QCoreApplication::translate("main", "Read the capture as it is, though its metadata keep a map (the frames in the order of the disc, from ld-discmap)"))
{
}

void IgnoreMapOption::addTo(QCommandLineParser &parser)
{
    parser.addOption(option);
}

NoBackupOption::NoBackupOption()
    : option(QStringList() << "n" << "nobackup",
             QCoreApplication::translate("main", "Do not create a backup of the input metadata"))
{
}

void NoBackupOption::addTo(QCommandLineParser &parser)
{
    parser.addOption(option);
}

bool NoBackupOption::backUp(const QString &inputMetadataFileName, const QString &outputMetadataFileName, bool noBackup, const QString &suffix, bool legacy)
{
    // If we're overwriting the input metadata file, back it up first
    if (inputMetadataFileName == outputMetadataFileName && !noBackup) {
        // (one suffix for every tool, and the backup of the last run is replaced; legacy: the suffix of the tool, and a backup that is there stops it)
        const QString backupName = inputMetadataFileName + (legacy ? suffix : QStringLiteral(".bup"));
        qInfo().nospace().noquote() << "Backing up metadata to " << backupName;
        if (!legacy && QFile::exists(backupName)) QFile::remove(backupName);
        if (!QFile::copy(inputMetadataFileName, backupName)) {
            qCritical() << "Unable to back-up input metadata file - back-up already exists?";
            return false;
        }
    }
    return true;
}

LegacyBackupOption::LegacyBackupOption()
    : option("legacy",
             QCoreApplication::translate("main", "Keep the backup of the metadata as the tools always did: the suffix of the tool (.bup for ld-process-vbi, .vbup for ld-process-vits), and an existing backup stops the run (without it the backup is always name.bup and replaces the last one)"))
{
}

void LegacyBackupOption::addTo(QCommandLineParser &parser)
{
    parser.addOption(option);
}
