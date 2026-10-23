/******************************************************************************
 * framemap.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FRAMEMAP_H
#define FRAMEMAP_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// The map of a capture: for each frame of the capture put in the order of the disc (what ld-discmap works out), which frame of the capture goes there
// and the number it has on the disc. It is kept as runs, so that a capture without a skip is one run and a disc with thirty skips is sixty (see
// audit-step2-mapping.md in docs/Development, and the specification of the container for the object in a file).
//
// Where the number or the place of a frame comes from is kept too, one letter for each run:
enum class MapBasis : char {
    Measured = 'm',       // the number was read from the VBI of the frame itself, and the two lines carrying it agree
    Unconfirmed = 'u',    // read, but from one line only, or the lines disagreed and one was kept
    Sequence = 's',       // the VBI gave another number, and the numbers of the neighbours replaced it
    Pulldown = 'd',       // an NTSC CAV pulldown frame, numbered by the 1-in-5 pattern
    Guessed = 'g',        // a frame with no number that could not be given one
    Padded = 'p'          // no frame of the capture: the gap between the numbers on each side of it
};

QChar mapBasisLetter(MapBasis basis);
bool mapBasisFromLetter(QChar letter, MapBasis &basis);

// One frame of the view of a map
struct MapFrame {
    qint32 discFrame = 0;          // the number on the disc
    qint32 captureFrame = 0;       // the frame of the capture (from 1); 0: there is none, the frame is padded (two black fields)
    MapBasis basis = MapBasis::Measured;
    bool unusable = false;         // kept, but its fields are read with a dropout over every line (a frame that could not be numbered)

    bool operator==(const MapFrame &other) const {
        return discFrame == other.discFrame && captureFrame == other.captureFrame && basis == other.basis && unusable == other.unusable;
    }
};

class JsonReader;
class JsonWriter;

class FrameMap
{
public:
    // A run of frames: the numbers of the disc and the frames of the capture each go up by one (the frames of a padded run are all 0)
    struct Run {
        qint32 discFrame = 0;
        qint32 captureFrame = 0;
        qint32 count = 0;
        MapBasis basis = MapBasis::Measured;
    };

    // What the map was made for: a reader that finds another capture refuses the map
    qint64 captureFields = 0;
    qint64 captureSize = 0;
    QString madeBy;
    // How the capture was read: the fields of a frame in the reverse order (second field first), and the kind of disc ("CAV" or "CLV"), which the numbers of the
    // disc are written back from
    bool reverseFields = false;
    QString discType;

    // The frames of the view, from the first: put them in runs (runs of frames that follow each other, with the same basis)
    static FrameMap fromFrames(const QVector<MapFrame> &frames);
    // The frames of the view
    QVector<MapFrame> frames() const;

    qint32 frameCount() const;
    // The frame at a place of the view (from 1); a padded frame with the number 0 if the place is not in the map
    MapFrame frameAt(qint32 place) const;
    // How many frames of each basis (index: the letter m, u, s, d, g, p as 0..5)
    QVector<qint32> basisCounts() const;
    // The share of the frames, from 0 to 1, whose number was measured (m)
    double measuredShare() const;

    const QVector<Run> &runs() const { return runList; }
    // The places (from 1) of the unusable frames
    const QVector<qint32> &unusable() const { return unusablePlaces; }

    // The object (the same wherever it is kept: in a session, or in a track of an LDX file)
    QJsonObject toJson() const;
    QByteArray toJsonBytes() const;
    static bool fromJson(const QJsonObject &object, FrameMap &map, QString &error);
    static bool fromJsonBytes(const QByteArray &bytes, FrameMap &map, QString &error);

    // The same object in the JSON of the metadata of a capture (the member "ldxMap" of the file), streamed as the metadata are
    void write(JsonWriter &writer) const;
    bool read(JsonReader &reader, QString &error);

    // Whether the map is good for a capture of these fields and this size
    bool isFor(qint64 fields, qint64 size) const { return fields == captureFields && size == captureSize; }

    bool operator==(const FrameMap &other) const;

private:
    QVector<Run> runList;
    QVector<qint32> unusablePlaces;
    // The first place of each run, to find a place by bisection
    QVector<qint32> runStarts;
    void indexRuns();
};

#endif // FRAMEMAP_H
