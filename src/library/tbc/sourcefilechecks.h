/******************************************************************************
 * sourcefilechecks.h
 * ld-decode-tools - the checks of the files of a tool that has several inputs and one output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SOURCEFILECHECKS_H
#define SOURCEFILECHECKS_H

#include <QString>
#include <QStringList>

// What ld-disc-stacker and ld-dropout-correct check about their files before they begin, with the same messages. Each returns the exit code for what is wrong, having
// logged why (2 when the command line is wrong, 1 when a file is in the way: see AGENTS.md 4.4), or 0 if all is well. "-" is the standard input or output, which is not a file.
namespace SourceFileChecks {

// No input is the output, and the output stays in the container of the first input (TBC or CVBS)
int outputIsNotAnInput(const QStringList &inputs, const QString &output);

// No input is given twice
int inputsAreDistinct(const QStringList &inputs);

// The output file does not exist yet: nothing is overwritten
int outputIsNew(const QString &output);

}

#endif // SOURCEFILECHECKS_H
