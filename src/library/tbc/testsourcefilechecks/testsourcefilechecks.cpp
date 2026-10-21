/******************************************************************************
 * testsourcefilechecks.cpp
 * ld-decode-tools - unit tests for SourceFileChecks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#include "sourcefilechecks.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::cerr << "Testing SourceFileChecks\n";

    // The output is not an input, and is in the container of the first input
    CHECK(SourceFileChecks::outputIsNotAnInput({ "a.tbc", "b.tbc" }, "out.tbc") == 0);
    CHECK(SourceFileChecks::outputIsNotAnInput({ "a.tbc", "b.tbc" }, "b.tbc") == 2);
    CHECK(SourceFileChecks::outputIsNotAnInput({ "a.tbc", "b.tbc" }, "out.cvbs") == 2);
    CHECK(SourceFileChecks::outputIsNotAnInput({ "a.cvbs", "b.cvbs" }, "out.cvbs") == 0);
    // (the standard output is not a file: "-" as an input and as the output is not the same file)
    CHECK(SourceFileChecks::outputIsNotAnInput({ "-", "b.tbc" }, "-") == 0);

    // No input twice
    CHECK(SourceFileChecks::inputsAreDistinct({ "a.tbc", "b.tbc", "c.tbc" }) == 0);
    CHECK(SourceFileChecks::inputsAreDistinct({ "a.tbc", "b.tbc", "a.tbc" }) == 2);
    CHECK(SourceFileChecks::inputsAreDistinct({}) == 0);

    // The output is new: a file that is there is a failure of the work (1), not of the command line
    QTemporaryDir directory;
    const QString existing = directory.filePath("there.tbc");
    {
        QFile file(existing);
        CHECK(file.open(QIODevice::WriteOnly));
    }
    CHECK(SourceFileChecks::outputIsNew(directory.filePath("new.tbc")) == 0);
    CHECK(SourceFileChecks::outputIsNew(existing) == 1);
    CHECK(SourceFileChecks::outputIsNew("-") == 0);

    std::cerr << "All tests passed\n";
    return 0;
}
