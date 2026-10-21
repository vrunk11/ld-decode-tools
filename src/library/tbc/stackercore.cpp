/************************************************************************

    stackercore.cpp

    ld-disc-stacker - Disc stacking for ld-decode
    Copyright (C) 2020-2022 Simon Inns

    SPDX-License-Identifier: GPL-3.0-or-later
    SPDX-FileCopyrightText: 2020-2022 Simon Inns

    This file is part of ld-decode-tools.

    ld-disc-stacker is free software: you can redistribute it and/or
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

#include "stackercore.h"
#include "tbc/logging.h"

// Method to stack fields
void StackerCore::stackField(const qint32 frameNumber,const QVector<SourceVideo::Data>& inputFields,
                                      const LdDecodeMetaData::VideoParameters& videoParameters,
                                      const QVector<LdDecodeMetaData::Field>& fieldMetadata,
                                      const QVector<qint32> availableSourcesForFrame,
                                      const bool& noDiffDod,const bool& passThrough,
                                      SourceVideo::Data &outputField,
                                      DropOuts &dropOuts,
                                      const qint32& mode,
                                      const qint32& smartThreshold,
                                      const bool& verbose)
{
    quint16 prevGoodValue = videoParameters.black16bIre;
    bool forceDropout = false;
    QVector<QVector<quint16>> tmpField(videoParameters.fieldHeight * videoParameters.fieldWidth);

    if (availableSourcesForFrame.size() > 0) {
        // Sources available - process field
        for (qint32 y = 0; y < videoParameters.fieldHeight; y++) {
            for (qint32 x = 0; x < videoParameters.fieldWidth; x++) {
                QVector<quint16> valuesN;//North neighbor pixel
                QVector<quint16> valuesS;//South neighbor pixel
                QVector<quint16> valuesE;//East neighbor pixel
                QVector<quint16> valuesW;//West neighbor pixel
                QVector<bool> isAllDropout = {true,true,true,true,true};//is neighbor pixel all dropout : current = [0] / N = [1] / S = [2] / E = [3] / W = [4]

                QVector<quint16> inputValues;
                // Get input values from the input sources (which are not marked as dropouts)
                if(mode >= 3)//get surrounding pixels
                {
                    StackerCore::getProcessedSample(x, y, availableSourcesForFrame, inputFields, tmpField, videoParameters, fieldMetadata, inputValues, valuesN, valuesS, valuesE, valuesW, isAllDropout, noDiffDod, verbose);
                }
                else// get only pixel 1 by 1
                {
                    for (qint32 i = 0; i < availableSourcesForFrame.size(); i++){
                        //read pixel
                        const quint16 pixelValue = inputFields[availableSourcesForFrame[i]][(videoParameters.fieldWidth * y) + x];
                        const bool sampleIsDropout = isDropout(fieldMetadata[availableSourcesForFrame[i]].dropOuts, x, y);

                        // Include the source's pixel data if it's not marked as a dropout
                        if (!sampleIsDropout) {
                            // Pixel is valid
                            inputValues.append(pixelValue);
                        }
                        else if((pixelValue > 0) && (!noDiffDod))
                        {
                            inputValues.append(pixelValue);
                        }

                        if(!sampleIsDropout)
                        {
                            isAllDropout[0] = false;
                        }
                    }

                    // If all possible input values are dropouts (and noDiffDod is false) and there are more than 3 input sources...
                    // Take the available values (marked as dropouts) and perform a diffDOD to try and determine if the dropout markings
                    // are false positives.
                    if (isAllDropout[0] && (availableSourcesForFrame.size() >= 3) && !noDiffDod) {
                        // Perform differential dropout detection to recover ld-decode false positive pixels
                        if(x > videoParameters.colourBurstStart)
                        {
                            inputValues = diffDod(inputValues, videoParameters, verbose);

                            if(verbose)
                            {
                                if (inputValues.size() > 0) {
                                    qInfo().nospace() << "Frame #" << frameNumber << ": DiffDOD recovered " << inputValues.size() <<
                                                         " values: " << inputValues << " for field location (" << x << ", " << y << ")";
                                } else if(x > videoParameters.colourBurstStart){
                                    qInfo().nospace() << "Frame #" << frameNumber << ": DiffDOD failed, no values recovered for field location (" << x << ", " << y << ")";
                                }
                                else{
                                    qInfo().nospace() << "Frame #" << frameNumber << ": Values 0 recovered for field location (" << x << ", " << y << ")";
                                }
                            }
                        }
                    }
                }

                // If passThrough is set, the output is always marked as a dropout if all input values are dropouts
                // (regardless of the diffDOD process result).
                forceDropout = false;
                if ((availableSourcesForFrame.size() > 0) && (passThrough)) {
                    if(x > videoParameters.colourBurstStart)
                    {
                        if (inputValues.size() == 0) {
                            forceDropout = true;
                            if(verbose)
                            {
                                qInfo().nospace() << "Frame #" << frameNumber << ": All sources for field location (" << x << ", " << y << ") are marked as dropout, passing through";
                            }
                        }
                    }
                }

                // Stack with intelligence:
                // If there are 3 or more sources - median (with central average for non-odd source sets)
                // If there are 2 sources - average
                // If there is 1 source - output as is
                // If there are zero sources - mark as a dropout in the output file
                if (inputValues.size() == 0) {
                    // No values available - use the previous good value and mark as a dropout
                    outputField[(videoParameters.fieldWidth * y) + x] = prevGoodValue;
                    if(x > videoParameters.colourBurstStart){dropOuts.append(x, x, y + 1);}
                } else if (inputValues.size() == 1) {
                    // 1 value available - just copy it to the output
                    outputField[(videoParameters.fieldWidth * y) + x] = inputValues[0];
                    prevGoodValue = outputField[(videoParameters.fieldWidth * y) + x];
                    if (forceDropout) dropOuts.append(x, x, y + 1);
                } else {
                    //2 or more values available - store the result in the output field
                    outputField[(videoParameters.fieldWidth * y) + x] = stackMode(inputValues, valuesN, valuesS, valuesE, valuesW, isAllDropout, mode, smartThreshold);
                    prevGoodValue = outputField[(videoParameters.fieldWidth * y) + x];
                    tmpField[(videoParameters.fieldWidth * y) + x] = QVector<quint16>{prevGoodValue};
                    if (forceDropout) dropOuts.append(x, x, y + 1);
                }
            }
        }

        // Concatenate the dropouts
        if (dropOuts.size() != 0) dropOuts.concatenate(verbose);
    } else {
        // No sources available for field - generate a dummy field at the black IRE level
        for (qint32 y = 0; y < videoParameters.fieldHeight; y++) {
            for (qint32 x = videoParameters.colourBurstStart; x < videoParameters.fieldWidth; x++) {
                outputField[(videoParameters.fieldWidth * y) + x] = videoParameters.black16bIre;
            }
        }
    }
}

// The mean of the samples that are within the threshold distance of a centre (not the distance itself), or the centre if there are none.
// The centre is signed in the smart mean mode and unsigned in the smart neighbour mode, and the arithmetic with the threshold is that of its type.
template<typename Centre>
static quint32 smartMean(const QVector<quint16>& elements, Centre centre, qint32 smartThreshold)
{
    qint32 nbSelected = 0;
    quint32 sum = 0;
    for(int i=0; i < elements.size();i++)
    {
        if(elements[i] < (centre + smartThreshold) &&  elements[i] > (centre - smartThreshold))
        {
            nbSelected++;
            sum += elements[i];
        }
    }
    //select the centre if all the samples are out of the threshold range
    if(nbSelected == 0) return centre;
    //mean averaging of selected sample
    return sum / nbSelected;
}

// What a neighbour gives: the median of its values if there are several and they all are dropouts, else the first one, or -1 if it has none
qint32 StackerCore::neighbourValue(const QVector<quint16>& elements, bool allDropout)
{
    if (elements.size() > 1 && allDropout) return StackerCore::median(elements);
    return elements.size() > 0 ? elements[0] : -1;
}

// What the four neighbours give, and how many of them are there (a neighbour that is not looked at, east and west when useEastWest is false, counts, with 0)
qint32 StackerCore::neighbourResults(const QVector<quint16>& elementsN, const QVector<quint16>& elementsS, const QVector<quint16>& elementsE, const QVector<quint16>& elementsW,
                                     const QVector<bool>& isAllDropout, bool useEastWest, qint32& resultN, qint32& resultS, qint32& resultE, qint32& resultW)
{
    resultN = neighbourValue(elementsN, isAllDropout[1]);
    resultS = neighbourValue(elementsS, isAllDropout[2]);
    if(useEastWest)
    {
        resultE = neighbourValue(elementsE, isAllDropout[3]);
        resultW = neighbourValue(elementsW, isAllDropout[4]);
    }

    //check number of neighbor available and prepare for mean
    qint32 nbNeighbor = 0;
    (resultN != -1) ? nbNeighbor++ : resultN = 0;
    (resultS != -1) ? nbNeighbor++ : resultS = 0;
    (resultE != -1) ? nbNeighbor++ : resultE = 0;
    (resultW != -1) ? nbNeighbor++ : resultW = 0;
    return nbNeighbor;
}

// The sample of the sources that is closest to each of the neighbours that has a value
QVector<quint16> StackerCore::closestToNeighbours(const QVector<quint16>& elements, qint32 resultN, qint32 resultS, qint32 resultE, qint32 resultW)
{
    QVector<quint16> closestList;
    if(resultN > 0){closestList.append(StackerCore::closest(elements, resultN));}
    if(resultS > 0){closestList.append(StackerCore::closest(elements, resultS));}
    if(resultE > 0){closestList.append(StackerCore::closest(elements, resultE));}
    if(resultW > 0){closestList.append(StackerCore::closest(elements, resultW));}
    return closestList;
}

// Method to stack a vector of quint16 using a selected mode
quint16 StackerCore::stackMode(const QVector<quint16>& elements, const QVector<quint16>& elementsN, const QVector<quint16>& elementsS, const QVector<quint16>& elementsE, const QVector<quint16>& elementsW, const QVector<bool>& isAllDropout, const qint32& mode, const qint32& smartThreshold)
{
    const qint32 nbOfElements = elements.size();
    quint32 result = 0;
    QVector<quint16> closestList;

    //neighbor pixel
    qint32 resultN = 0;
    qint32 resultS = 0;
    qint32 resultE = 0;
    qint32 resultW = 0;
    quint32 resultNeighbor = 0;

    qint32 nbNeighbor = 0;

    switch (mode) {
        case 0://mean mode
        {
            result = StackerCore::mean(elements);
            break;
        }
        case 1://median mode
        {
            result = StackerCore::median(elements);
            break;
        }
        case 2://smart mean mode
        {
            const qint32 median = StackerCore::median(elements);
            // the mean of the samples within the threshold distance to the median, or the median if there is none
            result = smartMean(elements, median, smartThreshold);
            break;
        }
        case 3://smart neighbor mode
        {
            const qint32 median = StackerCore::median(elements);

            // (east and west count unless the sample is a dropout in every source)
            nbNeighbor = neighbourResults(elementsN, elementsS, elementsE, elementsW, isAllDropout, !isAllDropout[0], resultN, resultS, resultE, resultW);

            if(nbNeighbor > 0)
            {
                //closest value to a neighbor
                closestList = closestToNeighbours(elements, resultN, resultS, resultE, resultW);

                resultNeighbor = StackerCore::closest(closestList, median);//get the closest value to the median/mean based on closest value to a neighbor
            }
            else
            {
                resultNeighbor = result;
            }

            if(nbOfElements > 2)//using median + mean
            {
                // the mean of the samples within the threshold distance to the closest value to a neighbor, or that value if there is none
                result = smartMean(elements, resultNeighbor, smartThreshold);
            }
            else//using surrounding sample
            {
                result = resultNeighbor;// get the the closest value to neighbor
            }
            break;
        }
        case 4://neighbor mode
        {
            const qint32 median = StackerCore::median(elements);

            nbNeighbor = neighbourResults(elementsN, elementsS, elementsE, elementsW, isAllDropout,
                                          !isAllDropout[0] || (isAllDropout[1] && isAllDropout[2]), resultN, resultS, resultE, resultW);

            if(nbNeighbor > 0)
            {
                closestList = closestToNeighbours(elements, resultN, resultS, resultE, resultW);

                result = StackerCore::closest(closestList, median);//get the closest value to the median/mean based on closest value to a neighbor

                if(nbOfElements > 2)
                {
                    result = (median + result) / 2;// get the mean between (median/mean) and the closest value to neighbor
                }
            }
            else
            {
                result = median;
            }
            break;
        }
    }

    return static_cast<quint16>(result);
}

// Method to find the median of a vector of quint16s
inline quint16 StackerCore::median(QVector<quint16> elements)
{
    const qint32 noOfElements = elements.size();

    if (noOfElements % 2 == 0) {
        // Input set is even length

        // Applying nth_element on n/2th index
        std::nth_element(elements.begin(), elements.begin() + noOfElements / 2, elements.end());

        // Applying nth_element on (n-1)/2 th index
        std::nth_element(elements.begin(), elements.begin() + (noOfElements - 1) / 2, elements.end());

        // Find the average of value at index N/2 and (N-1)/2
        return static_cast<quint16>((elements[(noOfElements - 1) / 2] + elements[noOfElements / 2]) / 2.0);
    } else {
        // Input set is odd length

        // Applying nth_element on n/2
        std::nth_element(elements.begin(), elements.begin() + noOfElements / 2, elements.end());

        // Value at index (N/2)th is the median
        return static_cast<quint16>(elements[noOfElements / 2]);
    }
}

// Method to find the mean of a vector of quint16s
inline qint32 StackerCore::mean(const QVector<quint16>& elements)
{
    quint32 result = 0;
    const qint32 nbElements = elements.size();

    if(nbElements > 1)
    {
        //compute mean of all values
        for(int i=0; i < nbElements;i++)
        {
            if(nbElements > 1)
            {
                result += elements[i];
            }
        }
        return (result / nbElements);
    }
    else if(nbElements == 1)
    {
        return elements[0];
    }
    else
    {
        return -1;
    }

}

// Method to find the closest value to a target
inline quint16 StackerCore::closest(const QVector<quint16>& elements, const qint32 target)
{
    const qint32 nbOfElements = elements.size();
    qint32 closest = 0;

    if(nbOfElements > 0)
    {
        closest = elements[0];
        for(int i=1;i < nbOfElements;i++)
        {
            if(abs(target - elements[i]) < abs(target - closest))
            {
                closest = elements[i];
            }
        }
    }

    return closest;
}

// get value that are unprocessed and reuse processed one for mode >= 3
void StackerCore::getProcessedSample(const qint32 x, const qint32 y, const QVector<qint32>& availableSourcesForFrame, const QVector<SourceVideo::Data>& inputFields, QVector<QVector<quint16>>& tmpField, const LdDecodeMetaData::VideoParameters& videoParameters, const QVector<LdDecodeMetaData::Field>& fieldMetadata, QVector<quint16>& sample, QVector<quint16>& sampleN, QVector<quint16>& sampleS, QVector<quint16>& sampleE, QVector<quint16>& sampleW, QVector<bool>& isAllDropout, const bool& noDiffDod, const bool& verbose)
{
    quint16 pixelValue = 0;
    qint32 source = 0;
    qint32 fieldWidth = videoParameters.fieldWidth;
    qint32 fieldHeight = videoParameters.fieldHeight;
    bool sampleIsDropout = true;
    for (qint32 i = 0; i < availableSourcesForFrame.size(); i++) {
        source = availableSourcesForFrame[i];
        if(y == 0)
        {
            if(x == 0)//read value + east + south
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * y) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sample.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sample.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[0] = false;
                }

                pixelValue = inputFields[source][(fieldWidth * y) + x + 1];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x+1, y);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleE.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleE.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[3] = false;//E = [3]
                }

                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
            else if(x == fieldWidth -1)//read south value
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
            else//read east + south
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * y) + x + 1];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x+1, y);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleE.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleE.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[3] = false;//E = [3]
                }

                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
        }
        else if(y != fieldHeight -1)//read south value
        {
            if(x == 0)//get neighbor value except on left
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
            if(x == fieldWidth -1)//get neighbor value except on right
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
            else
            {
                //read new value
                pixelValue = inputFields[source][(fieldWidth * (y+1)) + x];
                sampleIsDropout = isDropout(fieldMetadata[source].dropOuts, x, y+1);
                if (!sampleIsDropout) {
                    // Pixel is valid
                    sampleS.append(pixelValue);
                }
                else if((pixelValue > 0) && (!noDiffDod))
                {
                    sampleS.append(pixelValue);
                }

                if(!sampleIsDropout)
                {
                    isAllDropout[2] = false;//S = [2]
                }
            }
        }
    }
    // If all possible input values are dropouts (and noDiffDod is false) and there are more than 3 input sources...
    // Take the available values (marked as dropouts) and perform a diffDOD to try and determine if the dropout markings
    // are false positives.
    if(y == 0)
    {
        if(x == 0)//read value + east + south
        {
            if(!noDiffDod)
            {
                if(x > videoParameters.colourBurstStart)
                {
                    if (isAllDropout[0] && (availableSourcesForFrame.size() >= 3)) {
                        sample = diffDod(sample, videoParameters, verbose);
                    }
                    if (isAllDropout[3] && (availableSourcesForFrame.size() >= 3)) {
                        sampleE = diffDod(sampleE, videoParameters, verbose);
                    }
                    if (isAllDropout[2] && (availableSourcesForFrame.size() >= 3)) {
                        sampleS = diffDod(sampleS, videoParameters, verbose);
                    }
                }
            }
            tmpField[(fieldWidth * y) + x] = sample;
            tmpField[(fieldWidth * y) + x + 1] = sampleE;
            tmpField[(fieldWidth * (y+1)) + x] = sampleS;
        }
        else if(x == fieldWidth -1)//read south value
        {
            if(!noDiffDod)
            {
                if(x > videoParameters.colourBurstStart)
                {
                    if (isAllDropout[2] && (availableSourcesForFrame.size() >= 3)) {
                        sampleS = diffDod(sampleS, videoParameters, verbose);
                    }
                }
            }
            tmpField[(fieldWidth * (y+1)) + x] = sampleS;
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
        else//read east + south
        {
            if(!noDiffDod)
            {
                if(x > videoParameters.colourBurstStart)
                {
                    if (isAllDropout[3] && (availableSourcesForFrame.size() >= 3)) {
                        sampleE = diffDod(sampleE, videoParameters, verbose);
                    }
                    if (isAllDropout[2] && (availableSourcesForFrame.size() >= 3)) {
                        sampleS = diffDod(sampleS, videoParameters, verbose);
                    }
                }
            }
            tmpField[(fieldWidth * y) + x + 1] = sampleE;
            tmpField[(fieldWidth * (y+1)) + x] = sampleS;
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
    }
    else if(y != fieldHeight -1)//read south value
    {
        if(!noDiffDod)
        {
            if(x > videoParameters.colourBurstStart)
            {
                if (isAllDropout[2] && (availableSourcesForFrame.size() >= 3)) {
                    sampleS = diffDod(sampleS, videoParameters, verbose);
                }
            }
        }
        tmpField[(fieldWidth * (y+1)) + x] = sampleS;
        if(x == 0)
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleE = tmpField[(fieldWidth * y) + x + 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[3] = haveAllDropout(fieldMetadata,x+1,y);
        }
        else if (x == fieldWidth -1)
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
        else
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            sampleE = tmpField[(fieldWidth * y) + x + 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[3] = haveAllDropout(fieldMetadata,x+1,y);
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
    }
    else//all value already processed : reuse value
    {
        if(x == 0)
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleE = tmpField[(fieldWidth * y) + x + 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[3] = haveAllDropout(fieldMetadata,x+1,y);
        }
        if(x == fieldWidth -1)
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
        else
        {
            sample = tmpField[(fieldWidth * y) + x];
            sampleW = tmpField[(fieldWidth * y) + x - 1];
            sampleE = tmpField[(fieldWidth * y) + x + 1];
            sampleN = tmpField[(fieldWidth * (y-1)) + x];
            isAllDropout[1] = haveAllDropout(fieldMetadata,x,y-1);
            isAllDropout[3] = haveAllDropout(fieldMetadata,x+1,y);
            isAllDropout[4] = haveAllDropout(fieldMetadata,x-1,y);
        }
    }
}

// Method returns true if specified pixel is a dropout
inline bool StackerCore::isDropout(const DropOuts& dropOuts, const qint32 fieldX, const qint32 fieldY)
{
    for (qint32 i = 0; i < dropOuts.size(); i++) {
        if ((dropOuts.fieldLine(i) - 1) == fieldY) {
            if ((fieldX >= dropOuts.startx(i)) && (fieldX <= dropOuts.endx(i)))
                return true;
        }
    }

    return false;
}

// Method returns true if all specified pixel are dropouts
inline bool StackerCore::haveAllDropout(const QVector<LdDecodeMetaData::Field>& fieldMetadata, const qint32 x, const qint32 y)
{
    const qint32 size = fieldMetadata.size();
    for (qint32 i = 0; i < size; i++) {
        if(!isDropout(fieldMetadata[i].dropOuts,x,y))
            return false;
    }

    return true;
}

// Use differential dropout detection to remove suspected dropout error
// values from inputValues to produce the set of output values.  This generally improves everything, but
// might cause an increase in errors for really noisy frames (where the DOs are in the same place in
// multiple sources).  Another possible disadvantage is that diffDOD might pass through master plate errors
// which, whilst not technically errors, may be undesirable.
QVector<quint16> StackerCore::diffDod(const QVector<quint16>& inputValues, const LdDecodeMetaData::VideoParameters& videoParameters, const bool& verbose)
{
    QVector<quint16> outputValues;

    // Check that we have at least 3 input values
    if (inputValues.size() < 3) {
        return inputValues;
    }

    // Get the median value of the input values
    const double medianValue = static_cast<double>(median(inputValues));

    // Set the matching threshold to +-10% of the median value
    const double threshold = 10; // %

    // Set the maximum and minimum values for valid inputs
    double maxValueD = medianValue + ((medianValue / 100.0) * threshold);
    double minValueD = medianValue - ((medianValue / 100.0) * threshold);
    if (minValueD < 0) minValueD = 0;
    if (maxValueD > 65535) maxValueD = 65535;
    quint16 minValue = minValueD;
    quint16 maxValue = maxValueD;

    // Copy valid input values to the output set
    for (qint32 i = 0; i < inputValues.size(); i++) {
        if ((inputValues[i] > minValue) && (inputValues[i] < maxValue)) {
            outputValues.append(inputValues[i]);
        }
    }

    // Show debug
    if(verbose)
    {
        tbcDebugStream() << "diffDOD:  Input" << inputValues;
        if (outputValues.size() == 0) {
            tbcDebugStream().nospace() << "diffDOD: Empty output... Range was " << minValue << "-" << maxValue << " with a median of " << medianValue;
        } else {
            tbcDebugStream() << "diffDOD: Output" << outputValues;
        }
    }

    return outputValues;
}

// The stacking mode for a frame with this many sources, when the mode asked for is auto
qint32 StackerCore::autoMode(qint32 numberOfSources)
{
    if (numberOfSources <= 2) return 0;   // mean
    if (numberOfSources <= 4) return 2;   // smart mean
    return 3;                             // smart neighbour
}
