/******************************************************************************
 * lddecodemetadata.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2022 Ryan Holtz
 * SPDX-FileCopyrightText: 2022-2023 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDDECODEMETADATA_H
#define LDDECODEMETADATA_H

#include <QString>
#include <QVector>
#include <QTemporaryFile>
#include <QDebug>
#include <array>
#include <memory>
#include <functional>

#include "dropouts.h"
#include "framemap.h"

class JsonReader;
class JsonWriter;
class SqliteReader;
class SqliteWriter;

namespace Cvbs {
struct FileMetadata;
}

// The video system (combination of a line standard and a colour standard)
// Note: If you update this, be sure to update VIDEO_SYSTEM_DEFAULTS also
enum VideoSystem {
    PAL = 0,    // 625-line PAL
    NTSC,       // 525-line NTSC
    PAL_M,      // 525-line PAL
};

bool parseVideoSystemName(QString name, VideoSystem &system);

// The frame rate of the video: 25 frames a second for PAL, 30000 / 1001 for NTSC and PAL-M (the rate of the time of the video, as a fraction and as a number)
inline qint32 frameRateNumerator(VideoSystem system) { return system == PAL ? 25 : 30000; }
inline qint32 frameRateDenominator(VideoSystem system) { return system == PAL ? 1 : 1001; }
inline double videoFrameRate(VideoSystem system) { return static_cast<double>(frameRateNumerator(system)) / frameRateDenominator(system); }

// The frames that the time codes of a disc (CLV) count in a second: 25, or 30 for NTSC and PAL-M, the round figure, not the frame rate
inline qint32 timecodeFramesPerSecond(VideoSystem system) { return system == PAL ? 25 : 30; }

class LdDecodeMetaData
{

public:
    // VBI Metadata definition
    struct Vbi {
        bool inUse = false;
        std::array<qint32, 3> vbiData { 0, 0, 0 };

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
    };

    // Video metadata definition
    struct VideoParameters {
        // -- Members stored in the metadata --

        qint32 numberOfSequentialFields = -1;

        VideoSystem system = NTSC;
        bool isSubcarrierLocked = false;
        bool isWidescreen = false;

        qint32 colourBurstStart = -1;
        qint32 colourBurstEnd = -1;
        qint32 activeVideoStart = -1;
        qint32 activeVideoEnd = -1;

        qint32 white16bIre = -1;
        qint32 black16bIre = -1;
        qint32 blanking16bIre = -1;

        qint32 fieldWidth = -1;
        qint32 fieldHeight = -1;
        double sampleRate = -1.0;

        bool isMapped = false;
        QString tapeFormat = "";
        QString decoder = "ld-decode";      // what made the capture ("ld-decode" or "vhs-decode"), as the SQLite metadata say

        QString gitBranch;
        QString gitCommit;

        // -- Members set by the library --

        // Colour subcarrier frequency in Hz
        double fSC = -1.0;

        // The range of active lines within a frame.
        // This is the same information represented in two different ways, for
        // field- and frame-based processing respectively; the field range
        // should cover the active lines in both fields of a frame.
        // These are half-open ranges, where lines are numbered sequentially
        // from 1 within each field or interlaced frame.
        qint32 firstActiveFieldLine = -1;
        qint32 lastActiveFieldLine = -1;
        qint32 firstActiveFrameLine = -1;
        qint32 lastActiveFrameLine = -1;

        // Flags if our data has been initialized yet
        bool isValid = false;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId) const;
    };

    // Specification for customising the range of active lines in VideoParameters.
    // -1 for any of these means to use the default for the standard.
    struct LineParameters {
        qint32 firstActiveFieldLine = -1;
        qint32 lastActiveFieldLine = -1;
        qint32 firstActiveFrameLine = -1;
        qint32 lastActiveFrameLine = -1;

        void applyTo(VideoParameters &videoParameters);
    };

    // VITS metrics metadata definition
    struct VitsMetrics {
        bool inUse = false;
        double wSNR = 0.0;
        double bPSNR = 0.0;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
    };

    // NTSC Specific metadata definition
    struct ClosedCaption;
    struct Ntsc {
        bool inUse = false;
        bool isFmCodeDataValid = false;
        qint32 fmCodeData = 0;
        bool fieldFlag = false;
        bool isVideoIdDataValid = false;
        qint32 videoIdData = 0;
        bool whiteFlag = false;

        void read(JsonReader &reader, ClosedCaption &closedCaption);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId, int fieldId, ClosedCaption &closedCaption);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
    };

    // VITC timecode definition
    struct Vitc {
        bool inUse = false;

        // Just the VITC data, without the sync bits or CRC.
        // vitcData[0]'s LSB is bit 2; vitcData[7]'s MSB is bit 79.
        std::array<qint32, 8> vitcData;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
    };

    // Closed Caption definition
    struct ClosedCaption {
        bool inUse = false;

        qint32 data0 = -1;
        qint32 data1 = -1;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
    };

    // PCM sound metadata definition
    struct PcmAudioParameters {
        double sampleRate = -1.0;
        bool isLittleEndian = false;
        bool isSigned = false;
        qint32 bits = -1;

        // Flags if our data has been initialized yet
        bool isValid = false;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId) const;
    };

    // Field metadata definition
    struct Field {
        qint32 seqNo = 0;   // Note: This is the unique primary-key
        bool isFirstField = false;
        qint32 syncConf = 0;
        double medianBurstIRE = 0.0;
        qint32 fieldPhaseID = -1;
        qint32 audioSamples = -1;

        VitsMetrics vitsMetrics;
        Vbi vbi;
        Ntsc ntsc;
        Vitc vitc;
        ClosedCaption closedCaption;
        DropOuts dropOuts;
        bool pad = false;

        double diskLoc = -1;
        qint64 fileLoc = -1;
        qint32 decodeFaults = -1;
        qint32 efmTValues = -1;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId, unsigned tables) const;     // (the tables as LdDecodeMetaData::Tables)
    };

    // CLV timecode (used by frame number conversion methods)
    struct ClvTimecode {
        qint32 hours;
        qint32 minutes;
        qint32 seconds;
        qint32 pictureNumber;
    };

    LdDecodeMetaData();
    ~LdDecodeMetaData();

    // Prevent copying or assignment
    LdDecodeMetaData(const LdDecodeMetaData &) = delete;
    LdDecodeMetaData& operator=(const LdDecodeMetaData &) = delete;

    // Metadata file formats. SQLite (.tbc.db) and JSON (.tbc.json) are both
    // fully supported, for reading and for writing: current ld-decode writes
    // SQLite, while existing captures and other decoders use JSON.
    enum class MetadataFormat {
        Sqlite,
        Json,
    };

    void clear();

    // Metadata for video that comes without any, as from a pipe: the standard video
    // parameters of the system, with the black and white levels in the 16-bit TBC
    // domain (-1: the standard ones; the blanking level follows from them, as in
    // decode-orc). Fields have no metadata beyond their number and order, and are
    // added by ensureFields as the video turns out to be long enough.
    void initialiseDefault(VideoSystem system, qint32 black16bIre = -1, qint32 white16bIre = -1,
                           bool isSubcarrierLocked = true);
    // Make sure there are at least numberOfFields fields (see initialiseDefault)
    void ensureFields(qint32 numberOfFields);
    // Which of a pair of generated fields is the first field: the earlier one, or (reversed) the
    // later one, which pairs the fields one off. Applies to the fields already made and to those to come.
    void setGeneratedFieldOrder(bool reversed);
    // How many fields there must be for a frame to be complete: 2 a frame, and with the order
    // reversed 2 more, the frame being a field on from where it would start
    qint32 fieldsForFrame(qint32 frameNumber) const { return 2 * frameNumber + (generatedReversed ? 2 : 0); }
    // True for metadata made by initialiseDefault
    bool isGenerated() const { return generated; }
    // Where the metadata of a field comes from when it is not on hand with the video, as in a pipe whose producer sends
    // the metadata of each field just before its video: ensureFields asks for the field (by its sequence number, from 1),
    // and keeps the default one if the provider has none (false).
    // Metadata that has the parameters of the video but no fields yet: they are made, with their metadata from the
    // provider, as ensureFields asks for them (see initialiseDefault)
    void makeLive() { generated = true; }
    typedef std::function<bool(qint32 seqNo, Field &field)> FieldProvider;
    void setFieldProvider(FieldProvider provider) { fieldProvider = std::move(provider); }
    // One field as JSON (a member of "fields" in ld-decode metadata), and back. False if it is not one.
    static QByteArray fieldToJson(const Field &field);
    // The parameters of the video and of the audio alone, with no fields (the first block of the metadata of an LDX stream
    // whose fields come later, one block each): ld-decode metadata as JSON. Empty if it cannot be made.
    QByteArray parametersJson() const;
    static bool fieldFromJson(const QByteArray &json, Field &field);

    // Read metadata; the format is detected from the file contents. CVBS core
    // metadata (<base>.meta) is read too, with the fields it does not hold
    // filled in: see isCvbs().
    bool read(QString fileName);
    // Write metadata: JSON for a ".json" file name, SQLite for ".db", and
    // otherwise the format the metadata was read in (see getFormat()).
    // For a CVBS capture (<base>.meta or a <base>.cvbs.db/.json companion),
    // the companion, <base>.meta and <base>.dropouts.meta are all written.
    bool write(QString fileName) const { return write(fileName, AllTables); }
    // The tables of the fields that are written when the file is a SQLite file that exists: a tool that changes only some of them says which (the others are
    // left as they are in the file, and are not written again). A file that does not exist gets all of them.
    enum Tables : unsigned {
        FieldTable = 1,         // the field records (with the NTSC data)
        VitsTable = 2,
        VbiTable = 4,
        VitcTable = 8,
        ClosedCaptionTable = 16,
        DropOutsTable = 32,
        AllTables = 63
    };
    bool write(QString fileName, unsigned tables) const;

    // CVBS captures (.cvbs, .cvbsy, .cvbsc): the core metadata decode-orc
    // reads, set when CVBS metadata was read or for a capture to be written
    // as CVBS. Field metadata the CVBS core lacks is kept in the companion.
    bool isCvbs() const;
    const Cvbs::FileMetadata &getCvbsFileMetadata() const;
    void setCvbsFileMetadata(const Cvbs::FileMetadata &fileMetadata);

    // The format of the last file read (SQLite if none), used for outputs
    // that do not name a format, so JSON in gives JSON out.
    MetadataFormat getFormat() const;
    void setFormat(MetadataFormat format);

    // Detect the format of an existing metadata file from its contents, or of
    // a file still to be written from its extension.
    static MetadataFormat detectFormat(const QString &fileName);
    // The metadata file belonging to a TBC file: <tbc>.db if it exists,
    // otherwise <tbc>.json if that exists, otherwise <tbc>.db.
    // For CVBS data files the companion <base>.cvbs.db or .cvbs.json is used
    // the same way, then <base>.meta.
    // A chroma file (<name>.tbcc or <name>_chroma.tbc) with none of its own has the
    // metadata of the luma file; <name>.tbcy and <name>.tbcc (decode-orc) have that
    // of <name>.tbc.
    static QString findMetadataFile(const QString &tbcFileName);

    // Keep the map of a capture in its metadata (map null: take the one there is away): a SQLite file gets the table disc_map and nothing else is touched; a JSON
    // file is read and written again with the "ldxMap" member (a backup, name.bup, is made first); an LDX file gets the metadata with the map added, as the current
    // ones. Returns false, with the reason in error.
    static bool saveDiscMap(const QString &videoFileName, const FrameMap *map, QString &error);
    // The metadata file name for a TBC file in a given format; for a CVBS
    // data file, its companion (<base>.cvbs.db or <base>.cvbs.json).
    static QString metadataFileName(const QString &tbcFileName, MetadataFormat format);

    void readFields(JsonReader &reader);
    void writeFields(JsonWriter &writer) const;
    void readFields(SqliteReader &reader, int captureId);
    void writeFields(SqliteWriter &writer, int captureId, unsigned tables) const;

    const VideoParameters &getVideoParameters();
    void setVideoParameters(const VideoParameters &videoParameters);

    // False when the capture has no PCM audio (CVBS captures, for one)
    bool hasPcmAudioParameters() const;
    const PcmAudioParameters &getPcmAudioParameters();
    void setPcmAudioParameters(const PcmAudioParameters &pcmAudioParam);

    // Handle line parameters
    void processLineParameters(LdDecodeMetaData::LineParameters &_lineParameters);

    // Get field metadata
    const Field &getField(qint32 sequentialFieldNumber);
    const VitsMetrics &getFieldVitsMetrics(qint32 sequentialFieldNumber);
    const Vbi &getFieldVbi(qint32 sequentialFieldNumber);
    const Ntsc &getFieldNtsc(qint32 sequentialFieldNumber);
    const Vitc &getFieldVitc(qint32 sequentialFieldNumber);
    const ClosedCaption &getFieldClosedCaption(qint32 sequentialFieldNumber);
    const DropOuts &getFieldDropOuts(qint32 sequentialFieldNumber);

    // Set field metadata
    void updateField(const Field &field, qint32 sequentialFieldNumber);
    void updateFieldVitsMetrics(const LdDecodeMetaData::VitsMetrics &vitsMetrics, qint32 sequentialFieldNumber);
    void updateFieldVbi(const LdDecodeMetaData::Vbi &vbi, qint32 sequentialFieldNumber);
    void updateFieldNtsc(const LdDecodeMetaData::Ntsc &ntsc, qint32 sequentialFieldNumber);
    void updateFieldVitc(const LdDecodeMetaData::Vitc &vitc, qint32 sequentialFieldNumber);
    void updateFieldClosedCaption(const LdDecodeMetaData::ClosedCaption &closedCaption, qint32 sequentialFieldNumber);
    void updateFieldDropOuts(const DropOuts &dropOuts, qint32 sequentialFieldNumber);
    void clearFieldDropOuts(qint32 sequentialFieldNumber);

    void appendField(const Field &field);
    // Leave no fields (the parameters stay), to put others in their place
    void clearFields();

    void setNumberOfFields(qint32 numberOfFields);
    qint32 getNumberOfFields();
    qint32 getNumberOfFrames();
    qint32 getFirstFieldNumber(qint32 frameNumber);
    qint32 getSecondFieldNumber(qint32 frameNumber);

    void setIsFirstFieldFirst(bool flag);
    bool getIsFirstFieldFirst();

    qint32 convertClvTimecodeToFrameNumber(LdDecodeMetaData::ClvTimecode clvTimeCode);
    LdDecodeMetaData::ClvTimecode convertFrameNumberToClvTimecode(qint32 clvFrameNumber);

    // PCM Analogue audio helper methods
    qint32 getFieldPcmAudioStart(qint32 sequentialFieldNumber);
    qint32 getFieldPcmAudioLength(qint32 sequentialFieldNumber);

    // The map of the capture (see FrameMap), when the metadata keep one: in the "ldxMap" member of the JSON (and so in the metadata of an LDX file), and in the
    // table disc_map of a SQLite file. It is kept with the metadata as it is read and written; the fields are those of the capture (the map is not applied here).
    // Become the metadata of the view of a map (MappedMetadata::build): the parameters and the fields are those of view, the map is not kept (it is applied)
    void adoptView(const LdDecodeMetaData &view);
    bool hasDiscMap() const { return discMapPresent; }
    const FrameMap &getDiscMap() const { return discMap; }
    void setDiscMap(const FrameMap &map) { discMap = map; discMapPresent = true; }
    void clearDiscMap() { discMap = FrameMap(); discMapPresent = false; }

    // Video system helper methods
    QString getVideoSystemDescription() const;

private:
    MetadataFormat format = MetadataFormat::Sqlite;
    bool isFirstFieldFirst;
    bool generatedReversed = false;
    bool isFirstFieldSearchReported = false;     // the search for a first field failed and was said (once, not for every frame)
    FieldProvider fieldProvider;
    VideoParameters videoParameters;
    PcmAudioParameters pcmAudioParameters;
    FrameMap discMap;
    bool discMapPresent = false;
    QVector<Field> fields;

    Field &fieldAt(qint32 sequentialFieldNumber, const char *caller);
    QVector<qint32> pcmAudioFieldStartSampleMap;
    QVector<qint32> pcmAudioFieldLengthMap;

    bool readJson(QString fileName);
    bool writeJson(QString fileName) const;
    bool readSqlite(QString fileName);
    bool writeSqlite(QString fileName, unsigned tables = AllTables) const;
    void setStandardVideoParameters(VideoSystem system);
    bool readCvbsCore(QString fileName);
    bool writeCvbs(QString fileName) const;

    std::unique_ptr<Cvbs::FileMetadata> cvbsFileMetadata;
    bool generated = false;

    void initialiseVideoSystemParameters();
    qint32 getFieldNumber(qint32 frameNumber, qint32 field);
    void generatePcmAudioMap();
};

#endif // LDDECODEMETADATA_H
