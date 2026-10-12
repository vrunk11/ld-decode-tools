/******************************************************************************
 * fieldvbi.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2020 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FIELDVBI_H
#define FIELDVBI_H

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// The lines of a field that the VBI decoders read (1-based, inclusive)
constexpr qint32 VBI_START_FIELD_LINE = 6;
constexpr qint32 VBI_END_FIELD_LINE = 22;

// Decode what the field carries in its vertical blanking interval: the biphase code of lines
// 16-18, and for NTSC the FM code, white flag and video ID, as well as VITC and closed captions.
// fieldLines holds the field lines from firstLineInData (at most VBI_START_FIELD_LINE) to
// VBI_END_FIELD_LINE or later; the results are put in fieldMetadata (vbi, ntsc, vitc and
// closedCaption). This is what ld-process-vbi does for each field.
void decodeFieldVbi(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                    const LdDecodeMetaData::VideoParameters &videoParameters,
                    LdDecodeMetaData::Field &fieldMetadata);

#endif // FIELDVBI_H
