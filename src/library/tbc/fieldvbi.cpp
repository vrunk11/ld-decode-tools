/******************************************************************************
 * fieldvbi.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2020 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "fieldvbi.h"

#include "biphasecode.h"
#include "closedcaption.h"
#include "fmcode.h"
#include "videoid.h"
#include "vitccode.h"
#include "whiteflag.h"
#include "tbc/logging.h"

namespace {
// A single scanline of greyscale data
SourceVideo::Data getFieldLine(const SourceVideo::Data &fieldLines, qint32 firstLineInData, qint32 fieldLine,
                               const LdDecodeMetaData::VideoParameters &videoParameters)
{
    // Range-check the field line
    if (fieldLine < VBI_START_FIELD_LINE || fieldLine > VBI_END_FIELD_LINE || fieldLine < firstLineInData) {
        qWarning() << "Cannot generate field-line data, line number is out of bounds! Scan line =" << fieldLine;
        return SourceVideo::Data();
    }

    const qint32 startPointer = (fieldLine - firstLineInData) * videoParameters.fieldWidth;
    return fieldLines.mid(startPointer, videoParameters.fieldWidth);
}
}

void decodeFieldVbi(const SourceVideo::Data &fieldLines, qint32 firstLineInData,
                    const LdDecodeMetaData::VideoParameters &videoParameters,
                    LdDecodeMetaData::Field &fieldMetadata)
{
    // Get the 24-bit biphase-coded data from field lines 16-18
    BiphaseCode biphaseCode;
    biphaseCode.decodeLines(getFieldLine(fieldLines, firstLineInData, 16, videoParameters),
                            getFieldLine(fieldLines, firstLineInData, 17, videoParameters),
                            getFieldLine(fieldLines, firstLineInData, 18, videoParameters),
                            videoParameters, fieldMetadata);

    // Process NTSC specific data if source type is NTSC
    if (videoParameters.system == NTSC) {
        // Get the 40-bit FM coded data from field line 10
        FmCode fmCode;
        fmCode.decodeLine(getFieldLine(fieldLines, firstLineInData, 10, videoParameters), videoParameters, fieldMetadata);

        // Get the white flag from field line 11
        WhiteFlag whiteFlag;
        whiteFlag.decodeLine(getFieldLine(fieldLines, firstLineInData, 11, videoParameters), videoParameters, fieldMetadata);

        // Get IEC 61880 data from field line 20
        VideoID videoID;
        videoID.decodeLine(getFieldLine(fieldLines, firstLineInData, 20, videoParameters), videoParameters, fieldMetadata);

        fieldMetadata.ntsc.inUse = true;
    }

    // Get VITC data, trying each possible line and stopping when we find a valid one
    VitcCode vitcCode;
    for (qint32 lineNumber: vitcCode.getLineNumbers(videoParameters)) {
        if (vitcCode.decodeLine(getFieldLine(fieldLines, firstLineInData, lineNumber, videoParameters),
                                videoParameters, fieldMetadata)) {
            break;
        }
    }

    // Get Closed Caption data from line 21 (525-line) or 22 (625-line)
    ClosedCaption closedCaption;
    closedCaption.decodeLine(getFieldLine(fieldLines, firstLineInData, (videoParameters.system == PAL) ? 22 : 21, videoParameters),
                             videoParameters, fieldMetadata);
}
