/******************************************************************************
 * decodingparameters.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef DECODINGPARAMETERS_H
#define DECODINGPARAMETERS_H

#include <QByteArray>
#include <QMap>
#include <QString>

class JsonReader;
class JsonWriter;

// How a person chose to decode a capture to colour (the chroma decoder and its settings), kept with the metadata of the capture (as the map is): the member
// "ldxDecoding" of the JSON (so in the metadata blocks of an LDX file) or the table decoding of a SQLite file. ld-analyse and ld-chroma-decoder take it as
// their default for the capture; what is said on the command line, or in the window, has the last word. ld-stacker-gui does not use it (it has several sources).
struct DecodingParameters {
    // pal2d, transform2d, transform3d, ntsc1d, ntsc2d, ntsc3d, ntsc3dnoadapt, hvd2d, hvd3d or mono (the names of ld-chroma-decoder --decoder)
    QString decoder;
    // The settings of the decoder, by name: lumaNR, chromaNR, chromaGain, chromaPhase, adaptThreshold, chromaWeight (numbers), phaseCompensation, simplePAL (0 or 1), hvdIterations, hvdStrength, hvdOverlap, hvdPasses, hvdFieldOrder, hvdSmoothness, hvdSubcarrierHz (numbers), hvdFast, hvdBidirectional, hvdSelective, hvdAcc, hvdMonochrome, hvdSymmetry (0 or 1), for hvd2d and hvd3d
    QMap<QString, double> settings;
    QString madeBy;

    bool has(const QString &name) const { return settings.contains(name); }
    double value(const QString &name, double fallback = 0.0) const { return settings.value(name, fallback); }

    // The JSON object, as bytes (for the SQLite table), and read back
    QByteArray toJsonBytes() const;
    static bool fromJsonBytes(const QByteArray &bytes, DecodingParameters &parameters, QString &error);
    // The same, streamed in the JSON of the metadata
    void write(JsonWriter &writer) const;
    bool read(JsonReader &reader, QString &error);

    bool operator==(const DecodingParameters &other) const {
        return decoder == other.decoder && settings == other.settings && madeBy == other.madeBy;
    }

    // Whether the capture is from a tape (VHS and the like): the metadata give a tape format, or say that vhs-decode made it
    static bool isTape(const QString &tapeFormat, const QString &decoder);
    // Whether a capture that has no parameters of its own is decoded in 2D by default: a stack of sources from tapes (the timing of a tape is not steady enough for the 3D
    // filters to line the sources up), where a stack of LaserDiscs is decoded in 3D like one capture
    static bool prefers2D(bool stackOfSources, bool tape);
    // The decoder: 3D (transform3d, ntsc3d) or 2D (transform2d, ntsc2d)
    static QString defaultDecoder(bool ntsc, bool twoD);
};

#endif // DECODINGPARAMETERS_H
