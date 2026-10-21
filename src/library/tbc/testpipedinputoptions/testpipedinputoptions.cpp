/******************************************************************************
 * testpipedinputoptions.cpp
 * ld-decode-tools - unit tests for PipedInputOptions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

#include "pipedinputoptions.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

// A parser with the options, that has read the arguments
struct Parsed {
    QCommandLineParser parser;
    PipedInputOptions options;
    explicit Parsed(const QStringList &arguments)
    {
        options.addTo(parser);
        CHECK(parser.parse(QStringList() << "tool" << arguments));
    }
};

static void testFormatAndSystem()
{
    std::cerr << "Testing the format, the system and the encoding\n";

    QString format;
    {
        Parsed none({ "-" });
        CHECK(none.options.format(none.parser, format) && format.isEmpty());
        CHECK(!none.options.isFormatSet(none.parser) && !none.options.isSystemSet(none.parser) && !none.options.isLevelSet(none.parser));
    }
    for (const char *name: { "ldx", "TBC", "Cvbs" }) {
        Parsed parsed({ "--input-format", name });
        CHECK(parsed.options.isFormatSet(parsed.parser));
        CHECK(parsed.options.format(parsed.parser, format));
        CHECK(format == QString(name).toLower());
    }
    {
        Parsed parsed({ "--input-format", "zzz" });
        CHECK(!parsed.options.format(parsed.parser, format));
    }

    VideoSystem system;
    for (const auto &entry: { std::make_pair("pal", PAL), std::make_pair("NTSC", NTSC), std::make_pair("pal-m", PAL_M), std::make_pair("PAL_M", PAL_M) }) {
        Parsed parsed({ "--system", entry.first });
        CHECK(parsed.options.videoSystem(parsed.parser, system) && system == entry.second);
    }
    {
        Parsed parsed({ "--system", "secam" });
        CHECK(!parsed.options.videoSystem(parsed.parser, system));
    }

    Cvbs::SampleEncoding encoding;
    for (const char *name: { "u10", "u16", "tpg21", "s16", "CVBS_U10_4FSC" }) {
        Parsed parsed({ "--sample-encoding", name });
        CHECK(parsed.options.isSampleEncodingSet(parsed.parser));
        CHECK(parsed.options.sampleEncoding(parsed.parser, encoding));
    }
    {
        Parsed parsed({ "--sample-encoding", "u12" });
        CHECK(!parsed.options.sampleEncoding(parsed.parser, encoding));
    }
}

static void testLevelsAndLocked()
{
    std::cerr << "Testing the levels and the subcarrier lock\n";

    qint32 black = 0, white = 0;
    {
        Parsed parsed({ "-" });
        CHECK(parsed.options.levels(parsed.parser, false, black, white) && black == -1 && white == -1);
    }
    {
        Parsed parsed({ "--black-level", "100", "--white-level", "60000" });
        CHECK(parsed.options.isLevelSet(parsed.parser));
        CHECK(parsed.options.levels(parsed.parser, false, black, white) && black == 100 && white == 60000);
        // CVBS has the standard levels: they are ignored
        CHECK(parsed.options.levels(parsed.parser, true, black, white) && black == -1 && white == -1);
    }
    // black can be 0, white must be above 0, neither negative
    {
        Parsed parsed({ "--black-level", "0" });
        CHECK(parsed.options.levels(parsed.parser, false, black, white) && black == 0);
    }
    for (const QStringList &arguments: { QStringList { "--black-level", "-5" }, QStringList { "--white-level", "0" }, QStringList { "--white-level", "x" } }) {
        Parsed parsed(arguments);
        CHECK(!parsed.options.levels(parsed.parser, false, black, white));
    }

    bool locked = false;
    {
        Parsed parsed({ "-" });
        CHECK(parsed.options.subcarrierLocked(parsed.parser, true, locked) && locked);
        CHECK(parsed.options.subcarrierLocked(parsed.parser, false, locked) && !locked);
    }
    {
        Parsed yes({ "--subcarrier-locked", "YES" });
        CHECK(yes.options.subcarrierLocked(yes.parser, false, locked) && locked);
        Parsed no({ "--subcarrier-locked", "no" });
        CHECK(no.options.subcarrierLocked(no.parser, true, locked) && !locked);
        Parsed maybe({ "--subcarrier-locked", "maybe" });
        CHECK(!maybe.options.subcarrierLocked(maybe.parser, true, locked));
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testFormatAndSystem();
    testLevelsAndLocked();
    std::cerr << "All tests passed\n";
    return 0;
}
