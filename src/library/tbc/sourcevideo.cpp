/************************************************************************

    sourcevideo.cpp

    ld-decode-tools TBC library
    Copyright (C) 2018-2019 Simon Inns

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

#include "sourcevideo.h"

#include <cstdio>
#include <QFileInfo>
#include "tbc/logging.h"

// Class constructor
SourceVideo::SourceVideo()
{
    // Default object settings
    isSourceVideoOpen = false;
    inputFilePos = -1;
    availableFields = -1;
    fieldLength = -1;
    fieldByteLength = -1;
    fieldLineLength = -1;
    isCvbs = false;
    cvbsPadValue = 0;

    // Set up the cache
    fieldCache.setMaxCost(100);
}

SourceVideo::~SourceVideo()
{
    // The reader of a stream must stop before the file it reads is closed (when the last
    // SourceVideo that shares it lets go of it)
    stream.reset();
    if (isSourceVideoOpen) inputFile.close();
}

void SourceVideo::setStreamOptions(const StreamReader::Options &options)
{
    streamOptions = options;
}

bool SourceVideo::isStream() const
{
    return stream != nullptr;
}

bool SourceVideo::shareStreamWith(const SourceVideo &other)
{
    if (isSourceVideoOpen || !other.stream) return false;

    stream = other.stream;
    fieldLength = other.fieldLength;
    fieldByteLength = other.fieldByteLength;
    fieldLineLength = other.fieldLineLength;
    availableFields = -1;
    isCvbs = false;
    isSourceVideoOpen = true;
    return true;
}

qint32 SourceVideo::getFieldsReadSoFar()
{
    return stream ? stream->fieldsRead() : 0;
}

qint32 SourceVideo::getFirstFieldHeld()
{
    return stream ? stream->firstFieldKept() + 1 : 0;
}

qint32 SourceVideo::getFramesReadAhead()
{
    return stream ? stream->framesAhead() : 0;
}

qint32 SourceVideo::getMaxFramesReadAhead()
{
    return stream ? stream->maxFramesAhead() : 0;
}

bool SourceVideo::hasField(qint32 fieldNumber)
{
    if (stream) return stream->hasField(fieldNumber - 1);
    return availableFields == -1 || (fieldNumber >= 1 && fieldNumber <= availableFields);
}

// Source Video file manipulation methods -----------------------------------------------------------------------------

// Open an input video data file. If filename is "-", read from stdin.
// Returns true on success.
bool SourceVideo::open(QString filename, qint32 _fieldLength, qint32 _fieldLineLength)
{
    fieldLength = _fieldLength;
    fieldByteLength = _fieldLength * 2;
    if (_fieldLineLength != -1) {
        fieldLineLength = _fieldLineLength * 2;
    } else fieldLineLength = -1;
    tbcDebugStream() << "SourceVideo::open(): Called with field byte length =" << fieldByteLength;

    if (isSourceVideoOpen) {
        // Video file is already open, close it
        qInfo() << "A source video input file is already open, cannot open a new one";
        return false;
    }

    // Open the source video file
    inputFile.setFileName(filename);
    isCvbs = false;
    if (filename != "-" && Cvbs::isDataFileName(filename)) {
        if (!openCvbs(filename)) return false;
    } else if (filename == "-" || (QFileInfo::exists(filename) && !QFileInfo(filename).isFile())) {
        // Standard input, or a named pipe: read ahead on a thread of its own. We
        // don't know how long the input will be.
        if (streamOptions.cvbs && Cvbs::geometry(streamOptions.system).fieldLength() != fieldLength) {
            qWarning() << "The CVBS stream is" << Cvbs::presetName(streamOptions.system)
                       << "but the video parameters describe fields of" << fieldLength << "samples";
            return false;
        }

        FILE *streamFile = stdin;
        if (filename != "-") {
            streamFile = fopen(QFile::encodeName(filename).constData(), "rb");
            if (!streamFile) {
                qWarning() << "Could not open" << filename << "as source video input file";
                return false;
            }
        }
        stream = std::make_unique<StreamReader>(streamFile, filename != "-", fieldLength, streamOptions);
        stream->start();
        availableFields = -1;
        tbcDebugStream() << "SourceVideo::open(): Reading a stream with a buffer of" << streamOptions.bufferFrames << "frames";
    } else {
        if (!inputFile.open(QIODevice::ReadOnly)) {
            // Failed to open named input file
            qWarning() << "Could not open" << filename << "as source video input file";
            return false;
        }

        // File open successful - configure source video parameters
        qint64 tAvailableFields = (inputFile.size() / fieldByteLength);
        availableFields = static_cast<qint32>(tAvailableFields);
        tbcDebugStream() << "SourceVideo::open(): Successful -" << availableFields << "fields available";
    }

    // Initialise cache
    fieldCache.clear();

    isSourceVideoOpen = true;
    inputFilePos = 0;

    return true;
}

// Close an input video data file
void SourceVideo::close()
{
    if (!isSourceVideoOpen) {
        tbcDebugStream() << "SourceVideo::close(): Called but no source video input file is open";
        return;
    }

    tbcDebugStream() << "SourceVideo::close(): Called, closing the source video file and emptying the frame cache";
    stream.reset();
    inputFile.close();
    isSourceVideoOpen = false;
    inputFilePos = -1;
    isCvbs = false;
    cvbsConverter.reset();

    tbcDebugStream() << "SourceVideo::close(): Source video input file closed";
}

// Get the validity of the source video file
bool SourceVideo::isSourceValid()
{
    return isSourceVideoOpen;
}

// Get the number of fields available from the source video file.
// Returns -1 if the length is unknown (e.g. we're reading from stdin).
qint32 SourceVideo::getNumberOfAvailableFields()
{
    // A stream's length is only known once it has ended
    if (stream) return stream->numberOfFields();
    return availableFields;
}

// Get the number of samples in a field
qint32 SourceVideo::getFieldLength()
{
    return fieldLength;
}

// Frame data retrieval methods ---------------------------------------------------------------------------------------

// Method to retrieve a range of field lines from a single video field.
// If startFieldLine and endFieldLine are both -1, read the whole field.
SourceVideo::Data SourceVideo::getVideoField(qint32 fieldNumber, qint32 startFieldLine, qint32 endFieldLine)
{
    // Adjust the field number to index from zero
    fieldNumber--;

    // Ensure source video is open
    if (!isSourceVideoOpen) qFatal("Application requested TBC field before opening TBC file - Fatal error");

    if (stream) {
        Data field;
        if (!stream->getField(fieldNumber, field)) {
            qFatal("The input stream ended after %d fields, but field %d was asked for - "
                   "the metadata describes more video than the stream holds",
                   stream->fieldsRead(), fieldNumber + 1);
        }
        if (startFieldLine == -1 && endFieldLine == -1) return field;

        if (fieldLineLength == -1) qFatal("Application did not set field line length when opening TBC file");
        const qint32 lineSamples = fieldLineLength / 2;
        if (startFieldLine < 1 || endFieldLine < startFieldLine || endFieldLine * lineSamples > field.size()) {
            qFatal("Application requested out-of-bounds field line");
        }
        return field.mid((startFieldLine - 1) * lineSamples, (endFieldLine - startFieldLine + 1) * lineSamples);
    }

    if (isCvbs) {
        // Fields come from whole frames, which are cached as two fields
        if (fieldNumber < 0 || fieldNumber >= availableFields) {
            qFatal("Application requested field that exceeds the boundaries of the input CVBS file");
        }
        if (!fieldCache.contains(fieldNumber)) readCvbsFrame(fieldNumber / 2);
        const Data &field = *fieldCache.object(fieldNumber);
        if (startFieldLine == -1 && endFieldLine == -1) return field;

        if (fieldLineLength == -1) qFatal("Application did not set field line length when opening TBC file");
        const qint32 lineSamples = fieldLineLength / 2;
        if (startFieldLine < 1 || endFieldLine < startFieldLine || endFieldLine * lineSamples > field.size()) {
            qFatal("Application requested out-of-bounds field line");
        }
        return field.mid((startFieldLine - 1) * lineSamples, (endFieldLine - startFieldLine + 1) * lineSamples);
    }

    // Calculate the position of the require field line data
    qint64 requiredStartPosition = static_cast<qint64>(fieldByteLength) * static_cast<qint64>(fieldNumber);
    qint64 requiredReadLength;

    if (startFieldLine == -1 && endFieldLine == -1) {
        // Read the whole field

        // Check the cache (we only cache whole fields)
        if (fieldCache.contains(fieldNumber)) {
            return *fieldCache.object(fieldNumber);
        }

        requiredReadLength = static_cast<qint64>(fieldByteLength);
    } else {
        // Read a range of lines

        // Adjust the field line range to index from zero
        startFieldLine--;
        endFieldLine--;

        // Verify the required range
        if (fieldLineLength == -1) qFatal("Application did not set field line length when opening TBC file");
        if (startFieldLine < 0) qFatal("Application requested out-of-bounds field line");

        requiredStartPosition += static_cast<qint64>(fieldLineLength) * static_cast<qint64>(startFieldLine);
        requiredReadLength = static_cast<qint64>(endFieldLine - startFieldLine + 1) * static_cast<qint64>(fieldLineLength);
    }

    // Check the requested field and lines are valid
    if (availableFields != -1
        && (requiredStartPosition < 0
            || requiredStartPosition + requiredReadLength > (static_cast<qint64>(fieldByteLength) * availableFields))) {
        qFatal("Application requested field line range that exceeds the boundaries of the input TBC file");
    }

    // Resize the output buffer
    outputFieldData.resize(static_cast<qint32>(requiredReadLength) / 2);

    // Seek to the correct file position (if not already there)
    if (inputFilePos != requiredStartPosition) {
        if (!inputFile.seek(requiredStartPosition)) {
            // Seek failed

            if (inputFilePos > requiredStartPosition) {
                qFatal("Could not seek backwards to required field position in input TBC file");
            } else {
                // Seeking forwards -- try reading and discarding data instead
                qint64 discardBytes = requiredStartPosition - inputFilePos;
                while (discardBytes > 0) {
                    qint64 readBytes = inputFile.read(reinterpret_cast<char *>(outputFieldData.data()),
                                                      qMin(discardBytes, static_cast<qint64>(outputFieldData.size() * 2)));
                    if (readBytes <= 0) {
                        qFatal("Could not seek or read forwards to required field position in input TBC file");
                    }
                    discardBytes -= readBytes;
                }
            }
        }
        inputFilePos = requiredStartPosition;
    }

    // Read the field lines from the input
    qint64 totalReceivedBytes = 0;
    qint64 receivedBytes = 0;
    do {
        receivedBytes = inputFile.read(reinterpret_cast<char *>(outputFieldData.data()) + totalReceivedBytes,
                                       requiredReadLength - totalReceivedBytes);
        if (receivedBytes > 0) {
            totalReceivedBytes += receivedBytes;
            inputFilePos += receivedBytes;
        }
    } while (receivedBytes > 0 && totalReceivedBytes < requiredReadLength);

    // Verify read was ok
    if (totalReceivedBytes != requiredReadLength) qFatal("Could not read field data from input TBC file");

    if (startFieldLine == -1 && endFieldLine == -1) {
        // Insert the field data into the cache
        fieldCache.insert(fieldNumber, new Data(outputFieldData), 1);
    }

    // Return the data
    return outputFieldData;
}














// CVBS input ---------------------------------------------------------------------------------------------------------

// Open a .cvbs, .cvbsy or .cvbsc file. The sample encoding comes from the
// <base>.meta metadata; without it, the default encoding (CVBS_U10_4FSC) is
// assumed and the video system is deduced from the field length.
bool SourceVideo::openCvbs(const QString &filename)
{
    VideoSystem system;
    Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
    const QString metaFilename = Cvbs::metaFileName(filename);

    if (QFileInfo::exists(metaFilename)) {
        Cvbs::FileMetadata fileMetadata;
        if (!fileMetadata.read(metaFilename)) return false;
        system = fileMetadata.system;
        encoding = fileMetadata.sampleEncoding;
    } else if (!Cvbs::systemForFieldLength(fieldLength, system)) {
        qWarning() << "Cannot tell the video system of" << filename << "- it has no metadata";
        return false;
    } else {
        qInfo().noquote() << filename << "has no" << metaFilename << "- assuming"
                          << Cvbs::sampleEncodingName(encoding) << "samples";
    }

    cvbsGeometry = Cvbs::geometry(system);
    if (cvbsGeometry.fieldLength() != fieldLength) {
        qWarning() << "The metadata field size does not match the" << Cvbs::presetName(system) << "CVBS file" << filename;
        return false;
    }
    cvbsConverter = std::make_unique<Cvbs::SampleConverter>(encoding, system);

    // Pad the second field's extra line with blanking (chroma: its zero level)
    const qint32 padLevel = Cvbs::channel(filename) == Cvbs::Channel::Chroma ? 512
                                                                              : Cvbs::standardLevels(system).blanking;
    cvbsPadValue = static_cast<quint16>(padLevel * 64);

    if (!inputFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open" << filename << "as source video input file";
        return false;
    }

    const qint64 frameBytes = cvbsGeometry.frameSamples * 2;
    availableFields = static_cast<qint32>(inputFile.size() / frameBytes) * 2;
    if (inputFile.size() % frameBytes != 0) {
        qWarning() << filename << "ends with a partial frame, which is ignored";
    }
    cvbsFrameData.resize(static_cast<qint32>(cvbsGeometry.frameSamples));
    isCvbs = true;

    tbcDebugStream() << "SourceVideo::openCvbs(): Successful -" << availableFields << "fields available";
    return true;
}

// Read a stored frame (0-based) and cache it as its two fields
void SourceVideo::readCvbsFrame(qint32 frameNumber)
{
    const qint64 frameBytes = cvbsGeometry.frameSamples * 2;
    const qint64 position = frameBytes * frameNumber;
    if (inputFilePos != position) {
        if (!inputFile.seek(position)) qFatal("Could not seek to required frame position in input CVBS file");
        inputFilePos = position;
    }

    if (inputFile.read(reinterpret_cast<char *>(cvbsFrameData.data()), frameBytes) != frameBytes) {
        qFatal("Could not read frame data from input CVBS file");
    }
    inputFilePos += frameBytes;

    Data *field1 = new Data(cvbsGeometry.fieldLength());
    Data *field2 = new Data(cvbsGeometry.fieldLength());
    Cvbs::frameToFields(cvbsFrameData.constData(), cvbsGeometry, *cvbsConverter, cvbsPadValue,
                        field1->data(), field2->data());
    fieldCache.insert(frameNumber * 2, field1, 1);
    fieldCache.insert(frameNumber * 2 + 1, field2, 1);
}
