/******************************************************************************
 * commonoptions.h
 * ld-decode-tools - command line options that more than one tool has
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef COMMONOPTIONS_H
#define COMMONOPTIONS_H

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

// The options that the tools have in common, defined once so that each is the same everywhere: the name, the short form, the text, the default and the check.
// (The metadata options are in MetadataOptions, -d and -q in logging.h.)

// -t, --threads <number>: the number of threads to work with
class ThreadsOption
{
public:
    ThreadsOption();

    // Register the option with the parser (before QCommandLineParser::process)
    void addTo(QCommandLineParser &parser);

    // The number of threads to use (after QCommandLineParser::process): the number of logical CPUs, or the one that was given. Returns false, having logged
    // why, if the number given is not above 0.
    bool process(const QCommandLineParser &parser, qint32 &threads) const;

private:
    QCommandLineOption option;
};

// -r, --reverse: the field order is second/first
class ReverseOption
{
public:
    ReverseOption();
    void addTo(QCommandLineParser &parser);
    bool isSet(const QCommandLineParser &parser) const { return parser.isSet(option); }

private:
    QCommandLineOption option;
};

// -n, --nobackup: do not make a backup of the metadata that is updated
// --ignore-map: read a capture as it is, though its metadata keep a map (see FrameMap): by default a tool that reads the video of a capture reads it
// through the map, in the order of the disc
class IgnoreMapOption
{
public:
    IgnoreMapOption();
    void addTo(QCommandLineParser &parser);
    bool isSet(const QCommandLineParser &parser) const { return parser.isSet(option); }

private:
    QCommandLineOption option;
};

// --legacy of the tools that back up their metadata: see NoBackupOption::backUp
class LegacyBackupOption
{
public:
    LegacyBackupOption();
    void addTo(QCommandLineParser &parser);
    bool isSet(const QCommandLineParser &parser) const { return parser.isSet(option); }

private:
    QCommandLineOption option;
};

class NoBackupOption
{
public:
    NoBackupOption();
    void addTo(QCommandLineParser &parser);
    bool isSet(const QCommandLineParser &parser) const { return parser.isSet(option); }

    // When the metadata file is going to be updated in place (the output is the input) and no backup was said to be useless, copy it to the same name with the
    // suffix (".bup") after it. A backup that is there is replaced: the last metadata before the tool ran are the ones to go back to. legacy: the way the tools
    // always did (the suffix they were given, and a backup that is there is a reason to stop). Returns false, having logged why, if that could not be done.
    static bool backUp(const QString &inputMetadataFileName, const QString &outputMetadataFileName, bool noBackup, const QString &suffix, bool legacy = false);

private:
    QCommandLineOption option;
};

#endif // COMMONOPTIONS_H
