/******************************************************************************
 * sourcefilechecks.cpp
 * ld-decode-tools - the checks of the files of a tool that has several inputs and one output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "sourcefilechecks.h"

#include <QDebug>
#include <QFileInfo>

#include "targetvideo.h"

namespace SourceFileChecks {

int outputIsNotAnInput(const QStringList &inputs, const QString &output)
{
    // Check that none of the input filenames are used as the output file
    for (const QString &input: inputs) {
        if (input == output && output != "-") {
            qCritical("Input and output files cannot have the same filenames");
            return 2;
        }
    }

    // The output stays in the container of the first input (TBC or CVBS)
    if (!inputs.isEmpty() && !checkSameVideoContainer(inputs.first(), output)) return 2;
    return 0;
}

int inputsAreDistinct(const QStringList &inputs)
{
    // Check that none of the input filenames are repeated
    for (qint32 i = 0; i < inputs.size(); i++) {
        for (qint32 j = 0; j < inputs.size(); j++) {
            if (i != j && inputs[i] == inputs[j]) {
                qCritical("Each input file should only be specified once - some filenames were repeated");
                return 2;
            }
        }
    }
    return 0;
}

int outputIsNew(const QString &output)
{
    // Check that the output file does not already exist
    if (output != "-" && QFileInfo(output).exists()) {
        qCritical("Specified output file already exists - will not overwrite");
        return 1;
    }
    return 0;
}

}
