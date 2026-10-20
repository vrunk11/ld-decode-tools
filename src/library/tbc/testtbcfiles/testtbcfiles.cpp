/******************************************************************************
 * testtbcfiles.cpp
 * ld-decode-tools TBC library - unit test of the names of the files that are dropped on a program
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

#include "tbcfiles.h"

#define CHECK(condition) \
    do { if (!(condition)) { std::fprintf(stderr, "FAILED: %s (line %d)\n", #condition, __LINE__); std::exit(1); } } while (0)

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    // The captures a program can open, whatever the case of the name
    for (const char *name: { "disc.tbc", "disc.TBC", "disc_chroma.tbc", "disc.tbcy", "disc.tbcc", "disc.cvbs", "disc.cvbsy", "disc.cvbsc",
                             "C:/captures/disc.ldx", "disc.LDX", "disc.ldx#3" }) {
        CHECK(TbcFiles::isCaptureFileName(QString::fromLatin1(name)));
    }
    // What is not one
    for (const char *name: { "", "disc", "disc.wav", "disc.efm", "disc.tbc.db", "disc.tbc.json", "disc.meta", "disc.stack", "notes.txt", "disc.tbc.bak" }) {
        CHECK(!TbcFiles::isCaptureFileName(QString::fromLatin1(name)));
    }

    // A source of a stack: the composite or luma file of a capture, not the chroma file of a pair
    for (const char *name: { "disc.tbc", "disc.tbcy", "disc.cvbs", "disc.cvbsy", "disc.ldx" }) {
        CHECK(TbcFiles::isStackSourceName(QString::fromLatin1(name)));
    }
    for (const char *name: { "disc_chroma.tbc", "disc.tbcc", "disc.cvbsc", "disc.wav", "disc.stack" }) {
        CHECK(!TbcFiles::isStackSourceName(QString::fromLatin1(name)));
    }

    // The file of the mapping of a capture (ld-discmap), beside it
    CHECK(TbcFiles::mappedFileName("disc.tbc") == "disc_map.tbc");
    CHECK(TbcFiles::mappedFileName("C:/discs/Disc.TBC") == "C:/discs/Disc_map.TBC");
    CHECK(TbcFiles::mappedFileName("disc.cvbs") == "disc_map.cvbs");
    CHECK(TbcFiles::mappedFileName("disc_map.tbc").isEmpty());          // (a mapping has none of its own)
    CHECK(TbcFiles::mappedFileName("disc_chroma.tbc").isEmpty());       // (nor the chroma of a pair)
    CHECK(TbcFiles::mappedFileName("disc.wav").isEmpty() && TbcFiles::mappedFileName("disc.ldx").isEmpty() && TbcFiles::mappedFileName(QString()).isEmpty());
    CHECK(TbcFiles::isMappedFileName("disc_map.tbc") && TbcFiles::isMappedFileName("DISC_MAP.TBC") && TbcFiles::isMappedFileName("disc_map.cvbs"));
    CHECK(!TbcFiles::isMappedFileName("disc.tbc") && !TbcFiles::isMappedFileName("disc_map.wav") && !TbcFiles::isMappedFileName("map.tbc"));
    CHECK(TbcFiles::originalFileName("disc_map.tbc") == "disc.tbc");
    CHECK(TbcFiles::originalFileName("D:/x/A_MAP.TBC") == "D:/x/A.TBC");
    CHECK(TbcFiles::originalFileName("disc.tbc") == "disc.tbc" && TbcFiles::originalFileName("disc_map.wav") == "disc_map.wav");
    CHECK(TbcFiles::originalFileName(TbcFiles::mappedFileName("some disc.cvbs")) == "some disc.cvbs");

    std::printf("testtbcfiles: all checks passed\n");
    return 0;
}
