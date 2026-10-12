/************************************************************************

    vitsanalyser.cpp

    ld-process-vits - Vertical Interval Test Signal processing
    Copyright (C) 2020 Simon Inns

    This file is part of ld-decode-tools.

    ld-process-vits is free software: you can redistribute it and/or
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

#include "vitsanalyser.h"
#include "vitsmetrics.h"
#include "processingpool.h"
#include "tbc/logging.h"

VitsAnalyser::VitsAnalyser(QAtomicInt& _abort, ProcessingPool& _processingPool, QObject *parent)
    : QThread(parent), abort(_abort), processingPool(_processingPool)
{

}

// Thread main processing method
void VitsAnalyser::run()
{
    qint32 fieldNumber;

    // Input data buffers
    SourceVideo::Data sourceFieldData;
    LdDecodeMetaData::Field fieldMetadata;

    while(!abort) {
        // Get the next field to process from the input file
        if (!processingPool.getInputField(fieldNumber, sourceFieldData, fieldMetadata, videoParameters)) {
            // No more input fields -- exit
            break;
        }

        // Show an update to the user (for every 1000th field)
        if (fieldNumber % 1000 == 0) {
            qInfo() << "Processing field" << fieldNumber;
        }

        // Measure the white SNR and black PSNR from the test signal lines
        const double old_wSNR = fieldMetadata.vitsMetrics.wSNR;
        const double old_bPSNR = fieldMetadata.vitsMetrics.bPSNR;
        computeVitsMetrics(sourceFieldData, 1, videoParameters, fieldMetadata);

        // Show the result as debug
        tbcDebugStream().nospace() << "Field #" << fieldNumber << " has wSNR of " << fieldMetadata.vitsMetrics.wSNR << " (" << old_wSNR << ")"
                 << " and bPSNR of " << fieldMetadata.vitsMetrics.bPSNR << " (" << old_bPSNR << ")";

        // Write the result to the output metadata
        if (!processingPool.setOutputField(fieldNumber, fieldMetadata)) {
            abort = true;
            break;
        }
    }
}

