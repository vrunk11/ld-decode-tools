/******************************************************************************
 * testcommonoptions.cpp
 * ld-decode-tools - unit tests for the options that the tools have in common
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdlib>
#include <iostream>

#include "commonoptions.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static void testThreads()
{
    std::cerr << "Testing ThreadsOption\n";

    qint32 threads = 0;
    {
        // not given: the logical CPUs
        QCommandLineParser parser;
        ThreadsOption option;
        option.addTo(parser);
        CHECK(parser.parse({ "tool", "input" }));
        CHECK(option.process(parser, threads));
        CHECK(threads == QThread::idealThreadCount());
    }
    for (const QStringList &arguments: { QStringList { "tool", "-t", "3" }, QStringList { "tool", "--threads", "3" }, QStringList { "tool", "--threads=3" } }) {
        QCommandLineParser parser;
        ThreadsOption option;
        option.addTo(parser);
        CHECK(parser.parse(arguments));
        CHECK(option.process(parser, threads));
        CHECK(threads == 3);
    }
    // 0, a negative number and a word are refused
    for (const char *value: { "0", "-2", "many" }) {
        QCommandLineParser parser;
        ThreadsOption option;
        option.addTo(parser);
        CHECK(parser.parse({ "tool", "-t", value }));
        CHECK(!option.process(parser, threads));
    }
}

static void testFlags()
{
    std::cerr << "Testing ReverseOption and NoBackupOption\n";

    QCommandLineParser parser;
    ReverseOption reverse;
    NoBackupOption noBackup;
    reverse.addTo(parser);
    noBackup.addTo(parser);
    CHECK(parser.parse({ "tool", "input" }));
    CHECK(!reverse.isSet(parser) && !noBackup.isSet(parser));

    QCommandLineParser other;
    ReverseOption reverseToo;
    NoBackupOption noBackupToo;
    reverseToo.addTo(other);
    noBackupToo.addTo(other);
    CHECK(other.parse({ "tool", "-r", "--nobackup", "input" }));
    CHECK(reverseToo.isSet(other) && noBackupToo.isSet(other));
    CHECK(other.positionalArguments() == QStringList({ "input" }));
}

static void testBackUp()
{
    std::cerr << "Testing NoBackupOption::backUp\n";

    QTemporaryDir directory;
    const QString metadata = directory.filePath("capture.tbc.db");
    {
        QFile file(metadata);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write("metadata");
    }

    // The output is another file: nothing to back up
    CHECK(NoBackupOption::backUp(metadata, directory.filePath("other.db"), false, ".bup"));
    CHECK(!QFile::exists(metadata + ".bup"));

    // Said not to
    CHECK(NoBackupOption::backUp(metadata, metadata, true, ".bup"));
    CHECK(!QFile::exists(metadata + ".bup"));

    // Updated in place: a copy beside it, with the suffix
    CHECK(NoBackupOption::backUp(metadata, metadata, false, ".bup"));
    CHECK(QFile::exists(metadata + ".bup"));
    QFile copy(metadata + ".bup");
    CHECK(copy.open(QIODevice::ReadOnly) && copy.readAll() == "metadata");

    // A backup that is there is not replaced: it fails
    CHECK(!NoBackupOption::backUp(metadata, metadata, false, ".bup"));
    CHECK(NoBackupOption::backUp(metadata, metadata, false, ".vbup"));
    CHECK(QFile::exists(metadata + ".vbup"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testThreads();
    testFlags();
    testBackUp();
    std::cerr << "All tests passed\n";
    return 0;
}
