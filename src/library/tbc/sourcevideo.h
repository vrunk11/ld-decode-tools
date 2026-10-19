/************************************************************************

    sourcevideo.h

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

#ifndef SOURCEVIDEO_H
#define SOURCEVIDEO_H

#include <QFile>
#include <QCache>
#include <QDebug>
#include <QVector>
#include <memory>

#include "cvbs.h"
#include "streamreader.h"

class StackedVideo;
class LdxCapture;

class SourceVideo
{
public:
    // A QVector of timebase-corrected video samples.
    // This is usually a complete field, but it may be a partial field if
    // you've requested fewer lines from getVideoField (or if you've sliced it
    // yourself).
    using Data = QVector<quint16>;

    SourceVideo();
    ~SourceVideo();

    // Prevent copying or assignment
    SourceVideo(const SourceVideo &) = delete;
    SourceVideo& operator=(const SourceVideo &) = delete;

    // A .stack file (a stacking session) is read as the TBC file of the stack of its sources, which are
    // stacked as the fields are asked for (see StackedVideo); its metadata is read by LdDecodeMetaData.
    // "capture.ldx" or "capture.ldx#3" (a track of an LDX file) is read as the TBC file of that track, or, for an LDX
    // file that keeps a stacking session and no track named, as the stack (see LdxCapture and StackedVideo).
    // File handling methods. A .cvbs, .cvbsy or .cvbsc file is read as TBC
    // fields: each stored frame becomes two fields of _fieldLength samples.
    // "-" (standard input), or a named pipe, is read as a stream: see StreamReader.
    // Fields can then only be asked for in about the order they come in (within
    // the buffer), and the stream may end sooner than the metadata says.
    bool open(QString filename, qint32 _fieldLength, qint32 _fieldLineLength = -1);
    void close(void);

    // How a stream is read: TBC fields (the default) or CVBS frames, and the buffer
    // size. Call before open().
    void setStreamOptions(const StreamReader::Options &options);
    bool isStream() const;
    // Read the same stream as another SourceVideo, which stays the one that opened it (and
    // must outlive this). For several threads each decoding frames of one live stream.
    bool shareStreamWith(const SourceVideo &other);
    // Fields read from the stream so far, and the first one (numbered from 1) still held;
    // 0 for a source that is not a stream
    qint32 getFieldsReadSoFar();
    qint32 getFirstFieldHeld();
    // A CVBS stream whose first frame has samples beyond 10 bits
    bool streamLooksWiderThan10Bit();
    // A stream read as a ring (see StreamReader): hold on to the fields first to last (numbered from 1,
    // and those between) against being overwritten, and let them go again
    void protectFields(qint32 firstField, qint32 lastField);
    void clearProtection();
    // Whether the field is still held, and the first held one at or after it (-1 if none)
    bool isFieldHeld(qint32 fieldNumber);
    qint32 firstFieldHeldAtOrAfter(qint32 fieldNumber);

    // Field handling methods
    Data getVideoField(qint32 fieldNumber, qint32 startFieldLine = -1, qint32 endFieldLine = -1);
    // True if the field (numbered from 1) exists. Waits for it, if it is not yet read
    // from a stream that is still going; a false means the stream ended before it.
    bool hasField(qint32 fieldNumber);

    // A program that shows video (ld-analyse) does not stop on a capture that is cut short, or whose metadata describe more video than
    // it holds: a field that is not there is given blank (and a warning said), instead of the fatal error the tools that process it give
    void setLenient(bool lenient) { lenientReads = lenient; }
    bool isLenient() const { return lenientReads; }

    // Get and set methods
    bool isSourceValid();
    qint32 getNumberOfAvailableFields();
    qint32 getFieldLength();

private:
    // File handling globals
    QFile inputFile;
    qint64 inputFilePos;
    bool isSourceVideoOpen;
    bool lenientReads = false;
    Data blankField(qint32 startFieldLine, qint32 endFieldLine) const;
    qint32 availableFields;
    qint32 fieldLength;
    qint32 fieldByteLength;
    qint32 fieldLineLength;

    Data outputFieldData;

    // Field caching
    QCache<qint32, Data> fieldCache;

    // Stream input (standard input or a named pipe)
    StreamReader::Options streamOptions;
    std::shared_ptr<StreamReader> stream;

    // A stacking session (.stack)
    std::unique_ptr<StackedVideo> stacked;

    // A capture kept in an LDX file (a track of it)
    std::unique_ptr<LdxCapture> ldx;

    // CVBS input (.cvbs, .cvbsy, .cvbsc): whole frames, split into fields
    bool isCvbs;
    Cvbs::Geometry cvbsGeometry;
    std::unique_ptr<Cvbs::SampleConverter> cvbsConverter;
    quint16 cvbsPadValue;
    QVector<qint16> cvbsFrameData;

    bool openCvbs(const QString &filename);
    void readCvbsFrame(qint32 frameNumber);
};

#endif // SOURCEVIDEO_H
