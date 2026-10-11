/************************************************************************

    sourcefield.cpp

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

#include "sourcefield.h"

#include "sourcevideo.h"
#include "cvbs.h"

void SourceField::loadFields(SourceVideo &sourceVideo, LdDecodeMetaData &ldDecodeMetaData,
                             qint32 firstFrameNumber, qint32 numFrames,
                             qint32 lookBehindFrames, qint32 lookAheadFrames,
                             QVector<SourceField> &fields, qint32 &startIndex, qint32 &endIndex)
{
    const LdDecodeMetaData::VideoParameters &videoParameters = ldDecodeMetaData.getVideoParameters();

    // Work out indexes.
    // fields will contain {lookbehind fields... [startIndex] real fields... [endIndex] lookahead fields...}.
    startIndex = 2 * lookBehindFrames;
    endIndex = startIndex + (2 * numFrames);
    fields.resize(endIndex + (2 * lookAheadFrames));

    // Video from a stream that came without metadata has fields made as it turns
    // out to be long enough: the first two are needed for the blank frames
    const bool isGenerated = ldDecodeMetaData.isGenerated();
    if (isGenerated) ldDecodeMetaData.ensureFields(2);

    // Populate fields
    qint32 frameNumber = firstFrameNumber - lookBehindFrames;
    for (qint32 i = 0; i < fields.size(); i += 2) {

        // Is this frame outside the bounds of the input file? Metadata that is made
        // up has no bounds, but the stream it goes with may end, anywhere.
        // If so, use real metadata (from frame 1) and black fields.
        if (isGenerated && frameNumber >= 1) ldDecodeMetaData.ensureFields(2 * frameNumber);
        bool useBlankFrame = frameNumber < 1 || frameNumber > ldDecodeMetaData.getNumberOfFrames();

        // Get the first frame from the file (using frame 1 if outside bounds)
        qint32 firstFieldNumber = ldDecodeMetaData.getFirstFieldNumber(useBlankFrame ? 1 : frameNumber);
        qint32 secondFieldNumber = ldDecodeMetaData.getSecondFieldNumber(useBlankFrame ? 1 : frameNumber);

        // A stream may end before the metadata says it does
        if (!useBlankFrame && sourceVideo.isStream()
            && !sourceVideo.hasField(qMax(firstFieldNumber, secondFieldNumber))) {
            useBlankFrame = true;
            firstFieldNumber = ldDecodeMetaData.getFirstFieldNumber(1);
            secondFieldNumber = ldDecodeMetaData.getSecondFieldNumber(1);
        }

        // Fetch the input metadata
        fields[i].field = ldDecodeMetaData.getField(firstFieldNumber);
        fields[i + 1].field = ldDecodeMetaData.getField(secondFieldNumber);

        const quint16 black = videoParameters.black16bIre;

        if (useBlankFrame) {
            // Fill both fields with black
            fields[i].data.fill(black, sourceVideo.getFieldLength());
            fields[i + 1].data.fill(black, sourceVideo.getFieldLength());
        } else {
            // Fetch the input fields
            fields[i].data = sourceVideo.getVideoField(firstFieldNumber);
            fields[i + 1].data = sourceVideo.getVideoField(secondFieldNumber);

            // Without the phase of the field in the metadata (a CVBS capture, or a
            // stream with none) it is measured from the colour burst
            if (videoParameters.system == NTSC) {
                for (SourceField *sourceField: {&fields[i], &fields[i + 1]}) {
                    if (sourceField->field.fieldPhaseID < 1) {
                        sourceField->field.fieldPhaseID = Cvbs::measureFieldPhaseID(sourceField->data.constData(), NTSC,
                                                                                    sourceField->field.isFirstField);
                    }
                }
            }

            if ((videoParameters.system == PAL || videoParameters.system == PAL_M) && videoParameters.isSubcarrierLocked) {
                // With subcarrier-locked 4fSC PAL sampling, we have four
                // "extra" samples over the course of the frame, so the two
                // fields will be horizontally misaligned by two samples. Shift
                // the second field to the left to compensate.
                //
                // XXX This should be done elsewhere, as it affects other tools
                // too.

                fields[i + 1].data.remove(0, 2);
                for (int j = 0; j < 2; j++) {
                    fields[i + 1].data.append(black);
                }
            }
        }

        frameNumber++;
    }
}
