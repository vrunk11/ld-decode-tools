/************************************************************************

    sourceaudio.cpp

    ld-decode-tools TBC library
    Copyright (C) 2018-2020 Simon Inns

    This file is part of ld-decode-tools.

    ld-decode-tools is free software: you can redistribute it and/or
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

#include "sourceaudio.h"

#include "ldxcapture.h"

#include <QDataStream>
#include <QtEndian>
#include "tbc/logging.h"

SourceAudio::SourceAudio()
{
    audioFileByteLength = 0;
}

SourceAudio::~SourceAudio()
{
    if (audioFileByteLength != 0) inputAudioFile.close();
}

// Open an audio source file
bool SourceAudio::open(QFileInfo inputFileInfo)
{
    // The audio of a capture kept in an LDX file is a track of it
    if (LdxCapture::isLdxName(inputFileInfo.filePath())) {
        auto capture = std::make_unique<LdxCapture>();
        QString error;
        if (!capture->openAudio(inputFileInfo.filePath(), error)) {
            qCritical().noquote() << error;
            return false;
        }
        audioFileByteLength = capture->audioSamples() * 4;
        ldx = std::move(capture);
        return true;
    }

    // Get the input audio fileinfo from the input TBC fileinfo:
    QFileInfo inputAudioFileInfo(inputFileInfo.absolutePath() + "/" + inputFileInfo.completeBaseName() + ".pcm");

    // Open the audio source data file
    inputAudioFile.setFileName(inputAudioFileInfo.filePath());
    if (!inputAudioFile.open(QIODevice::ReadOnly)) {
        // Failed to open named input file
        tbcDebugStream() << "Could not open" << inputAudioFileInfo.filePath() << "as source audio input file";
        qCritical().noquote() << "Could not open the PCM audio file" << inputAudioFileInfo.filePath();
        return false;
    }

    // Get the length of the PCM audio file
    audioFileByteLength = inputAudioFileInfo.size();
    if (audioFileByteLength == 0) {
        tbcDebugStream() << "Could get file size of" << inputAudioFileInfo.filePath() << "(or file was 0 bytes length)";
        qCritical().noquote() << "The PCM audio file" << inputAudioFileInfo.filePath() << "is empty";
        inputAudioFile.close();
        return false;
    }

    return true;
}

// Close an audio source file
void SourceAudio::close()
{
    ldx.reset();
    // Close the audio source data file
    inputAudioFile.close();
    audioFileByteLength = 0;
}

SourceAudio::Data SourceAudio::getAudioDataPadded(qint32 startSample, qint32 numberOfSamples, bool *shortRead)
{
    Data result;
    if (shortRead) *shortRead = false;
    if (numberOfSamples < 1) return result;
    const qint64 available = audioFileByteLength / 4;
    const qint64 start = qMax<qint64>(0, startSample);
    const qint64 readable = start >= available ? 0 : qMin<qint64>(numberOfSamples, available - start);
    if (readable > 0) {
        if (ldx) {
            result = ldx->getAudio(start, readable);
        } else if (inputAudioFile.seek(start * 4)) {
            // (the samples are read as they are in the file: 16-bit, little-endian, a left and a right for each)
            const QByteArray bytes = inputAudioFile.read(readable * 4);
            const qsizetype count = bytes.size() / 2;
            result.resize(count);
            for (qsizetype i = 0; i < count; i++) result[i] = qFromLittleEndian<qint16>(bytes.constData() + 2 * i);
        }
    }
    if (readable < numberOfSamples && shortRead) *shortRead = true;
    // (what the file does not have is silence)
    result.resize(static_cast<qsizetype>(numberOfSamples) * 2);
    return result;
}

// Get audio data for a single field from the audio source file
SourceAudio::Data SourceAudio::getAudioData(qint32 startSample, qint32 numberOfSamples)
{
    // Create a buffer for the sample data
    SourceAudio::Data sampleData;

    // Check that audio data is available
    if (audioFileByteLength == 0) {
        qFatal("getAudioData requested, but no sample data is available (not open?)!");
        return sampleData;
    }

    // Translate the start and number from stereo 16-bit pair samples to bytes (x4)
    qint64 startByte = static_cast<qint64>(startSample) * 4;
    qint64 lengthInBytes = static_cast<qint64>(numberOfSamples) * 4;

    // Range check the request
    if (startByte < 0) {
        qFatal("getAudioData requested, but startSample was less than 0!");
        return sampleData;
    }

    if (lengthInBytes < 1) {
        qFatal("getAudioData requested, but numberOfSamples was less than 1!");
        return sampleData;
    }

    if ((startByte + lengthInBytes) > audioFileByteLength) {
        qFatal("getAudioData requested, but startSample + numberOfSamples was out of bounds!");
        return sampleData;
    }

    if (ldx) return ldx->getAudio(startSample, numberOfSamples);

    // Seek to the correct file position (if not already there)
    if (!inputAudioFile.seek(startByte)) {
        // Seek failed
        qFatal("Could not seek to field position in input audio file!");
        return sampleData;
    }

    // Define a data stream to read 16 bit values from the audio file
    QDataStream audioData(&inputAudioFile);
    audioData.setByteOrder(QDataStream::LittleEndian);
    qint16 x;
    for (qint32 sample = 0; sample < numberOfSamples; sample++) {
        // Left 16 bit sample
        audioData >> x;
        sampleData.append(x);
        if (audioData.atEnd()) {
            qWarning("getAudioData hit end of file!");
            return sampleData;
        }

        // Right 16 bit sample
        audioData >> x;
        sampleData.append(x);
        if (audioData.atEnd()) {
            qWarning("getAudioData hit end of file!");
            return sampleData;
        }
    }

    return sampleData;
}

