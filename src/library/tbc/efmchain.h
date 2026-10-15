/******************************************************************************
 * efmchain.h
 * ld-decode-tools TBC library - the EFM of an LDX file through the stages of the EFM decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef EFMCHAIN_H
#define EFMCHAIN_H

#include <QString>

class QTemporaryDir;

// The EFM decoder is a chain of tools, each of which reads the file the one before made: efm-decoder-f2 (t-values to F2
// sections), efm-decoder-d24 (to Data24 sections) and efm-decoder-audio or efm-decoder-data. The EFM of an LDX file is the
// t-values, so a tool further down the chain that is given an LDX file runs the stages before it, with the tools that are beside
// this program, on files in a temporary directory, and goes on from there.
namespace EfmChain {

enum class Stage { F2Sections, Data24Sections };

// Run the stages up to the one named on the EFM of an LDX file (capture.ldx, or capture.ldx#4), and give the file made. The
// temporary directory must live as long as the file is used. False, with the reason, if a stage is not there or fails.
// noTimecodes is the option of efm-decoder-f2 of that name, for EFM that has no timecodes (the usual one of a LaserDisc).
bool decode(const QString &ldxName, Stage upTo, bool noTimecodes, QTemporaryDir &directory, QString &resultFile, QString &error);

} // namespace EfmChain

#endif // EFMCHAIN_H
