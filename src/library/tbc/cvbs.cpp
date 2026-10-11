/******************************************************************************
 * cvbs.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "cvbs.h"

#include <QAtomicInt>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>
#include <QDebug>
#include <algorithm>
#include <cmath>

namespace Cvbs {

// Names and constants ------------------------------------------------------------------------------------------------

namespace {

struct EncodingName {
    SampleEncoding encoding;
    const char *name;
};

constexpr EncodingName ENCODING_NAMES[] = {
    {SampleEncoding::U10_4FSC, "CVBS_U10_4FSC"},
    {SampleEncoding::U16_4FSC, "CVBS_U16_4FSC"},
    {SampleEncoding::TPG21_4FSC, "CVBS_TPG21_4FSC"},
    {SampleEncoding::S16_4FSC, "CVBS_S16_4FSC"},
};

// The extensions of the three data file kinds, in Channel order
constexpr const char *DATA_EXTENSIONS[] = {".cvbs", ".cvbsy", ".cvbsc"};

// Fixed device offset of the Snell & Wilcox TPG21 encoding
constexpr qint32 TPG21_OFFSET = 508;

// Legal 10-bit range for the encodings that must clamp
constexpr qint32 LEGAL_MIN = 4;
constexpr qint32 LEGAL_MAX = 1019;

// Metadata schema versions written (CVBS file format specification v1.6.0)
constexpr int CORE_SCHEMA_VERSION = 11;
constexpr int DROPOUT_SCHEMA_VERSION = 5;

// A QtSql connection to one SQLite file, removed again when this goes out of
// scope (QSqlDatabase::removeDatabase needs every handle to be gone first).
class SqliteFile
{
public:
    explicit SqliteFile(const QString &fileName)
    {
        static QAtomicInt counter;
        connectionName = QString("cvbs_%1").arg(counter.fetchAndAddRelaxed(1));
        db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        db.setDatabaseName(fileName);
        opened = db.open();
    }

    ~SqliteFile()
    {
        if (opened) db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    SqliteFile(const SqliteFile &) = delete;
    SqliteFile &operator=(const SqliteFile &) = delete;

    bool isOpen() const { return opened; }
    QSqlDatabase &database() { return db; }
    QString lastError() const { return db.lastError().text(); }

    bool hasTable(const QString &table) const { return db.tables().contains(table); }

    int userVersion()
    {
        QSqlQuery query(db);
        if (!query.exec("PRAGMA user_version") || !query.next()) return -1;
        return query.value(0).toInt();
    }

private:
    QString connectionName;
    QSqlDatabase db;
    bool opened = false;
};

// Round a 16-bit TBC value to the 10-bit domain
qint32 tbcTo10Bit(quint16 tbc)
{
    return (static_cast<qint32>(tbc) + 32) / 64;
}

qint16 clampToInt16(qint32 value)
{
    return static_cast<qint16>(std::clamp<qint32>(value, -32768, 32767));
}

quint16 clampToUint16(qint32 value)
{
    return static_cast<quint16>(std::clamp<qint32>(value, 0, 65535));
}

} // namespace

QString sampleEncodingName(SampleEncoding encoding)
{
    for (const auto &entry: ENCODING_NAMES) {
        if (entry.encoding == encoding) return entry.name;
    }
    return QString();
}

bool parseSampleEncodingName(const QString &name, SampleEncoding &encoding)
{
    for (const auto &entry: ENCODING_NAMES) {
        if (name.compare(entry.name, Qt::CaseInsensitive) == 0) {
            encoding = entry.encoding;
            return true;
        }
    }
    return false;
}

QString presetName(VideoSystem system)
{
    switch (system) {
    case PAL: return "PAL";
    case NTSC: return "NTSC";
    case PAL_M: return "PAL_M";
    }
    return QString();
}

// File naming --------------------------------------------------------------------------------------------------------

bool isDataFileName(const QString &fileName)
{
    for (const char *extension: DATA_EXTENSIONS) {
        if (fileName.endsWith(extension, Qt::CaseInsensitive)) return true;
    }
    return false;
}

Channel channel(const QString &dataFileName)
{
    if (dataFileName.endsWith(".cvbsy", Qt::CaseInsensitive)) return Channel::Luma;
    if (dataFileName.endsWith(".cvbsc", Qt::CaseInsensitive)) return Channel::Chroma;
    return Channel::Composite;
}

QString baseName(const QString &dataFileName)
{
    for (const char *extension: DATA_EXTENSIONS) {
        if (dataFileName.endsWith(extension, Qt::CaseInsensitive)) {
            return dataFileName.left(dataFileName.length() - static_cast<int>(qstrlen(extension)));
        }
    }
    return dataFileName;
}

QString dataFileName(const QString &base, Channel _channel)
{
    return base + DATA_EXTENSIONS[static_cast<int>(_channel)];
}

QString pairedDataFileName(const QString &dataFileName)
{
    switch (channel(dataFileName)) {
    case Channel::Luma: return Cvbs::dataFileName(baseName(dataFileName), Channel::Chroma);
    case Channel::Chroma: return Cvbs::dataFileName(baseName(dataFileName), Channel::Luma);
    case Channel::Composite: break;
    }
    return QString();
}

QString metaFileName(const QString &dataFileName)
{
    return baseName(dataFileName) + ".meta";
}

QString dropoutsFileName(const QString &dataFileName)
{
    return baseName(dataFileName) + ".dropouts.meta";
}

QString companionFileName(const QString &dataFileName, bool json)
{
    return baseName(dataFileName) + (json ? ".cvbs.json" : ".cvbs.db");
}

QString dataFileForMetadata(const QString &metadataFileName)
{
    QString base;
    if (metadataFileName.endsWith(".dropouts.meta", Qt::CaseInsensitive)) {
        return QString();
    } else if (metadataFileName.endsWith(".meta", Qt::CaseInsensitive)) {
        base = metadataFileName.left(metadataFileName.length() - 5);
    } else if (metadataFileName.endsWith(".cvbs.db", Qt::CaseInsensitive)) {
        base = metadataFileName.left(metadataFileName.length() - 8);
    } else if (metadataFileName.endsWith(".cvbs.json", Qt::CaseInsensitive)) {
        base = metadataFileName.left(metadataFileName.length() - 10);
    } else {
        return QString();
    }

    for (const char *extension: DATA_EXTENSIONS) {
        if (QFileInfo::exists(base + extension)) return base + extension;
    }
    return base + DATA_EXTENSIONS[0];
}

bool isCoreMetadataFile(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    static const QByteArray sqliteHeader("SQLite format 3\0", 16);
    if (file.read(sqliteHeader.size()) != sqliteHeader) return false;
    file.close();

    SqliteFile sqlite(fileName);
    return sqlite.isOpen() && sqlite.hasTable("cvbs_file");
}

// Geometry -----------------------------------------------------------------------------------------------------------

qint64 Geometry::lineOffset(qint32 frameLine) const
{
    // PAL's two extra samples end the first field's last line, so every line
    // of the second field starts two samples later
    return static_cast<qint64>(frameLine) * lineWidth + (frameLine >= field1Lines ? extraSamples : 0);
}

Geometry geometry(VideoSystem system)
{
    Geometry result;
    switch (system) {
    case PAL:
        // 1135.0064 samples per line on average: 709,379 per frame [EBU 3280]
        result.lineWidth = 1135;
        result.field1Lines = 313;
        result.field2Lines = 312;
        result.extraSamples = 2;
        break;
    case NTSC:
        result.lineWidth = 910;
        result.field1Lines = 263;
        result.field2Lines = 262;
        break;
    case PAL_M:
        result.lineWidth = 909;
        result.field1Lines = 263;
        result.field2Lines = 262;
        break;
    }
    result.frameSamples = static_cast<qint64>(result.lineWidth) * (result.field1Lines + result.field2Lines)
                          + 2 * result.extraSamples;
    return result;
}

bool systemForFieldLength(qint32 fieldLength, VideoSystem &system)
{
    for (VideoSystem candidate: {PAL, NTSC, PAL_M}) {
        if (geometry(candidate).fieldLength() == fieldLength) {
            system = candidate;
            return true;
        }
    }
    return false;
}

Levels standardLevels(VideoSystem system)
{
    // EBU Tech. 3280-E (PAL); SMPTE 244M / 170M (NTSC, and PAL-M which shares them)
    if (system == PAL) return {256, 256, 844};
    return {240, 282, 800};
}

// Core metadata ------------------------------------------------------------------------------------------------------

FileMetadata::FileMetadata()
    : system(PAL)
{
}

bool FileMetadata::isPhaseLocked() const
{
    return signalStatePreset.endsWith("_LOCKED") && !signalStatePreset.endsWith("_UNLOCKED");
}

bool FileMetadata::isStandardStable() const
{
    return signalStatePreset.startsWith("STANDARD_STABLE_");
}

bool FileMetadata::read(const QString &fileName)
{
    SqliteFile sqlite(fileName);
    if (!sqlite.isOpen()) {
        qCritical().noquote() << "Cannot open CVBS metadata" << fileName << "-" << sqlite.lastError();
        return false;
    }
    if (!sqlite.hasTable("cvbs_file")) {
        qCritical().noquote() << fileName << "is not CVBS metadata (it has no cvbs_file table)";
        return false;
    }

    const int version = sqlite.userVersion();

    QSqlQuery query(sqlite.database());
    if (!query.exec("SELECT * FROM cvbs_file ORDER BY cvbs_file_id LIMIT 1") || !query.next()) {
        qCritical().noquote() << "CVBS metadata" << fileName << "has no cvbs_file row";
        return false;
    }

    const QSqlRecord record = query.record();
    auto text = [&](const char *column) {
        const int index = record.indexOf(column);
        return index < 0 ? QString() : query.value(index).toString();
    };
    auto optionalInt = [&](const char *column) -> std::optional<qint32> {
        const int index = record.indexOf(column);
        if (index < 0 || query.value(index).isNull()) return std::nullopt;
        return query.value(index).toInt();
    };

    const QString preset = text("preset");
    if (!parseVideoSystemName(preset, system)) {
        qCritical().noquote() << "CVBS metadata" << fileName << "has an unknown video standard preset" << preset;
        return false;
    }

    const QString encodingName = text("sample_encoding_preset");
    if (!parseSampleEncodingName(encodingName, sampleEncoding)) {
        qCritical().noquote() << "CVBS metadata" << fileName << "has an unsupported sample encoding" << encodingName;
        return false;
    }

    // Specification v1.5.0 (user_version 10) named the time-base stable states
    // *_TBC_*; v1.6.0 renamed them *_STABLE_*
    signalStatePreset = text("signal_state_preset");
    if (version <= 10) signalStatePreset.replace("_TBC_", "_STABLE_");

    const std::optional<qint32> continuous = optionalInt("sequence_continuous");
    sequenceContinuous = continuous ? std::optional<bool>(*continuous != 0) : std::nullopt;
    signalType = text("signal_type");
    decoder = text("decoder");
    gitBranch = text("git_branch");
    gitCommit = text("git_commit");
    numberOfFrames = optionalInt("number_of_sequential_frames").value_or(-1);
    blackLevel = optionalInt("black_level");
    const std::optional<qint32> nonstandard = optionalInt("has_nonstandard_values");
    hasNonstandardValues = nonstandard ? std::optional<bool>(*nonstandard != 0) : std::nullopt;
    captureNotes = text("capture_notes");

    return true;
}

bool FileMetadata::write(const QString &fileName) const
{
    SqliteFile sqlite(fileName);
    if (!sqlite.isOpen()) {
        qCritical().noquote() << "Cannot create CVBS metadata" << fileName << "-" << sqlite.lastError();
        return false;
    }

    QSqlDatabase &db = sqlite.database();
    QSqlQuery query(db);
    db.transaction();

    if (!sqlite.hasTable("cvbs_file")) {
        // CVBS file format specification v1.6.0: Metadata Schema
        const char *schema[] = {
            "PRAGMA user_version = 11",
            "CREATE TABLE cvbs_file ("
            "    cvbs_file_id                INTEGER PRIMARY KEY,"
            "    preset                      TEXT    NOT NULL"
            "        CHECK (preset IN ('NTSC', 'PAL', 'PAL_M')),"
            "    sample_encoding_preset      TEXT    NOT NULL"
            "        CHECK (sample_encoding_preset IN ('CVBS_U10_4FSC', 'CVBS_U16_4FSC', 'RAW_S16_28M',"
            "            'RAW_S16_40M', 'CVBS_TPG21_4FSC', 'CVBS_S16_4FSC')),"
            "    signal_state_preset         TEXT    NOT NULL"
            "        CHECK (signal_state_preset IN ("
            "            'STANDARD_STABLE_LOCKED', 'STANDARD_STABLE_UNLOCKED', 'STANDARD_RAW',"
            "            'NONSTANDARD_STABLE_LOCKED', 'NONSTANDARD_STABLE_UNLOCKED', 'NONSTANDARD_RAW')),"
            "    sequence_continuous         BOOLEAN,"
            "    signal_type                 TEXT    NOT NULL"
            "        CHECK (signal_type IN ('composite', 'yc')),"
            "    decoder                     TEXT    NOT NULL,"
            "    git_branch                  TEXT,"
            "    git_commit                  TEXT,"
            "    number_of_sequential_frames INTEGER"
            "        CHECK (number_of_sequential_frames IS NULL OR number_of_sequential_frames >= 1),"
            "    black_level                 INTEGER,"
            "    has_nonstandard_values      BOOLEAN,"
            "    capture_notes               TEXT"
            ")",
            "CREATE TABLE audio_channel_pair ("
            "    channel_pair                INTEGER PRIMARY KEY"
            "        CHECK (channel_pair BETWEEN 0 AND 7),"
            "    description                 TEXT"
            ")",
        };
        for (const char *statement: schema) {
            if (!query.exec(statement)) {
                qCritical().noquote() << "Cannot create CVBS metadata schema in" << fileName << "-"
                                      << query.lastError().text();
                db.rollback();
                return false;
            }
        }
    } else if (sqlite.userVersion() < CORE_SCHEMA_VERSION) {
        // Bring an older file up to the version this writes
        if (!sqlite.database().record("cvbs_file").contains("sequence_continuous")) {
            query.exec("ALTER TABLE cvbs_file ADD COLUMN sequence_continuous BOOLEAN");
        }
        query.exec(QString("PRAGMA user_version = %1").arg(CORE_SCHEMA_VERSION));
    }

    // One row per file: replace it, keeping the other tables
    query.exec("DELETE FROM cvbs_file");
    query.prepare("INSERT INTO cvbs_file (cvbs_file_id, preset, sample_encoding_preset, signal_state_preset,"
                  " sequence_continuous, signal_type, decoder, git_branch, git_commit,"
                  " number_of_sequential_frames, black_level, has_nonstandard_values, capture_notes)"
                  " VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    auto optionalText = [](const QString &value) {
        return value.isEmpty() ? QVariant() : QVariant(value);
    };
    query.addBindValue(presetName(system));
    query.addBindValue(sampleEncodingName(sampleEncoding));
    query.addBindValue(signalStatePreset);
    query.addBindValue(sequenceContinuous ? QVariant(*sequenceContinuous ? 1 : 0) : QVariant());
    query.addBindValue(signalType);
    query.addBindValue(decoder.isEmpty() ? QString("other") : decoder);
    query.addBindValue(optionalText(gitBranch));
    query.addBindValue(optionalText(gitCommit));
    query.addBindValue(numberOfFrames >= 1 ? QVariant(numberOfFrames) : QVariant());
    query.addBindValue(blackLevel ? QVariant(*blackLevel) : QVariant());
    query.addBindValue(hasNonstandardValues ? QVariant(*hasNonstandardValues ? 1 : 0) : QVariant());
    query.addBindValue(optionalText(captureNotes));

    if (!query.exec()) {
        qCritical().noquote() << "Cannot write CVBS metadata" << fileName << "-" << query.lastError().text();
        db.rollback();
        return false;
    }

    return db.commit();
}

bool setAudioChannelPair(const QString &metaFileName, qint32 pair, const QString &description)
{
    SqliteFile sqlite(metaFileName);
    if (!sqlite.isOpen() || !sqlite.hasTable("audio_channel_pair")) {
        qCritical().noquote() << "Cannot record the audio in" << metaFileName;
        return false;
    }

    QSqlQuery query(sqlite.database());
    query.prepare("INSERT OR REPLACE INTO audio_channel_pair (channel_pair, description) VALUES (?, ?)");
    query.addBindValue(pair);
    query.addBindValue(description.isEmpty() ? QVariant() : QVariant(description));
    if (!query.exec()) {
        qCritical().noquote() << "Cannot record the audio in" << metaFileName << "-" << query.lastError().text();
        return false;
    }
    return true;
}

// Sample conversion --------------------------------------------------------------------------------------------------

SampleConverter::SampleConverter(SampleEncoding _encoding, VideoSystem system)
    : encoding(_encoding), blanking(standardLevels(system).blanking)
{
}

quint16 SampleConverter::toTbc(qint16 stored) const
{
    switch (encoding) {
    case SampleEncoding::U10_4FSC:
        return clampToUint16(static_cast<qint32>(stored) * 64);
    case SampleEncoding::U16_4FSC:
        // Already the TBC domain: the same bits read as unsigned
        return static_cast<quint16>(stored);
    case SampleEncoding::TPG21_4FSC:
        return clampToUint16(static_cast<qint32>(stored) + TPG21_OFFSET * 64);
    case SampleEncoding::S16_4FSC:
        return clampToUint16(static_cast<qint32>(stored) * 2 + blanking * 64);
    }
    return 0;
}

void SampleConverter::setLevelMap(double scale, double offset)
{
    levelScale = scale;
    levelOffset = offset;
    hasLevelMap = (scale != 1.0 || offset != 0.0);
}

qint16 SampleConverter::fromTbc(quint16 tbc) const
{
    qint32 value;
    if (hasLevelMap) {
        // Round to nearest, ties away from zero, in the 10-bit domain
        const double scaled = (tbc * levelScale + levelOffset) / 64.0;
        value = static_cast<qint32>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    } else {
        value = tbcTo10Bit(tbc);
    }
    switch (encoding) {
    case SampleEncoding::U10_4FSC:
        return clampToInt16(value);
    case SampleEncoding::U16_4FSC:
        return static_cast<qint16>(static_cast<quint16>(std::clamp(value, 0, 1023) * 64));
    case SampleEncoding::TPG21_4FSC:
        return clampToInt16((std::clamp(value, LEGAL_MIN, LEGAL_MAX) - TPG21_OFFSET) * 64);
    case SampleEncoding::S16_4FSC:
        return clampToInt16((std::clamp(value, LEGAL_MIN, LEGAL_MAX) - blanking) * 32);
    }
    return 0;
}

void frameToFields(const qint16 *frame, const Geometry &geometry, const SampleConverter &converter,
                   quint16 padValue, quint16 *field1, quint16 *field2)
{
    const qint32 field1Samples = geometry.field1Lines * geometry.lineWidth;
    const qint32 field2Samples = geometry.field2Lines * geometry.lineWidth;

    for (qint32 i = 0; i < field1Samples; i++) field1[i] = converter.toTbc(frame[i]);

    const qint16 *source2 = frame + field1Samples + geometry.extraSamples;
    for (qint32 i = 0; i < field2Samples; i++) field2[i] = converter.toTbc(source2[i]);

    // The second field is a line shorter than the TBC field it becomes
    std::fill(field2 + field2Samples, field2 + geometry.fieldLength(), padValue);
}

void fieldsToFrame(const quint16 *field1, const quint16 *field2, const Geometry &geometry,
                   const SampleConverter &converter, qint16 *frame)
{
    const qint32 field1Samples = geometry.field1Lines * geometry.lineWidth;
    const qint32 field2Samples = geometry.field2Lines * geometry.lineWidth;

    // PAL's extra samples bridge the end of each field's last line to what
    // follows, interpolated at 1/3 and 2/3 as decode-orc does [EBU 3280 1.3.1]
    auto bridge = [](qint16 *destination, qint16 last, qint16 next) {
        const qint32 a = last;
        const qint32 b = next;
        destination[0] = static_cast<qint16>((2 * a + b) / 3);
        destination[1] = static_cast<qint16>((a + 2 * b) / 3);
    };

    qint16 *destination = frame;
    for (qint32 i = 0; i < field1Samples; i++) destination[i] = converter.fromTbc(field1[i]);
    destination += field1Samples;
    if (geometry.extraSamples == 2) {
        bridge(destination, destination[-1], converter.fromTbc(field2[0]));
        destination += 2;
    }

    for (qint32 i = 0; i < field2Samples; i++) destination[i] = converter.fromTbc(field2[i]);
    destination += field2Samples;
    if (geometry.extraSamples == 2) bridge(destination, destination[-1], destination[-1]);
}

// Field phase IDs ----------------------------------------------------------------------------------------------------

namespace {

// Lines of each field read to measure its phase: the first 8 lines are vertical
// sync, and PAL needs lines up to 19
constexpr qint32 PHASE_LINES = 40;
// 10 subcarrier cycles of burst are demodulated
constexpr qint32 BURST_SAMPLES = 40;
// Below this amplitude (10-bit units) the burst is considered absent
constexpr double MIN_BURST_AMPLITUDE = 20.0;

// Measures colour burst phase on lines of a frame, following decode-orc's
// colour frame phase observer, which mirrors ld-decode's own measurements
class PhaseMeter
{
public:
    PhaseMeter(VideoSystem _system, double _blanking)
        : system(_system), geometry(Cvbs::geometry(_system)), blanking(_blanking),
          burstStart(_system == PAL ? 98 : 72)
    {
    }

    // The phase ID of a field whose first PHASE_LINES lines are in region, whose
    // first line is frame line firstFrameLine. isLdFirst: the field ld-decode
    // calls first (the first field of each stored frame).
    qint32 phaseID(const double *region, qint32 firstFrameLine, bool isLdFirst) const
    {
        return system == NTSC ? ntscPhaseID(region, firstFrameLine, isLdFirst)
                              : palPhaseID(region, firstFrameLine, isLdFirst);
    }

private:
    VideoSystem system;
    Cvbs::Geometry geometry;
    double blanking;
    qint32 burstStart;

    // Demodulate the burst of a field line at the 4fsc sample grid of the frame
    // buffer: the amplitude, and Q, which is positive for a rising burst
    void burst(const double *region, qint32 firstFrameLine, qint32 line, double &amplitude, double &q) const
    {
        const qint64 offset = geometry.lineOffset(firstFrameLine + line) + burstStart;
        const double *samples = region + static_cast<qint64>(line) * geometry.lineWidth + burstStart;

        double i = 0.0;
        q = 0.0;
        for (qint32 n = 0; n < BURST_SAMPLES; n++) {
            const double ac = samples[n] - blanking;
            switch ((offset + n) % 4) {
            case 0: i += ac; break;
            case 1: q += ac; break;
            case 2: i -= ac; break;
            default: q -= ac; break;
            }
        }
        amplitude = std::sqrt(i * i + q * q);
    }

    // NTSC: the majority of rising bursts on even lines, with the field order,
    // gives the phase ID (SMPTE 244M / ld-decode FieldNTSC)
    qint32 ntscPhaseID(const double *region, qint32 firstFrameLine, bool isLdFirst) const
    {
        qint32 rising = 0;
        qint32 total = 0;
        for (qint32 line = 8; line < PHASE_LINES; line += 2) {
            double amplitude, q;
            burst(region, firstFrameLine, line, amplitude, q);
            if (amplitude >= MIN_BURST_AMPLITUDE) {
                if (q > 0.0) rising++;
                total++;
            }
        }
        if (total == 0) return -1;

        bool field14 = rising > total / 2;
        // The second field starts 263 * 910 samples into the frame: 2 modulo 4,
        // which rotates the demodulation by 180 degrees
        if ((static_cast<qint64>(firstFrameLine) * geometry.lineWidth) % 4 == 2) field14 = !field14;

        if (isLdFirst && field14) return 1;
        if (!isLdFirst && !field14) return 2;
        if (isLdFirst && !field14) return 3;
        return 4;
    }

    // PAL and PAL-M: burst missing on field line 6 (EBU Tech. 3280-E) gives the
    // position among 4 fields, and rising bursts on lines 7, 11, 15 and 19
    // tell the first four fields from the second four (ld-decode FieldPAL)
    qint32 palPhaseID(const double *region, qint32 firstFrameLine, bool isLdFirst) const
    {
        double amplitude, q;
        burst(region, firstFrameLine, 19, amplitude, q);
        const double reference = amplitude;
        if (reference < MIN_BURST_AMPLITUDE) return -1;

        burst(region, firstFrameLine, 5, amplitude, q);
        bool hasBurst;
        if (amplitude >= reference * 0.8) hasBurst = true;
        else if (amplitude < reference * 0.2) hasBurst = false;
        else return -1;

        qint32 m4;
        if (isLdFirst) m4 = hasBurst ? 3 : 1;
        else m4 = hasBurst ? 2 : 4;

        qint32 rising = 0;
        qint32 total = 0;
        for (qint32 line: {6, 10, 14, 18}) {
            burst(region, firstFrameLine, line, amplitude, q);
            if (amplitude >= MIN_BURST_AMPLITUDE) {
                if (q > 0.0) rising++;
                total++;
            }
        }
        if (total == 0 || rising * 2 == total) return -1;

        bool isFirstFour = rising * 2 > total;
        if (m4 == 2) isFirstFour = !isFirstFour;
        return m4 + (isFirstFour ? 0 : 4);
    }
};

} // namespace

bool measureFieldPhaseIDs(const QString &dataFileName, VideoSystem system, SampleEncoding encoding, bool isChroma,
                          qint32 numberOfFrames, const std::function<void(qint32, qint32)> &setPhaseID)
{
    QFile file(dataFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "Cannot read the colour burst from" << dataFileName;
        return false;
    }

    const Geometry frame = geometry(system);
    const SampleConverter converter(encoding, system);
    const double zero = isChroma ? 512.0 : standardLevels(system).blanking;
    const PhaseMeter meter(system, zero);
    const qint64 frameBytes = frame.frameSamples * 2;
    const qint32 regionSamples = PHASE_LINES * frame.lineWidth;

    QVector<qint16> raw(regionSamples);
    QVector<double> region(regionSamples);

    for (qint32 frameNumber = 0; frameNumber < numberOfFrames; frameNumber++) {
        for (qint32 field = 0; field < 2; field++) {
            const qint32 firstFrameLine = field == 0 ? 0 : frame.field1Lines;
            const qint64 position = frameBytes * frameNumber + frame.lineOffset(firstFrameLine) * 2;
            if (!file.seek(position)
                || file.read(reinterpret_cast<char *>(raw.data()), regionSamples * 2) != regionSamples * 2) {
                return false;
            }

            // The 10-bit domain of the encoding, as decode-orc measures in
            for (qint32 i = 0; i < regionSamples; i++) region[i] = converter.toTbc(raw[i]) / 64.0;

            setPhaseID(frameNumber * 2 + field, meter.phaseID(region.constData(), firstFrameLine, field == 0));
        }
    }
    return true;
}

// Dropout extension --------------------------------------------------------------------------------------------------

bool readDropouts(const QString &fileName, VideoSystem system, qint32 numberOfFrames,
                  const std::function<void(qint32, const DropOuts &)> &setFieldDropOuts)
{
    // The extension is optional
    if (!QFileInfo::exists(fileName)) return true;

    SqliteFile sqlite(fileName);
    if (!sqlite.isOpen() || !sqlite.hasTable("dropout_run")) {
        qWarning().noquote() << "Ignoring unreadable CVBS dropout file" << fileName;
        return false;
    }

    QSqlQuery query(sqlite.database());
    query.setForwardOnly(true);
    if (!query.exec("SELECT frame_id, sample_start, sample_count FROM dropout_run"
                    " ORDER BY frame_id, sample_start")) {
        qWarning().noquote() << "Ignoring unreadable CVBS dropout file" << fileName << "-" << query.lastError().text();
        return false;
    }

    const Geometry frame = geometry(system);
    const qint32 frameLines = frame.field1Lines + frame.field2Lines;

    qint32 currentFrame = -1;
    DropOuts fieldDropOuts[2];
    auto flush = [&]() {
        if (currentFrame < 0) return;
        for (qint32 field = 0; field < 2; field++) {
            if (!fieldDropOuts[field].empty()) setFieldDropOuts(currentFrame * 2 + field, fieldDropOuts[field]);
            fieldDropOuts[field].clear();
        }
    };

    while (query.next()) {
        const qint32 frameId = query.value(0).toInt();
        qint64 offset = query.value(1).toLongLong();
        qint64 count = query.value(2).toLongLong();

        // Consumers ignore runs outside the capture [dropout extension]
        if (frameId < 0 || (numberOfFrames >= 0 && frameId >= numberOfFrames)) continue;
        if (offset < 0 || count <= 0 || offset >= frame.frameSamples) continue;

        if (frameId != currentFrame) {
            flush();
            currentFrame = frameId;
        }

        // A run may continue over several lines: split it at line ends
        while (count > 0 && offset < frame.frameSamples) {
            qint32 line;
            if (offset < frame.lineOffset(frame.field1Lines)) {
                line = std::min(static_cast<qint32>(offset / frame.lineWidth), frame.field1Lines - 1);
            } else {
                line = std::min(static_cast<qint32>((offset - frame.extraSamples) / frame.lineWidth), frameLines - 1);
            }
            const qint32 sample = static_cast<qint32>(offset - frame.lineOffset(line));
            const bool lastLineOfField = (line == frame.field1Lines - 1 || line == frameLines - 1);
            const qint32 lineLength = frame.lineWidth + (lastLineOfField ? frame.extraSamples : 0);
            const qint32 runLength = static_cast<qint32>(std::min<qint64>(count, lineLength - sample));

            // The extra PAL samples are not part of a TBC line
            if (sample < frame.lineWidth) {
                const qint32 endx = std::min(sample + runLength, frame.lineWidth) - 1;
                const bool secondField = line >= frame.field1Lines;
                const qint32 fieldLine = (secondField ? line - frame.field1Lines : line) + 1;
                fieldDropOuts[secondField ? 1 : 0].append(sample, endx, fieldLine);
            }

            offset += runLength;
            count -= runLength;
        }
    }
    flush();

    return true;
}

bool writeDropouts(const QString &fileName, VideoSystem system, qint32 numberOfFrames,
                   const std::function<const DropOuts &(qint32)> &fieldDropOuts)
{
    // Start from an empty file: the runs are always written in full
    if (QFileInfo::exists(fileName) && !QFile::remove(fileName)) {
        qCritical().noquote() << "Cannot replace CVBS dropout file" << fileName;
        return false;
    }

    SqliteFile sqlite(fileName);
    if (!sqlite.isOpen()) {
        qCritical().noquote() << "Cannot create CVBS dropout file" << fileName << "-" << sqlite.lastError();
        return false;
    }

    QSqlDatabase &db = sqlite.database();
    QSqlQuery query(db);
    db.transaction();

    const char *schema[] = {
        "PRAGMA user_version = 5",
        "CREATE TABLE dropout_run ("
        "    cvbs_file_id    INTEGER NOT NULL,"
        "    frame_id        INTEGER NOT NULL CHECK (frame_id >= 0),"
        "    sample_start    INTEGER NOT NULL CHECK (sample_start >= 0),"
        "    sample_count    INTEGER NOT NULL CHECK (sample_count > 0),"
        "    severity        INTEGER NOT NULL CHECK (severity >= 0 AND severity <= 100),"
        "    PRIMARY KEY (cvbs_file_id, frame_id, sample_start)"
        ")",
        "CREATE INDEX idx_dropout_run_frame ON dropout_run (cvbs_file_id, frame_id)",
    };
    static_assert(DROPOUT_SCHEMA_VERSION == 5, "schema text above writes version 5");
    for (const char *statement: schema) {
        if (!query.exec(statement)) {
            qCritical().noquote() << "Cannot create CVBS dropout schema in" << fileName << "-"
                                  << query.lastError().text();
            db.rollback();
            return false;
        }
    }

    const Geometry frame = geometry(system);
    query.prepare("INSERT OR REPLACE INTO dropout_run (cvbs_file_id, frame_id, sample_start, sample_count, severity)"
                  " VALUES (1, ?, ?, ?, 100)");

    for (qint32 frameId = 0; frameId < numberOfFrames; frameId++) {
        for (qint32 field = 0; field < 2; field++) {
            const DropOuts &dropOuts = fieldDropOuts(frameId * 2 + field);
            const qint32 fieldLines = field == 0 ? frame.field1Lines : frame.field2Lines;

            for (qint32 i = 0; i < dropOuts.size(); i++) {
                // TBC field lines are 1-based, and the padding line has no
                // place in a stored frame
                const qint32 line = dropOuts.fieldLine(i) - 1;
                if (line < 0 || line >= fieldLines) continue;
                const qint32 startx = std::max(dropOuts.startx(i), 0);
                const qint32 endx = std::min(dropOuts.endx(i), frame.lineWidth - 1);
                if (endx < startx) continue;

                const qint32 frameLine = field == 0 ? line : frame.field1Lines + line;
                query.addBindValue(frameId);
                query.addBindValue(frame.lineOffset(frameLine) + startx);
                query.addBindValue(endx - startx + 1);
                if (!query.exec()) {
                    qCritical().noquote() << "Cannot write CVBS dropout file" << fileName << "-"
                                          << query.lastError().text();
                    db.rollback();
                    return false;
                }
            }
        }
    }

    return db.commit();
}

} // namespace Cvbs
