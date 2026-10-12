/******************************************************************************
 * vitsmetrics.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019-2020 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VITSMETRICS_H
#define VITSMETRICS_H

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// The lines of a field that the VITS measurements read (1-based, inclusive)
constexpr qint32 VITS_START_FIELD_LINE = 1;
constexpr qint32 VITS_END_FIELD_LINE = 22;

// Measure the white SNR and black PSNR of a field from its test signal lines, as ld-process-vits
// does, and put them in fieldMetadata.vitsMetrics. fieldLines holds the field lines from
// firstLineInData (at most VITS_START_FIELD_LINE) to VITS_END_FIELD_LINE or later.
void computeVitsMetrics(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                        const LdDecodeMetaData::VideoParameters &videoParameters,
                        LdDecodeMetaData::Field &fieldMetadata);

#endif // VITSMETRICS_H
