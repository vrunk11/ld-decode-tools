/******************************************************************************
 * cvbs.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CVBS_H
#define CVBS_H

#include <QString>
#include <QVector>
#include <functional>
#include <optional>

#include "dropouts.h"
#include "lddecodemetadata.h"

// Support for the CVBS file format used by decode-orc and the decode family
// (https://github.com/decode-orc/cvbs-file-format-specification), so the tools
// can read captures written by decode-orc and write captures decode-orc reads.
//
// A CVBS capture is <base>.cvbs (or a <base>.cvbsy / <base>.cvbsc Y/C pair)
// holding whole frames, plus the optional <base>.meta SQLite metadata and the
// <base>.dropouts.meta dropout extension. The tools themselves work on fields
// in the 16-bit TBC domain, so frames are split into two TBC-shaped fields on
// reading and joined again on writing. The 16-bit TBC domain is the CVBS 10-bit
// domain multiplied by 64, so the sample conversion is exact.
//
// Per-field metadata (VBI, VITS, ...) has no place in the CVBS core metadata;
// the tools keep it in a companion <base>.cvbs.db or <base>.cvbs.json file in
// the usual ld-decode format, next to the files decode-orc reads.
namespace Cvbs {

// Sample encoding presets (how sample words are stored)
enum class SampleEncoding {
    U10_4FSC,    // 10-bit values in signed 16-bit words (the default)
    U16_4FSC,    // 10-bit values shifted left by 6 in unsigned 16-bit words
    TPG21_4FSC,  // (value - 508) * 64 in signed 16-bit words
    S16_4FSC,    // (value - blanking) * 32 in signed 16-bit words
};

QString sampleEncodingName(SampleEncoding encoding);
bool parseSampleEncodingName(const QString &name, SampleEncoding &encoding);

// The CVBS name of a video system ("PAL", "NTSC", "PAL_M")
QString presetName(VideoSystem system);

// Which signal a data file carries
enum class Channel {
    Composite,  // .cvbs
    Luma,       // .cvbsy
    Chroma,     // .cvbsc
};

// File naming --------------------------------------------------------------------------------------------------------

// True for a .cvbs, .cvbsy or .cvbsc file name
bool isDataFileName(const QString &fileName);
Channel channel(const QString &dataFileName);
// <base> for <base>.cvbs, <base>.cvbsy and <base>.cvbsc
QString baseName(const QString &dataFileName);
QString dataFileName(const QString &baseName, Channel channel);
// The other file of a Y/C pair (.cvbsy <-> .cvbsc); empty for composite
QString pairedDataFileName(const QString &dataFileName);
QString metaFileName(const QString &dataFileName);
QString dropoutsFileName(const QString &dataFileName);
// The companion ld-decode metadata file, <base>.cvbs.db or <base>.cvbs.json
QString companionFileName(const QString &dataFileName, bool json);

// For a metadata file name (<base>.meta or a companion), the data file it
// belongs to: an existing .cvbs, .cvbsy or .cvbsc, or <base>.cvbs.
// Returns an empty string if the name is not CVBS metadata.
QString dataFileForMetadata(const QString &metadataFileName);

// True if the file is CVBS core metadata (SQLite with a cvbs_file table)
bool isCoreMetadataFile(const QString &fileName);

// Geometry -----------------------------------------------------------------------------------------------------------

// The layout of a stored frame and of the TBC fields it maps to
struct Geometry {
    qint32 lineWidth = 0;      // samples per line (nominal 1135 for PAL)
    qint32 field1Lines = 0;    // lines of the first field (313 / 263)
    qint32 field2Lines = 0;    // lines of the second field (312 / 262)
    qint32 extraSamples = 0;   // PAL: 2 samples ending each field's last line
    qint64 frameSamples = 0;   // samples per stored frame

    // TBC fields are field1Lines high; the second has one padding line
    qint32 fieldHeight() const { return field1Lines; }
    qint32 fieldLength() const { return lineWidth * field1Lines; }
    // Offset of a frame line (0-based, across both fields) in a stored frame
    qint64 lineOffset(qint32 frameLine) const;
};

Geometry geometry(VideoSystem system);

// The video system whose TBC field length is fieldLength
bool systemForFieldLength(qint32 fieldLength, VideoSystem &system);

// Standard 10-bit levels of a video system
struct Levels {
    qint32 blanking;
    qint32 black;
    qint32 white;
};
Levels standardLevels(VideoSystem system);

// Core metadata (<base>.meta) ----------------------------------------------------------------------------------------

struct FileMetadata {
    VideoSystem system;
    SampleEncoding sampleEncoding = SampleEncoding::U10_4FSC;
    QString signalStatePreset = "STANDARD_STABLE_UNLOCKED";
    std::optional<bool> sequenceContinuous;
    QString signalType = "composite";
    QString decoder = "other";
    QString gitBranch;
    QString gitCommit;
    qint32 numberOfFrames = -1;            // -1 when not recorded
    std::optional<qint32> blackLevel;       // 10-bit black level override
    std::optional<bool> hasNonstandardValues;
    QString captureNotes;

    FileMetadata();

    bool isPhaseLocked() const;
    // Time-base stable at 4fsc: the only states the tools can process
    bool isStandardStable() const;

    // Read or write <base>.meta. Writing an existing file updates its
    // cvbs_file row and keeps any other tables (audio channel pairs).
    // Both log the reason and return false on failure.
    bool read(const QString &fileName);
    bool write(const QString &fileName) const;
};

// Record an audio channel pair (0-7, matching <base>_audio_<pair>.wav) in the
// core metadata, replacing any row for that pair. Returns false, having logged
// why, on failure.
bool setAudioChannelPair(const QString &metaFileName, qint32 pair, const QString &description);

// Sample conversion --------------------------------------------------------------------------------------------------

// Converts between stored sample words and the 16-bit TBC domain
class SampleConverter
{
public:
    SampleConverter(SampleEncoding encoding, VideoSystem system);

    quint16 toTbc(qint16 stored) const;
    qint16 fromTbc(quint16 tbc) const;

    // A linear level correction applied to TBC values before they are
    // stored (tbc * scale + offset, in the 16-bit domain), so that values
    // falling below zero keep the headroom the encoding has
    void setLevelMap(double scale, double offset);

private:
    SampleEncoding encoding;
    qint32 blanking;
    double levelScale = 1.0;
    double levelOffset = 0.0;
    bool hasLevelMap = false;
};

// Split a stored frame into two TBC fields of geometry.fieldLength() samples.
// The second field's padding line is filled with padValue and PAL's extra
// samples are dropped.
void frameToFields(const qint16 *frame, const Geometry &geometry, const SampleConverter &converter,
                   quint16 padValue, quint16 *field1, quint16 *field2);
// Join two TBC fields into a stored frame of geometry.frameSamples samples,
// dropping the padding line and interpolating PAL's extra samples.
void fieldsToFrame(const quint16 *field1, const quint16 *field2, const Geometry &geometry,
                   const SampleConverter &converter, qint16 *frame);

// Field phase IDs -----------------------------------------------------------------------------------------------------

// CVBS captures carry no per-field metadata, so the phase ID of each field (1-4
// for NTSC, 1-8 for PAL and PAL-M: its position in the colour field sequence)
// is measured from the colour burst in the samples, as decode-orc does. The
// burst is read from the first lines of each field, which is a small part of
// the file. setPhaseID is called for each 0-based field index with the phase
// ID, or -1 where the burst cannot be measured. isChroma selects a .cvbsc
// file, whose zero level is 512. Returns false if the file cannot be read.
bool measureFieldPhaseIDs(const QString &dataFileName, VideoSystem system, SampleEncoding encoding, bool isChroma,
                          qint32 numberOfFrames, const std::function<void(qint32, qint32)> &setPhaseID);

// Fill the fields whose phase ID could not be measured (-1) with the one the colour
// sequence expects there: it goes up by one for each field, from 1 to modulus and
// round again. Phase IDs that were measured are never changed, as a measurement that
// differs from the expected one is most likely a real break in the sequence. The
// expected IDs follow from the nearest measured field, before it or after it. anchorPhase
// (if not -1) is the phase of the field at anchorIndex, which a run of fields follows on
// from: either one of the fields (taken as measured if it has no ID), or the field just
// before the first (anchorIndex -1). Fields with nothing to follow from stay at -1.
void fillPhaseGaps(QVector<qint32> &phaseIDs, qint32 modulus, qint32 anchorIndex = -1, qint32 anchorPhase = -1);

// The phase ID of a field of TBC samples (a whole field, its first lines at least),
// measured the same way; for video that comes with no metadata. -1 if it cannot
// be measured.
qint32 measureFieldPhaseID(const quint16 *field, VideoSystem system, bool isFirstField);

// Dropout extension (<base>.dropouts.meta) ---------------------------------------------------------------------------

// Read the per-frame dropout runs as per-field TBC dropouts (1-based field
// lines, inclusive end). setFieldDropOuts is called for each field with
// dropouts, with the 0-based field index. Returns false if the file exists
// but cannot be read.
bool readDropouts(const QString &fileName, VideoSystem system, qint32 numberOfFrames,
                  const std::function<void(qint32, const DropOuts &)> &setFieldDropOuts);
// Write per-field TBC dropouts as per-frame runs. fieldDropOuts returns the
// dropouts of a 0-based field index.
bool writeDropouts(const QString &fileName, VideoSystem system, qint32 numberOfFrames,
                   const std::function<const DropOuts &(qint32)> &fieldDropOuts);

} // namespace Cvbs

#endif // CVBS_H
