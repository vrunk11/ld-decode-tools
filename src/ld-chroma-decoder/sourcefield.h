/************************************************************************

    sourcefield.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2019 Adam Sampson

    This file is part of ld-decode-tools.

    ld-chroma-decoder is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#ifndef SOURCEFIELD_H
#define SOURCEFIELD_H

#include "lddecodemetadata.h"
#include "sourcevideo.h"

// A field read from the input, with metadata and data
struct SourceField {
    LdDecodeMetaData::Field field;
    SourceVideo::Data data;

    // Load a sequence of frames from the input files.
    //
    // fields will contain {lookbehind fields... [startIndex] real fields... [endIndex] lookahead fields...}.
    // Fields requested outside the bounds of the file will have dummy metadata and black data.
    //
    // NTSC fields with no phase ID in the metadata have it measured from the colour burst,
    // and where that fails it is the one the sequence expects. carryPhase, if given, is the
    // phase ID of the last field before this run (-1 if unknown), which the expected ones
    // follow from; it is set to that of this run's last field, for the next run.
    static void loadFields(SourceVideo &sourceVideo, LdDecodeMetaData &ldDecodeMetaData,
                           qint32 firstFrameNumber, qint32 numFrames,
                           qint32 lookBehindFrames, qint32 lookAheadFrames,
                           QVector<SourceField> &fields, qint32 &startIndex, qint32 &endIndex,
                           qint32 *carryPhase = nullptr);

    // The same for one pair of fields that begins at any field (a "weave" of fields p and p+1, which may be the second field of a
    // frame and the first of the next), with lookBehindPairs and lookAheadPairs pairs of fields each side. Each pair is given with its top
    // field first, as the decoders take it, whichever of the two comes first in time. Fields outside the file are black. A NTSC phase
    // that is not in the metadata is measured from the burst.
    static void loadFieldRun(SourceVideo &sourceVideo, LdDecodeMetaData &ldDecodeMetaData,
                             qint32 firstFieldNumber, qint32 lookBehindPairs, qint32 lookAheadPairs,
                             QVector<SourceField> &fields, qint32 &startIndex, qint32 &endIndex);

    // Return the vertical offset of this field within the interlaced frame
    // (i.e. 0 for the top field, 1 for the bottom field).
    qint32 getOffset() const {
        return field.isFirstField ? 0 : 1;
    }

    // Return the first/last active line numbers within this field's data,
    // given the video parameters.
    qint32 getFirstActiveLine(const LdDecodeMetaData::VideoParameters &videoParameters) const {
        return (videoParameters.firstActiveFrameLine + 1 - getOffset()) / 2;
    }
    qint32 getLastActiveLine(const LdDecodeMetaData::VideoParameters &videoParameters) const {
        return (videoParameters.lastActiveFrameLine + 1 - getOffset()) / 2;
    }
};

#endif
