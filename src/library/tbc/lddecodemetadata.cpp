/******************************************************************************
 * lddecodemetadata.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2022 Ryan Holtz
 * SPDX-FileCopyrightText: 2022-2023 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "lddecodemetadata.h"
#include "stackedvideo.h"
#include "ldxcapture.h"
#include <QTemporaryFile>

#include "cvbs.h"
#include "cvbsaudio.h"
#include "tbcfiles.h"
#include "jsonio.h"
#include "sqliteio.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <QFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDebug>
#include <QMap>
#include <QMultiMap>
#include <QHash>
#include <QSqlRecord>
#include <algorithm>
#include <array>
#include <QVariant>
#include "tbc/logging.h"

// Default values used when configuring VideoParameters for a particular video system.
// See the comments in VideoParameters for the meanings of these values.
// For descriptions of the systems, see ITU BT.1700.
struct VideoSystemDefaults {
    VideoSystem system;
    const char *name;
    double fSC;
    qint32 minActiveFrameLine;
    qint32 firstActiveFieldLine;
    qint32 lastActiveFieldLine;
    qint32 firstActiveFrameLine;
    qint32 lastActiveFrameLine;
};

static constexpr VideoSystemDefaults palDefaults {
    PAL,
    "PAL",
    (283.75 * 15625) + 25,
    2,
    22, 308,
    // Interlaced line 44 is PAL line 23 (the first active half-line)
    // Interlaced line 619 is PAL line 623 (the last active half-line)
    44, 620,
};

static constexpr VideoSystemDefaults ntscDefaults {
    NTSC,
    "NTSC",
    315.0e6 / 88.0,
    1,
    20, 263,
    // Interlaced line 40 is NTSC line 21 (the closed-caption line before the first active half-line)
    // Interlaced line 524 is NTSC line 263 (the last active half-line)
    40, 525,
};

static constexpr VideoSystemDefaults palMDefaults {
    PAL_M,
    "PAL_M",
    5.0e6 * (63.0 / 88.0) * (909.0 / 910.0),
    ntscDefaults.minActiveFrameLine,
    ntscDefaults.firstActiveFieldLine, ntscDefaults.lastActiveFieldLine,
    ntscDefaults.firstActiveFrameLine, ntscDefaults.lastActiveFrameLine,
};

// These must be in the same order as enum VideoSystem
static constexpr VideoSystemDefaults VIDEO_SYSTEM_DEFAULTS[] = {
    palDefaults,
    ntscDefaults,
    palMDefaults,
};

// Return appropriate defaults for the selected video system
static const VideoSystemDefaults &getSystemDefaults(const LdDecodeMetaData::VideoParameters &videoParameters)
{
    return VIDEO_SYSTEM_DEFAULTS[videoParameters.system];
}

// Look up a video system by name.
// Return true and set system if found; if not found, return false.
bool parseVideoSystemName(QString name, VideoSystem &system)
{
    // JSON metadata spells PAL_M as "PAL-M"
    if (name == "PAL-M") name = "PAL_M";

    // Search VIDEO_SYSTEM_DEFAULTS for a matching name
    for (const auto &defaults: VIDEO_SYSTEM_DEFAULTS) {
        if (name == defaults.name) {
            system = defaults.system;
            return true;
        }
    }
    return false;
}

// Read Vbi from JSON
void LdDecodeMetaData::Vbi::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "vbiData") {
            reader.beginArray();

            // There should be exactly 3 values, but handle more or less
            unsigned int i = 0;
            while (reader.readElement()) {
                int value;
                reader.read(value);

                if (i < vbiData.size()) vbiData[i++] = value;
            }
            while (i < vbiData.size()) vbiData[i++] = 0;

            reader.endArray();
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Write Vbi to JSON
void LdDecodeMetaData::Vbi::write(JsonWriter &writer) const
{
    assert(inUse);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("vbiData");
    writer.beginArray();
    for (auto value : vbiData) {
        writer.writeElement();
        writer.write(value);
    }
    writer.endArray();

    writer.endObject();
}

// Read VideoParameters from JSON
void LdDecodeMetaData::VideoParameters::read(JsonReader &reader)
{
    bool isSourcePal = false;
    std::string systemString = "";

    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "activeVideoEnd") reader.read(activeVideoEnd);
        else if (member == "activeVideoStart") reader.read(activeVideoStart);
        else if (member == "black16bIre") reader.read(black16bIre);
        else if (member == "blanking16bIre") reader.read(blanking16bIre);
        else if (member == "colourBurstEnd") reader.read(colourBurstEnd);
        else if (member == "colourBurstStart") reader.read(colourBurstStart);
        else if (member == "fieldHeight") reader.read(fieldHeight);
        else if (member == "fieldWidth") reader.read(fieldWidth);
        else if (member == "gitBranch") reader.read(gitBranch);
        else if (member == "gitCommit") reader.read(gitCommit);
        else if (member == "isMapped") reader.read(isMapped);
        else if (member == "isSourcePal") reader.read(isSourcePal); // obsolete
        else if (member == "isSubcarrierLocked") reader.read(isSubcarrierLocked);
        else if (member == "isWidescreen") reader.read(isWidescreen);
        else if (member == "numberOfSequentialFields") reader.read(numberOfSequentialFields);
        else if (member == "sampleRate") reader.read(sampleRate);
        else if (member == "system") reader.read(systemString);
        else if (member == "white16bIre") reader.read(white16bIre);
        else if (member == "tapeFormat") reader.read(tapeFormat);
        else reader.discard();
    }

    reader.endObject();

    // Work out which video system is being used
    if (systemString == "") {
        // Not specified -- detect based on isSourcePal and fieldHeight
        if (isSourcePal) {
            if (fieldHeight < 300) system = PAL_M;
            else system = PAL;
        } else system = NTSC;
    } else if (!parseVideoSystemName(QString::fromStdString(systemString), system)) {
        reader.throwError("unknown value for videoParameters.system");
    }

    // JSON files written before blanking16bIre existed do not have it, so use
    // the black level, as the SQLite reader does
    if (blanking16bIre == -1) blanking16bIre = black16bIre;

    isValid = true;
}

// Write VideoParameters to JSON
void LdDecodeMetaData::VideoParameters::write(JsonWriter &writer) const
{
    assert(isValid);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("activeVideoEnd", activeVideoEnd);
    writer.writeMember("activeVideoStart", activeVideoStart);
    writer.writeMember("black16bIre", black16bIre);
    if (blanking16bIre != -1 && blanking16bIre != black16bIre) {
        writer.writeMember("blanking16bIre", blanking16bIre);
    }
    writer.writeMember("colourBurstEnd", colourBurstEnd);
    writer.writeMember("colourBurstStart", colourBurstStart);
    writer.writeMember("fieldHeight", fieldHeight);
    writer.writeMember("fieldWidth", fieldWidth);
    if (gitBranch != "") {
        writer.writeMember("gitBranch", gitBranch);
    }
    if (gitCommit != "") {
        writer.writeMember("gitCommit", gitCommit);
    }
    writer.writeMember("isMapped", isMapped);
    writer.writeMember("isSubcarrierLocked", isSubcarrierLocked);
    writer.writeMember("isWidescreen", isWidescreen);
    writer.writeMember("numberOfSequentialFields", numberOfSequentialFields);
    writer.writeMember("sampleRate", sampleRate);
    // JSON spells PAL_M as "PAL-M", as every JSON reader expects
    writer.writeMember("system", system == PAL_M ? "PAL-M" : VIDEO_SYSTEM_DEFAULTS[system].name);
    writer.writeMember("white16bIre", white16bIre);
	if(tapeFormat != "") {
		writer.writeMember("tapeFormat", tapeFormat);
	}

    writer.endObject();
}

// Read VitsMetrics from JSON
void LdDecodeMetaData::VitsMetrics::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "bPSNR") reader.read(bPSNR);
        else if (member == "wSNR") reader.read(wSNR);
        else reader.discard();
    }

    reader.endObject();

    inUse = true;
}

// Write VitsMetrics to JSON
void LdDecodeMetaData::VitsMetrics::write(JsonWriter &writer) const
{
    assert(inUse);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("bPSNR", bPSNR);
    writer.writeMember("wSNR", wSNR);

    writer.endObject();
}

// Read Ntsc from JSON
void LdDecodeMetaData::Ntsc::read(JsonReader &reader, ClosedCaption &closedCaption)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "isFmCodeDataValid") reader.read(isFmCodeDataValid);
        else if (member == "fmCodeData") reader.read(fmCodeData);
        else if (member == "fieldFlag") reader.read(fieldFlag);
        else if (member == "isVideoIdDataValid") reader.read(isVideoIdDataValid);
        else if (member == "videoIdData") reader.read(videoIdData);
        else if (member == "whiteFlag") reader.read(whiteFlag);
        else if (member == "ccData0") {
            // rev7 and earlier put ccData0/1 here rather than in cc
            reader.read(closedCaption.data0);
            closedCaption.inUse = true;
        } else if (member == "ccData1") {
            reader.read(closedCaption.data1);
            closedCaption.inUse = true;
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Write Ntsc to JSON
void LdDecodeMetaData::Ntsc::write(JsonWriter &writer) const
{
    assert(inUse);

    writer.beginObject();

    // Keep members in alphabetical order
    if (isFmCodeDataValid) {
        writer.writeMember("fieldFlag", fieldFlag);
    }
    if (isFmCodeDataValid) {
        writer.writeMember("fmCodeData", fmCodeData);
    }
    writer.writeMember("isFmCodeDataValid", isFmCodeDataValid);
    if (isVideoIdDataValid) {
        writer.writeMember("videoIdData", videoIdData);
    }
    writer.writeMember("isVideoIdDataValid", isVideoIdDataValid);
    if (whiteFlag) {
        writer.writeMember("whiteFlag", whiteFlag);
    }

    writer.endObject();
}

// Read Vitc from JSON
void LdDecodeMetaData::Vitc::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "vitcData") {
            reader.beginArray();

            // There should be exactly 8 values, but handle more or less
            unsigned int i = 0;
            while (reader.readElement()) {
                int value;
                reader.read(value);

                if (i < vitcData.size()) vitcData[i++] = value;
            }
            while (i < vitcData.size()) vitcData[i++] = 0;

            reader.endArray();
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Write Vitc to JSON
void LdDecodeMetaData::Vitc::write(JsonWriter &writer) const
{
    assert(inUse);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("vitcData");
    writer.beginArray();
    for (auto value : vitcData) {
        writer.writeElement();
        writer.write(value);
    }
    writer.endArray();

    writer.endObject();
}

// Read ClosedCaption from JSON
void LdDecodeMetaData::ClosedCaption::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "data0") reader.read(data0);
        else if (member == "data1") reader.read(data1);
        else reader.discard();
    }

    reader.endObject();

    inUse = true;
}

// Write ClosedCaption to JSON
void LdDecodeMetaData::ClosedCaption::write(JsonWriter &writer) const
{
    assert(inUse);

    writer.beginObject();

    // Keep members in alphabetical order
    if (data0 != -1) {
        writer.writeMember("data0", data0);
    }
    if (data1 != -1) {
        writer.writeMember("data1", data1);
    }

    writer.endObject();
}

// Read PcmAudioParameters from JSON
void LdDecodeMetaData::PcmAudioParameters::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "bits") reader.read(bits);
        else if (member == "isLittleEndian") reader.read(isLittleEndian);
        else if (member == "isSigned") reader.read(isSigned);
        else if (member == "sampleRate") reader.read(sampleRate);
        else reader.discard();
    }

    reader.endObject();

    isValid = true;
}

// Write PcmAudioParameters to JSON
void LdDecodeMetaData::PcmAudioParameters::write(JsonWriter &writer) const
{
    assert(isValid);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("bits", bits);
    writer.writeMember("isLittleEndian", isLittleEndian);
    writer.writeMember("isSigned", isSigned);
    writer.writeMember("sampleRate", sampleRate);

    writer.endObject();
}

// Read Field from JSON
void LdDecodeMetaData::Field::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "audioSamples") reader.read(audioSamples);
        else if (member == "cc") closedCaption.read(reader);
        else if (member == "decodeFaults") reader.read(decodeFaults);
        else if (member == "diskLoc") reader.read(diskLoc);
        else if (member == "dropOuts") dropOuts.read(reader);
        else if (member == "efmTValues") reader.read(efmTValues);
        else if (member == "fieldPhaseID") reader.read(fieldPhaseID);
        else if (member == "fileLoc") reader.read(fileLoc);
        else if (member == "isFirstField") reader.read(isFirstField);
        else if (member == "medianBurstIRE") reader.read(medianBurstIRE);
        else if (member == "ntsc") ntsc.read(reader, closedCaption);
        else if (member == "pad") reader.read(pad);
        else if (member == "seqNo") reader.read(seqNo);
        else if (member == "syncConf") reader.read(syncConf);
        else if (member == "vbi") vbi.read(reader);
        else if (member == "vitc") vitc.read(reader);
        else if (member == "vitsMetrics") vitsMetrics.read(reader);
        else reader.discard();
    }

    reader.endObject();
}

// Write Field to JSON
void LdDecodeMetaData::Field::write(JsonWriter &writer) const
{
    writer.beginObject();

    // Keep members in alphabetical order
    if (audioSamples != -1) {
        writer.writeMember("audioSamples", audioSamples);
    }
    if (closedCaption.inUse) {
        writer.writeMember("cc");
        closedCaption.write(writer);
    }
    if (decodeFaults != -1) {
        writer.writeMember("decodeFaults", decodeFaults);
    }
    if (diskLoc != -1) {
        writer.writeMember("diskLoc", diskLoc);
    }
    if (!dropOuts.empty()) {
        writer.writeMember("dropOuts");
        dropOuts.write(writer);
    }
    if (efmTValues != -1) {
        writer.writeMember("efmTValues", efmTValues);
    }
    if (fieldPhaseID != -1) {
        writer.writeMember("fieldPhaseID", fieldPhaseID);
    }
    if (fileLoc != -1) {
        writer.writeMember("fileLoc", fileLoc);
    }
    writer.writeMember("isFirstField", isFirstField);
    writer.writeMember("medianBurstIRE", medianBurstIRE);
    if (ntsc.inUse) {
        writer.writeMember("ntsc");
        ntsc.write(writer);
    }
    writer.writeMember("pad", pad);
    writer.writeMember("seqNo", seqNo);
    writer.writeMember("syncConf", syncConf);
    if (vbi.inUse) {
        writer.writeMember("vbi");
        vbi.write(writer);
    }
    if (vitc.inUse) {
        writer.writeMember("vitc");
        vitc.write(writer);
    }
    if (vitsMetrics.inUse) {
        writer.writeMember("vitsMetrics");
        vitsMetrics.write(writer);
    }

    writer.endObject();
}

// Read VBI from SQLite
void LdDecodeMetaData::Vbi::read(SqliteReader &reader, int captureId, int fieldId)
{
    int vbi0, vbi1, vbi2;
    if (reader.readFieldVbi(captureId, fieldId, vbi0, vbi1, vbi2)) {
        vbiData[0] = vbi0;
        vbiData[1] = vbi1;
        vbiData[2] = vbi2;
        inUse = true;
    } else {
        inUse = false;
    }
}

// Write VBI to SQLite
void LdDecodeMetaData::Vbi::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    if (inUse) {
        writer.writeFieldVbi(captureId, fieldId, vbiData[0], vbiData[1], vbiData[2]);
    }
}

// Read VideoParameters from SQLite (handled in main read method)
void LdDecodeMetaData::VideoParameters::read(SqliteReader &reader, int captureId)
{
    // This method is no longer used - data is read directly in LdDecodeMetaData::read
}

// Write VideoParameters to SQLite (handled in main write method)
void LdDecodeMetaData::VideoParameters::write(SqliteWriter &writer, int captureId) const
{
    // This method is no longer used - data is written directly in LdDecodeMetaData::write
}

// Read VitsMetrics from SQLite
void LdDecodeMetaData::VitsMetrics::read(SqliteReader &reader, int captureId, int fieldId)
{
    double wSnr, bPsnr;
    if (reader.readFieldVitsMetrics(captureId, fieldId, wSnr, bPsnr)) {
        this->wSNR = wSnr;
        this->bPSNR = bPsnr;
        inUse = true;
    } else {
        inUse = false;
    }
}

// Write VitsMetrics to SQLite
void LdDecodeMetaData::VitsMetrics::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    if (inUse) {
        writer.writeFieldVitsMetrics(captureId, fieldId, wSNR, bPSNR);
    }
}

QByteArray LdDecodeMetaData::fieldToJson(const Field &field)
{
    std::ostringstream out;
    JsonWriter writer(out);
    field.write(writer);
    const std::string text = out.str();
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

QByteArray LdDecodeMetaData::parametersJson() const
{
    LdDecodeMetaData parameters;
    VideoParameters video = videoParameters;
    video.numberOfSequentialFields = 0;         // (the fields come later)
    parameters.setVideoParameters(video);
    parameters.setPcmAudioParameters(pcmAudioParameters);
    parameters.setIsFirstFieldFirst(isFirstFieldFirst);
    QTemporaryDir directory;
    if (!directory.isValid()) return QByteArray();
    const QString path = directory.filePath("parameters.json");
    if (!parameters.writeJson(path)) return QByteArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
}

bool LdDecodeMetaData::fieldFromJson(const QByteArray &json, Field &field)
{
    try {
        std::istringstream in(std::string(json.constData(), static_cast<size_t>(json.size())));
        JsonReader reader(in);
        field = Field();
        field.read(reader);
    } catch (JsonReader::Error &) {
        return false;
    }
    return field.seqNo > 0;
}

// Read Ntsc from SQLite (data is read from main field record)
void LdDecodeMetaData::Ntsc::read(SqliteReader &reader, int captureId, int fieldId, ClosedCaption &closedCaption)
{
    // NTSC data is read directly from the field_record table in readFields
    // Closed caption is read separately
    closedCaption.read(reader, captureId, fieldId);
}

// Write Ntsc to SQLite (data is written to main field record)
void LdDecodeMetaData::Ntsc::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    // NTSC data is written directly to the field_record table in Field::write
    // This method is essentially a no-op since the data is handled elsewhere
}

// Read Vitc from SQLite
void LdDecodeMetaData::Vitc::read(SqliteReader &reader, int captureId, int fieldId)
{
    int vitcDataArray[8];
    if (reader.readFieldVitc(captureId, fieldId, vitcDataArray)) {
        for (int i = 0; i < 8; i++) {
            vitcData[i] = vitcDataArray[i];
        }
        inUse = true;
    } else {
        inUse = false;
    }
}

// Write Vitc to SQLite
void LdDecodeMetaData::Vitc::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    if (inUse) {
        int vitcDataArray[8];
        for (int i = 0; i < 8; i++) {
            vitcDataArray[i] = vitcData[i];
        }
        writer.writeFieldVitc(captureId, fieldId, vitcDataArray);
    }
}

// Read ClosedCaption from SQLite
void LdDecodeMetaData::ClosedCaption::read(SqliteReader &reader, int captureId, int fieldId)
{
    int data0Val, data1Val;
    if (reader.readFieldClosedCaption(captureId, fieldId, data0Val, data1Val)) {
        data0 = data0Val;
        data1 = data1Val;
        inUse = true;
    } else {
        inUse = false;
    }
}

// Write ClosedCaption to SQLite
void LdDecodeMetaData::ClosedCaption::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    if (inUse) {
        writer.writeFieldClosedCaption(captureId, fieldId, data0, data1);
    }
}

// Read PcmAudioParameters from SQLite (handled in main read method)
void LdDecodeMetaData::PcmAudioParameters::read(SqliteReader &reader, int captureId)
{
    // This method is no longer used - data is read directly in LdDecodeMetaData::read
}

// Write PcmAudioParameters to SQLite (handled in main write method)
void LdDecodeMetaData::PcmAudioParameters::write(SqliteWriter &writer, int captureId) const
{
    // This method is no longer used - data is written directly in LdDecodeMetaData::write
}

// Read Field from SQLite (data is read in readFields)
void LdDecodeMetaData::Field::read(SqliteReader &reader, int captureId)
{
    // This method is no longer used - data is read directly in LdDecodeMetaData::readFields
}

// Write Field to SQLite
void LdDecodeMetaData::Field::write(SqliteWriter &writer, int captureId, unsigned tables) const
{
    // Convert seqNo (1-indexed) to fieldId (0-indexed)
    int fieldId = seqNo - 1;
    
    // Write main field record with NTSC data embedded
    if (tables & FieldTable) {
        writer.writeField(captureId, fieldId, audioSamples, decodeFaults, diskLoc,
                         efmTValues, fieldPhaseID, fileLoc, isFirstField, medianBurstIRE,
                         pad, syncConf, ntsc.isFmCodeDataValid, ntsc.fmCodeData,
                         ntsc.fieldFlag, ntsc.isVideoIdDataValid, ntsc.videoIdData,
                         ntsc.whiteFlag);
    }

    // Write optional field data
    if (tables & VitsTable) vitsMetrics.write(writer, captureId, fieldId);
    if (tables & VbiTable) vbi.write(writer, captureId, fieldId);
    if (tables & VitcTable) vitc.write(writer, captureId, fieldId);
    if (tables & ClosedCaptionTable) closedCaption.write(writer, captureId, fieldId);
    if (tables & DropOutsTable) dropOuts.write(writer, captureId, fieldId);
}

LdDecodeMetaData::LdDecodeMetaData()
{
    clear();
}

LdDecodeMetaData::~LdDecodeMetaData() = default;

// Reset the metadata to the defaults
void LdDecodeMetaData::clear()
{
    // Default to the standard still-frame field order (of first field first)
    isFirstFieldFirst = true;

    // Reset the parameters to their defaults
    videoParameters = VideoParameters();
    pcmAudioParameters = PcmAudioParameters();
    clearDiscMap();
    clearDecodingParameters();

    fields.clear();
    cvbsFileMetadata.reset();
    generated = false;
}

// Read all metadata, in whichever format the file is in
bool LdDecodeMetaData::read(QString fileName)
{
    // A capture kept in an LDX file: its own metadata, or those of the stack of the session the file keeps
    if (LdxCapture::isLdxName(fileName)) {
        QString path;
        qint32 track;
        LdxCapture::split(fileName, path, track);
        QString error;
        if (track == 0 && LdxCapture::hasSession(path)) {
            if (!StackedVideo::readMetadata(fileName, *this, error)) {
                qCritical().noquote() << error;
                return false;
            }
            return true;
        }
        LdxCapture capture;
        QByteArray json;
        if (!capture.open(fileName, error) || !capture.readMetadata(json, error)) {
            qCritical().noquote() << error;
            return false;
        }
        QTemporaryFile temporary;
        if (!temporary.open() || temporary.write(json) != json.size() || !temporary.flush()) {
            qCritical() << "Could not hold the metadata of" << fileName;
            return false;
        }
        if (!readJson(temporary.fileName())) return false;
        format = MetadataFormat::Json;
        return true;
    }

    // A stacking session: the metadata of its stack
    if (fileName.endsWith(".stack", Qt::CaseInsensitive)) {
        QString error;
        if (!StackedVideo::readMetadata(fileName, *this, error)) {
            qCritical().noquote() << error;
            return false;
        }
        return true;
    }

    // CVBS core metadata, as written by decode-orc. Its companion, created
    // when the metadata is written back, is SQLite unless asked otherwise.
    if (Cvbs::isCoreMetadataFile(fileName)) {
        if (!readCvbsCore(fileName)) return false;
        format = MetadataFormat::Sqlite;
        return true;
    }

    const MetadataFormat fileFormat = detectFormat(fileName);
    const bool ok = (fileFormat == MetadataFormat::Json) ? readJson(fileName) : readSqlite(fileName);
    if (!ok) return false;

    // Remember the format so that outputs which do not name one match the input
    format = fileFormat;

    // The companion of a CVBS capture: the core metadata says how its samples
    // are stored
    const QString dataFileName = Cvbs::dataFileForMetadata(fileName);
    if (!dataFileName.isEmpty()) {
        Cvbs::FileMetadata fileMetadata;
        fileMetadata.system = videoParameters.system;
        const QString metaFileName = Cvbs::metaFileName(dataFileName);
        if (QFileInfo::exists(metaFileName) && !fileMetadata.read(metaFileName)) return false;
        cvbsFileMetadata = std::make_unique<Cvbs::FileMetadata>(fileMetadata);
    }

    return true;
}

// Write all metadata, choosing the format from the file name or the input
bool LdDecodeMetaData::write(QString fileName, unsigned tables) const
{
    // The metadata of a capture is kept in its LDX file: the new ones are added to it, and are the current ones from then on
    if (LdxCapture::isLdxName(fileName)) {
        QTemporaryFile temporary;
        if (!temporary.open()) {
            qCritical() << "Could not hold the metadata to keep them in" << fileName;
            return false;
        }
        temporary.close();
        if (!writeJson(temporary.fileName())) return false;
        QFile json(temporary.fileName());
        if (!json.open(QIODevice::ReadOnly)) {
            qCritical() << "Could not read back the metadata to keep them in" << fileName;
            return false;
        }
        QString error;
        if (!LdxCapture::writeMetadata(fileName, json.readAll(), error)) {
            qCritical().noquote() << error;
            return false;
        }
        return true;
    }

    // The metadata of a stacking session is that of its sources: there is none to write
    if (fileName.endsWith(".stack", Qt::CaseInsensitive)) {
        qWarning() << "The metadata of a stacking session cannot be written";
        return false;
    }

    // A CVBS capture: the companion and the files decode-orc reads
    if (!Cvbs::dataFileForMetadata(fileName).isEmpty()) return writeCvbs(fileName);

    MetadataFormat outputFormat = format;
    if (fileName.endsWith(".json", Qt::CaseInsensitive)) outputFormat = MetadataFormat::Json;
    else if (fileName.endsWith(".db", Qt::CaseInsensitive)) outputFormat = MetadataFormat::Sqlite;

    return (outputFormat == MetadataFormat::Json) ? writeJson(fileName) : writeSqlite(fileName, tables);
}

LdDecodeMetaData::MetadataFormat LdDecodeMetaData::getFormat() const
{
    return format;
}

void LdDecodeMetaData::setFormat(MetadataFormat _format)
{
    format = _format;
}

// Detect a metadata file's format: an existing file from its contents (SQLite
// files start with a fixed header), a file still to be written from its name.
LdDecodeMetaData::MetadataFormat LdDecodeMetaData::detectFormat(const QString &fileName)
{
    QFile file(fileName);
    if (file.open(QIODevice::ReadOnly)) {
        static const QByteArray sqliteHeader("SQLite format 3\0", 16);
        return file.read(sqliteHeader.size()) == sqliteHeader ? MetadataFormat::Sqlite : MetadataFormat::Json;
    }

    return fileName.endsWith(".json", Qt::CaseInsensitive) ? MetadataFormat::Json : MetadataFormat::Sqlite;
}

// Find the metadata for a TBC file, preferring SQLite when both exist
QString LdDecodeMetaData::findMetadataFile(const QString &tbcFileName)
{
    // An LDX file (or one of its tracks) has its metadata in itself
    if (LdxCapture::isLdxName(tbcFileName)) return tbcFileName;

    // A stacking session has the metadata of its stack in itself
    if (tbcFileName.endsWith(".stack", Qt::CaseInsensitive)) return tbcFileName;

    const QString sqliteFileName = metadataFileName(tbcFileName, MetadataFormat::Sqlite);
    const QString jsonFileName = metadataFileName(tbcFileName, MetadataFormat::Json);

    if (QFileInfo::exists(sqliteFileName)) return sqliteFileName;
    if (QFileInfo::exists(jsonFileName)) return jsonFileName;

    // A CVBS capture without a companion yet: its core metadata
    if (Cvbs::isDataFileName(tbcFileName) && QFileInfo::exists(Cvbs::metaFileName(tbcFileName))) {
        return Cvbs::metaFileName(tbcFileName);
    }

    // A chroma file has no metadata of its own: it is that of the luma file
    if (TbcFiles::isChromaFile(tbcFileName)) {
        const QString lumaFileName = TbcFiles::lumaFileName(tbcFileName);
        if (lumaFileName != tbcFileName) {
            const QString lumaMetadataFileName = findMetadataFile(lumaFileName);
            if (QFileInfo::exists(lumaMetadataFileName)) return lumaMetadataFileName;
        }
    }
    return sqliteFileName;
}

void LdDecodeMetaData::adoptView(const LdDecodeMetaData &view)
{
    videoParameters = view.videoParameters;
    pcmAudioParameters = view.pcmAudioParameters;
    fields = view.fields;
    isFirstFieldFirst = true;
    clearDiscMap();
    generatePcmAudioMap();
}

// Keep one thing (the map, the decoding parameters) in the metadata of a capture, or take it away: a SQLite file gets its table and nothing else is touched; the other
// formats are read and written again with the change (a backup, name.bup, is made first, except for an LDX file, which keeps the metadata it had)
static bool saveInMetadata(const QString &videoFileName, const char *table, const char *column, const QByteArray &json, const std::function<void(LdDecodeMetaData &)> &change,
                           QString &error)
{
    const QString metadataFile = LdDecodeMetaData::findMetadataFile(videoFileName);
    const bool isLdx = LdxCapture::isLdxName(metadataFile);
    if (!isLdx && !QFileInfo::exists(metadataFile)) {
        error = QObject::tr("The metadata of %1 are not there").arg(videoFileName);
        return false;
    }

    // A SQLite file: one table, in place
    if (!isLdx && metadataFile.endsWith(".db", Qt::CaseInsensitive) && !Cvbs::isDataFileName(videoFileName)) {
        try {
            int captureId = 1;
            {
                SqliteReader reader(metadataFile);
                QString system, decoder, gitBranch, gitCommit, notes;
                double sampleRate;
                int activeStart, activeEnd, width, height, numberOfFields, burstStart, burstEnd, white, black, blanking;
                bool isMapped, locked, wide;
                if (!reader.readCaptureMetadata(captureId, system, decoder, gitBranch, gitCommit, sampleRate, activeStart, activeEnd, width, height,
                                                numberOfFields, burstStart, burstEnd, isMapped, locked, wide, white, black, blanking, notes)) {
                    error = QObject::tr("The capture in %1 could not be read").arg(metadataFile);
                    return false;
                }
            }
            QFile::remove(metadataFile + ".bup");
            QFile::copy(metadataFile, metadataFile + ".bup");
            SqliteWriter writer(metadataFile);
            if (!writer.beginTransaction()) {
                error = QObject::tr("The metadata %1 could not be written").arg(metadataFile);
                return false;
            }
            if (!writer.writeJsonTable(captureId, table, column, json)) {
                writer.rollbackTransaction();
                error = QObject::tr("%1 could not be written in %2").arg(table, metadataFile);
                return false;
            }
            if (!writer.commitTransaction()) {
                error = QObject::tr("The metadata %1 could not be written").arg(metadataFile);
                return false;
            }
        } catch (SqliteWriter::Error &sqliteError) {
            error = QString::fromUtf8(sqliteError.what());
            return false;
        }
        return true;
    }

    // JSON, an LDX file, a CVBS capture: the metadata are read and written again with the change
    LdDecodeMetaData metadata;
    if (!metadata.read(metadataFile)) {
        error = QObject::tr("The metadata of %1 could not be read").arg(videoFileName);
        return false;
    }
    change(metadata);
    if (!isLdx && QFileInfo::exists(metadataFile)) {
        QFile::remove(metadataFile + ".bup");
        QFile::copy(metadataFile, metadataFile + ".bup");
    }
    if (!metadata.write(metadataFile)) {
        error = QObject::tr("The metadata of %1 could not be written").arg(videoFileName);
        return false;
    }
    return true;
}

bool LdDecodeMetaData::saveDiscMap(const QString &videoFileName, const FrameMap *map, QString &error)
{
    return saveInMetadata(videoFileName, "disc_map", "map", map ? map->toJsonBytes() : QByteArray(), [map](LdDecodeMetaData &metadata) {
        if (map) metadata.setDiscMap(*map);
        else metadata.clearDiscMap();
    }, error);
}

bool LdDecodeMetaData::saveDecodingParameters(const QString &videoFileName, const DecodingParameters *parameters, QString &error)
{
    return saveInMetadata(videoFileName, "decoding", "parameters", parameters ? parameters->toJsonBytes() : QByteArray(), [parameters](LdDecodeMetaData &metadata) {
        if (parameters) metadata.setDecodingParameters(*parameters);
        else metadata.clearDecodingParameters();
    }, error);
}

QString LdDecodeMetaData::metadataFileName(const QString &tbcFileName, MetadataFormat _format)
{
    // An LDX file keeps its metadata in itself
    if (LdxCapture::isLdxName(tbcFileName)) return tbcFileName;
    if (Cvbs::isDataFileName(tbcFileName)) return Cvbs::companionFileName(tbcFileName, _format == MetadataFormat::Json);
    // decode-orc's <name>.tbcy and <name>.tbcc have the metadata of <name>.tbc
    return TbcFiles::metadataBaseName(tbcFileName) + (_format == MetadataFormat::Json ? ".json" : ".db");
}

bool LdDecodeMetaData::isCvbs() const
{
    return cvbsFileMetadata != nullptr;
}

const Cvbs::FileMetadata &LdDecodeMetaData::getCvbsFileMetadata() const
{
    static const Cvbs::FileMetadata defaults;
    return cvbsFileMetadata ? *cvbsFileMetadata : defaults;
}

void LdDecodeMetaData::setCvbsFileMetadata(const Cvbs::FileMetadata &fileMetadata)
{
    cvbsFileMetadata = std::make_unique<Cvbs::FileMetadata>(fileMetadata);
}

// The video parameters that follow from the video system alone, with ld-decode's
// colour burst and active video positions for the standard
void LdDecodeMetaData::setStandardVideoParameters(VideoSystem system)
{
    const Cvbs::Geometry geometry = Cvbs::geometry(system);

    videoParameters.system = system;
    videoParameters.isWidescreen = false;
    videoParameters.isMapped = false;

    if (system == PAL) {
        videoParameters.colourBurstStart = 98;
        videoParameters.colourBurstEnd = 138;
        videoParameters.activeVideoStart = 185;
        videoParameters.activeVideoEnd = 1107;
    } else {
        videoParameters.colourBurstStart = 74;
        videoParameters.colourBurstEnd = 110;
        videoParameters.activeVideoStart = 134;
        videoParameters.activeVideoEnd = (system == PAL_M) ? 893 : 894;
    }

    videoParameters.fieldWidth = geometry.lineWidth;
    videoParameters.fieldHeight = geometry.fieldHeight();
    videoParameters.isValid = true;
}

void LdDecodeMetaData::initialiseDefault(VideoSystem system, qint32 black16bIre, qint32 white16bIre,
                                         bool isSubcarrierLocked)
{
    clear();
    generated = true;
    setStandardVideoParameters(system);
    videoParameters.numberOfSequentialFields = 0;
    videoParameters.isSubcarrierLocked = isSubcarrierLocked;

    // The levels of the 16-bit TBC domain, which is the 10-bit CVBS one times 64
    const Cvbs::Levels levels = Cvbs::standardLevels(system);
    const qint32 white = white16bIre > 0 ? white16bIre : levels.white * 64;
    qint32 black = black16bIre >= 0 ? black16bIre : levels.black * 64;
    qint32 blanking;
    if (system == PAL) {
        // No setup: black is blanking
        blanking = black16bIre >= 0 ? black16bIre : levels.blanking * 64;
        black = blanking;
    } else if (black16bIre >= 0) {
        // Black on the 7.5 IRE setup above blanking
        blanking = black - static_cast<qint32>(std::lround(7.5 * (white - black) / 92.5));
    } else {
        blanking = levels.blanking * 64;
    }
    videoParameters.white16bIre = white;
    videoParameters.black16bIre = black;
    videoParameters.blanking16bIre = blanking;

    initialiseVideoSystemParameters();
    videoParameters.sampleRate = 4.0 * videoParameters.fSC;
}

void LdDecodeMetaData::setGeneratedFieldOrder(bool reversed)
{
    generatedReversed = reversed;
    for (qint32 i = 0; i < fields.size(); i++) fields[i].isFirstField = ((i % 2) == 0) != reversed;
}

void LdDecodeMetaData::ensureFields(qint32 numberOfFields)
{
    const qint32 have = fields.size();
    if (numberOfFields <= have) return;

    fields.resize(numberOfFields);
    for (qint32 i = have; i < numberOfFields; i++) {
        fields[i].seqNo = i + 1;
        fields[i].isFirstField = ((i % 2) == 0) != generatedReversed;
        if (fieldProvider) {
            Field provided;
            if (fieldProvider(i + 1, provided)) {
                provided.seqNo = i + 1;
                fields[i] = provided;
            }
        }
    }
    videoParameters.numberOfSequentialFields = numberOfFields;
}

// Read CVBS core metadata. It holds no per-field metadata, so the fields are
// filled in as a TBC capture of the same video would have them, with the
// dropouts of the dropout extension.
bool LdDecodeMetaData::readCvbsCore(QString fileName)
{
    clear();

    Cvbs::FileMetadata fileMetadata;
    if (!fileMetadata.read(fileName)) return false;

    // Fixed line lengths and 4fsc sampling are what the tools are built on
    if (!fileMetadata.isStandardStable()) {
        qCritical().noquote() << fileName << "describes a" << fileMetadata.signalStatePreset
                              << "capture - only time-base stable 4fsc captures (STANDARD_STABLE_*) can be processed";
        return false;
    }

    const QString dataFileName = Cvbs::dataFileForMetadata(fileName);
    const Cvbs::Geometry geometry = Cvbs::geometry(fileMetadata.system);

    qint32 numberOfFrames = fileMetadata.numberOfFrames;
    if (numberOfFrames < 1) {
        // Not recorded: count the frames in the data file
        const QFileInfo dataFile(dataFileName);
        if (!dataFile.exists()) {
            qCritical().noquote() << fileName << "does not give the number of frames and" << dataFileName
                                  << "does not exist";
            return false;
        }
        numberOfFrames = static_cast<qint32>(dataFile.size() / (geometry.frameSamples * 2));
    }

    const Cvbs::Levels levels = Cvbs::standardLevels(fileMetadata.system);

    setStandardVideoParameters(fileMetadata.system);
    videoParameters.numberOfSequentialFields = numberOfFrames * 2;
    videoParameters.isSubcarrierLocked = fileMetadata.isPhaseLocked();

    // The 16-bit TBC domain is the 10-bit CVBS domain times 64
    videoParameters.white16bIre = levels.white * 64;
    videoParameters.blanking16bIre = levels.blanking * 64;
    videoParameters.black16bIre = fileMetadata.blackLevel.value_or(levels.black) * 64;

    // CVBS capture notes are free text, not the tape format the TBC
    // capture_notes column holds, so they stay in the core metadata
    videoParameters.gitBranch = fileMetadata.gitBranch;
    videoParameters.gitCommit = fileMetadata.gitCommit;

    // Frames hold the first field, then the second
    fields.resize(numberOfFrames * 2);
    for (qint32 i = 0; i < fields.size(); i++) {
        fields[i].seqNo = i + 1;
        fields[i].isFirstField = (i % 2) == 0;
    }

    Cvbs::readDropouts(Cvbs::dropoutsFileName(dataFileName), fileMetadata.system, numberOfFrames,
                       [this](qint32 fieldIndex, const DropOuts &dropOuts) {
                           fields[fieldIndex].dropOuts = dropOuts;
                       });

    // Field phase IDs are not in the metadata: measure them from the colour
    // burst, as decode-orc does (on the chroma file of a Y/C pair)
    const bool isYc = Cvbs::channel(dataFileName) != Cvbs::Channel::Composite;
    const QString burstFileName = isYc ? Cvbs::dataFileName(Cvbs::baseName(dataFileName), Cvbs::Channel::Chroma)
                                       : dataFileName;
    if (QFileInfo::exists(burstFileName)) {
        for (Field &field: fields) field.fieldPhaseID = -1;
        // What was measured before is kept beside the file (<file>.phases) while the file is as it was
        QVector<qint32> measured;
        if (Cvbs::readPhaseCache(burstFileName, fields.size(), measured)) {
            for (qint32 i = 0; i < fields.size(); i++) fields[i].fieldPhaseID = measured[i];
        } else {
            if (!Cvbs::measureFieldPhaseIDs(burstFileName, fileMetadata.system, fileMetadata.sampleEncoding, isYc,
                                            numberOfFrames, [this](qint32 fieldIndex, qint32 phaseID) {
                                                fields[fieldIndex].fieldPhaseID = phaseID;
                                            })) {
                qWarning().noquote() << "Could not measure the field phases of" << burstFileName;
            } else {
                measured.resize(fields.size());
                for (qint32 i = 0; i < fields.size(); i++) measured[i] = fields[i].fieldPhaseID;
                Cvbs::writePhaseCache(burstFileName, measured);
            }
        }

        // NTSC fields with no measurable burst follow the colour sequence (1 to 4, a
        // field at a time); the measured ones are left as they are
        if (fileMetadata.system == NTSC) {
            QVector<qint32> phaseIDs(fields.size());
            for (qint32 i = 0; i < fields.size(); i++) phaseIDs[i] = fields[i].fieldPhaseID;
            Cvbs::fillPhaseGaps(phaseIDs, 4);
            for (qint32 i = 0; i < fields.size(); i++) fields[i].fieldPhaseID = phaseIDs[i];
        }
    }

    // The audio of the capture (one channel pair): give each field its share of it,
    // as the field metadata of a TBC capture does, so tools can find the audio of a field
    CvbsAudio::WavInfo wavInfo;
    const QString wavFileName = CvbsAudio::wavFileName(Cvbs::baseName(dataFileName), 0);
    if (QFileInfo::exists(wavFileName) && CvbsAudio::readWavInfo(wavFileName, wavInfo)) {
        const qint64 numberOfFields = fields.size();
        for (qint64 i = 0; i < numberOfFields; i++) {
            fields[static_cast<qint32>(i)].audioSamples = static_cast<qint32>(
                (wavInfo.numberOfSamples * (i + 1) + numberOfFields / 2) / numberOfFields
                - (wavInfo.numberOfSamples * i + numberOfFields / 2) / numberOfFields);
        }
    }

    initialiseVideoSystemParameters();
    videoParameters.sampleRate = 4.0 * videoParameters.fSC;
    generatePcmAudioMap();

    cvbsFileMetadata = std::make_unique<Cvbs::FileMetadata>(fileMetadata);
    tbcDebugStream() << "LdDecodeMetaData::readCvbsCore(): Read" << numberOfFrames << "frames of CVBS metadata from"
                     << fileName;
    return true;
}

// Write the metadata of a CVBS capture: the companion with all the field
// metadata, plus the core metadata and dropout extension that decode-orc reads
bool LdDecodeMetaData::writeCvbs(QString fileName) const
{
    const QString dataFileName = Cvbs::dataFileForMetadata(fileName);

    // Writing to <base>.meta itself puts the field metadata in the companion
    const QString companionFileName = fileName.endsWith(".meta", Qt::CaseInsensitive)
            ? Cvbs::companionFileName(dataFileName, format == MetadataFormat::Json)
            : fileName;
    const bool companionIsJson = companionFileName.endsWith(".json", Qt::CaseInsensitive);
    if (!(companionIsJson ? writeJson(companionFileName) : writeSqlite(companionFileName))) return false;

    const qint32 numberOfFrames = fields.size() / 2;

    Cvbs::FileMetadata fileMetadata;
    if (cvbsFileMetadata) {
        fileMetadata = *cvbsFileMetadata;
    } else {
        // A capture becoming CVBS: describe it from its video parameters
        fileMetadata.signalStatePreset = videoParameters.isSubcarrierLocked ? "STANDARD_STABLE_LOCKED"
                                                                            : "STANDARD_STABLE_UNLOCKED";
        fileMetadata.gitBranch = videoParameters.gitBranch;
        fileMetadata.gitCommit = videoParameters.gitCommit;
        fileMetadata.captureNotes = videoParameters.tapeFormat;
    }
    fileMetadata.system = videoParameters.system;
    fileMetadata.signalType = (Cvbs::channel(dataFileName) == Cvbs::Channel::Composite) ? "composite" : "yc";
    fileMetadata.numberOfFrames = numberOfFrames;

    // A black level clearly away from the standard's (NTSC-J, for one) is an
    // override; a measured level within about 2 IRE of it is not
    if (videoParameters.black16bIre >= 0) {
        const qint32 black = (videoParameters.black16bIre + 32) / 64;
        if (std::abs(black - Cvbs::standardLevels(videoParameters.system).black) > 10) {
            fileMetadata.blackLevel = black;
            fileMetadata.hasNonstandardValues = true;
        } else {
            fileMetadata.blackLevel.reset();
        }
    }

    if (!fileMetadata.write(Cvbs::metaFileName(dataFileName))) return false;

    return Cvbs::writeDropouts(Cvbs::dropoutsFileName(dataFileName), videoParameters.system, numberOfFrames,
                               [this](qint32 fieldIndex) -> const DropOuts & { return fields[fieldIndex].dropOuts; });
}

// Read all metadata from a JSON file
bool LdDecodeMetaData::readJson(QString fileName)
{
    std::ifstream jsonFile(fileName.toStdString());
    if (jsonFile.fail()) {
        qCritical("Opening JSON input file failed: JSON file cannot be opened/does not exist");
        return false;
    }

    clear();

    JsonReader reader(jsonFile);

    try {
        reader.beginObject();

        std::string member;
        while (reader.readMember(member)) {
            if (member == "fields") readFields(reader);
            else if (member == "ldxMap") {
                FrameMap map;
                QString mapError;
                if (map.read(reader, mapError)) setDiscMap(map);
                else qWarning().noquote() << "The map in" << fileName << "is not used:" << mapError;
            }
            else if (member == "ldxDecoding") {
                DecodingParameters parameters;
                QString decodingError;
                if (parameters.read(reader, decodingError)) setDecodingParameters(parameters);
                else qWarning().noquote() << "The decoding parameters in" << fileName << "are not used:" << decodingError;
            }
            else if (member == "pcmAudioParameters") pcmAudioParameters.read(reader);
            else if (member == "videoParameters") videoParameters.read(reader);
            else reader.discard();
        }

        reader.endObject();
    } catch (JsonReader::Error &error) {
        qCritical() << "Parsing JSON file failed:" << error.what();
        return false;
    }

    jsonFile.close();

    // Check we saw VideoParameters - if not, we can't do anything useful!
    if (!videoParameters.isValid) {
        qCritical("JSON file invalid: videoParameters object is not defined");
        return false;
    }

    // Check numberOfSequentialFields is consistent
    if (videoParameters.numberOfSequentialFields != fields.size()) {
        qCritical("JSON file invalid: numberOfSequentialFields does not match fields array");
        return false;
    }

    // Now we know the video system, initialise the rest of VideoParameters
    initialiseVideoSystemParameters();

    // Generate the PCM audio map based on the field metadata
    generatePcmAudioMap();

    return true;
}

// Write all metadata out to a JSON file
bool LdDecodeMetaData::writeJson(QString fileName) const
{
    std::ofstream jsonFile(fileName.toStdString());
    if (jsonFile.fail()) {
        qCritical("Opening JSON output file failed");
        return false;
    }

    JsonWriter writer(jsonFile);

    writer.beginObject();

    // Keep members in alphabetical order
    writer.writeMember("fields");
    writeFields(writer);
    if (decodingPresent) {
        writer.writeMember("ldxDecoding");
        decodingParameters.write(writer);
    }
    if (discMapPresent) {
        writer.writeMember("ldxMap");
        discMap.write(writer);
    }
    if (pcmAudioParameters.isValid) {
        writer.writeMember("pcmAudioParameters");
        pcmAudioParameters.write(writer);
    }
    writer.writeMember("videoParameters");
    videoParameters.write(writer);

    writer.endObject();

    jsonFile.close();

    return true;
}

// Read array of Fields from JSON
void LdDecodeMetaData::readFields(JsonReader &reader)
{
    reader.beginArray();

    while (reader.readElement()) {
        Field field;
        field.read(reader);
        fields.push_back(field);
    }

    reader.endArray();
}

// Write array of Fields to JSON
void LdDecodeMetaData::writeFields(JsonWriter &writer) const
{
    writer.beginArray();

    for (const Field &field : fields) {
        writer.writeElement();
        field.write(writer);
    }

    writer.endArray();
}

// Read all metadata from SQLite file
bool LdDecodeMetaData::readSqlite(QString fileName)
{
    if (!QFileInfo::exists(fileName)) {
        qCritical() << "SQLite input file does not exist:" << fileName;
        return false;
    }

    clear();

    try {
        SqliteReader reader(fileName);
        
        int captureId;
        QString system, decoder, gitBranch, gitCommit, captureNotes;
        double videoSampleRate;
        int activeVideoStart, activeVideoEnd, fieldWidth, fieldHeight, numberOfSequentialFields;
        int colourBurstStart, colourBurstEnd, white16bIre, black16bIre, blanking16bIre;
        bool isMapped, isSubcarrierLocked, isWidescreen;

        // Read capture metadata
        if (!reader.readCaptureMetadata(captureId, system, decoder, gitBranch, gitCommit,
                                       videoSampleRate, activeVideoStart, activeVideoEnd,
                                       fieldWidth, fieldHeight, numberOfSequentialFields,
                                       colourBurstStart, colourBurstEnd, isMapped,
                                       isSubcarrierLocked, isWidescreen, white16bIre,
                                       black16bIre, blanking16bIre, captureNotes)) {
            qCritical() << "Failed to read capture metadata from SQLite file";
            return false;
        }

        // Set video parameters
        videoParameters.numberOfSequentialFields = numberOfSequentialFields;
        if (!parseVideoSystemName(system, videoParameters.system)) {
            qCritical() << "Unknown video system:" << system;
            return false;
        }
        videoParameters.isSubcarrierLocked = isSubcarrierLocked;
        videoParameters.isWidescreen = isWidescreen;
        videoParameters.colourBurstStart = colourBurstStart;
        videoParameters.colourBurstEnd = colourBurstEnd;
        videoParameters.activeVideoStart = activeVideoStart;
        videoParameters.activeVideoEnd = activeVideoEnd;
        videoParameters.white16bIre = white16bIre;
        videoParameters.black16bIre = black16bIre;
        videoParameters.blanking16bIre = blanking16bIre;
        videoParameters.fieldWidth = fieldWidth;
        videoParameters.fieldHeight = fieldHeight;
        videoParameters.sampleRate = videoSampleRate;
        videoParameters.isMapped = isMapped;
        videoParameters.tapeFormat = captureNotes;
        if (!decoder.isEmpty()) videoParameters.decoder = decoder;
        videoParameters.gitBranch = gitBranch;
        videoParameters.gitCommit = gitCommit;
        videoParameters.isValid = true;

        // Read PCM audio parameters if they exist
        int bits;
        bool isSigned, isLittleEndian;
        double audioSampleRate;
        QByteArray decodingJson;
        if (reader.readJsonTable(captureId, "decoding", "parameters", decodingJson)) {
            DecodingParameters parameters;
            QString decodingError;
            if (DecodingParameters::fromJsonBytes(decodingJson, parameters, decodingError)) setDecodingParameters(parameters);
            else qWarning().noquote() << "The decoding parameters in" << fileName << "are not used:" << decodingError;
        }
        QByteArray mapJson;
        if (reader.readDiscMap(captureId, mapJson)) {
            FrameMap map;
            QString mapError;
            if (FrameMap::fromJsonBytes(mapJson, map, mapError)) setDiscMap(map);
            else qWarning().noquote() << "The map in" << fileName << "is not used:" << mapError;
        }
        if (reader.readPcmAudioParameters(captureId, bits, isSigned, isLittleEndian, audioSampleRate)) {
            pcmAudioParameters.bits = bits;
            pcmAudioParameters.isSigned = isSigned;
            pcmAudioParameters.isLittleEndian = isLittleEndian;
            pcmAudioParameters.sampleRate = audioSampleRate;
            pcmAudioParameters.isValid = true;
        }

        // Read all fields
        readFields(reader, captureId);

    } catch (SqliteReader::Error &error) {
        qCritical() << "Reading SQLite file failed:" << error.what();
        return false;
    }

    // Now we know the video system, initialise the rest of VideoParameters
    initialiseVideoSystemParameters();

    // Generate the PCM audio map based on the field metadata
    generatePcmAudioMap();

    return true;
}

// Write all metadata out to an SQLite file
bool LdDecodeMetaData::writeSqlite(QString fileName, unsigned tables) const
{
    // Check if we're updating an existing file or creating a new one
    bool isUpdate = QFileInfo::exists(fileName);
    int captureId = 1; // Default for new files
    
    if (isUpdate) {
        // Try to read the existing capture_id from the file
        try {
            SqliteReader reader(fileName);
            QString existingSystem, existingDecoder, existingGitBranch, existingGitCommit, existingCaptureNotes;
            double existingVideoSampleRate;
            int existingActiveVideoStart, existingActiveVideoEnd, existingFieldWidth, existingFieldHeight;
            int existingNumberOfSequentialFields, existingColourBurstStart, existingColourBurstEnd;
            int existingWhite16bIre, existingBlack16bIre, existingBlanking16bIre;
            bool existingIsMapped, existingIsSubcarrierLocked, existingIsWidescreen;
            
            if (reader.readCaptureMetadata(captureId, existingSystem, existingDecoder, 
                                         existingGitBranch, existingGitCommit, existingVideoSampleRate,
                                         existingActiveVideoStart, existingActiveVideoEnd, 
                                         existingFieldWidth, existingFieldHeight, existingNumberOfSequentialFields,
                                         existingColourBurstStart, existingColourBurstEnd,
                                         existingIsMapped, existingIsSubcarrierLocked, existingIsWidescreen,
                                         existingWhite16bIre, existingBlack16bIre, existingBlanking16bIre, existingCaptureNotes)) {
                tbcDebugStream() << "Updating existing SQLite file with capture_id:" << captureId;
            } else {
                qWarning() << "Could not read existing capture metadata, treating as new file";
                isUpdate = false;
                captureId = 1;
            }
        } catch (...) {
            qWarning() << "Error reading existing SQLite file, treating as new file";
            isUpdate = false;
            captureId = 1;
        }
    }

    try {
        SqliteWriter writer(fileName);
        
        // Only create schema for new files
        if (!isUpdate) {
            if (!writer.createSchema()) {
                qCritical() << "Failed to create SQLite schema";
                return false;
            }
        }

        if (!writer.beginTransaction()) {
            qCritical() << "Failed to begin transaction";
            return false;
        }

        // A tool that changes some of the tables of the fields only, on a file that exists, writes those tables and nothing else
        const bool partial = isUpdate && tables != AllTables;
        const QString decoderName = videoParameters.decoder.isEmpty() ? QString("ld-decode") : videoParameters.decoder;

        // Write or update capture metadata
        QString systemName = getVideoSystemDescription();
        if (partial) {
            // (the capture itself is not changed)
        } else if (isUpdate) {
            // Update existing capture metadata
            if (!writer.updateCaptureMetadata(captureId, systemName, decoderName,
                                            videoParameters.gitBranch, videoParameters.gitCommit,
                                            videoParameters.sampleRate, videoParameters.activeVideoStart, 
                                            videoParameters.activeVideoEnd, videoParameters.fieldWidth,
                                            videoParameters.fieldHeight, videoParameters.numberOfSequentialFields,
                                            videoParameters.colourBurstStart, videoParameters.colourBurstEnd,
                                            videoParameters.isMapped, videoParameters.isSubcarrierLocked,
                                            videoParameters.isWidescreen, videoParameters.white16bIre,
                                            videoParameters.black16bIre, videoParameters.blanking16bIre, videoParameters.tapeFormat)) {
                writer.rollbackTransaction();
                return false;
            }
        } else {
            // Create new capture metadata
            captureId = writer.writeCaptureMetadata(
                systemName, decoderName,
                videoParameters.gitBranch, videoParameters.gitCommit,
                videoParameters.sampleRate, videoParameters.activeVideoStart, 
                videoParameters.activeVideoEnd, videoParameters.fieldWidth,
                videoParameters.fieldHeight, videoParameters.numberOfSequentialFields,
                videoParameters.colourBurstStart, videoParameters.colourBurstEnd,
                videoParameters.isMapped, videoParameters.isSubcarrierLocked,
                videoParameters.isWidescreen, videoParameters.white16bIre,
                videoParameters.black16bIre, videoParameters.blanking16bIre, videoParameters.tapeFormat);

            if (captureId == -1) {
                writer.rollbackTransaction();
                return false;
            }
        }

        // How the capture is decoded to colour (taken away from the file if the metadata have none)
        if (!partial && !writer.writeJsonTable(captureId, "decoding", "parameters", decodingPresent ? decodingParameters.toJsonBytes() : QByteArray())) {
            writer.rollbackTransaction();
            return false;
        }

        // The map of the capture (taken away from the file if the metadata have none)
        if (!partial && !writer.writeDiscMap(captureId, discMapPresent ? discMap.toJsonBytes() : QByteArray())) {
            writer.rollbackTransaction();
            return false;
        }

        // Write PCM audio parameters if they exist
        if (!partial && pcmAudioParameters.isValid) {
            if (!writer.writePcmAudioParameters(captureId, pcmAudioParameters.bits,
                                              pcmAudioParameters.isSigned,
                                              pcmAudioParameters.isLittleEndian,
                                              pcmAudioParameters.sampleRate)) {
                writer.rollbackTransaction();
                return false;
            }
        }

        // The rows of the tables that are written again are taken away first, so that a row the metadata no longer have (a dropout that was
        // corrected, a VBI that was cleared) does not stay in the file
        if (isUpdate) {
            const struct { unsigned table; const char *name; } rowTables[] = {
                { VitsTable, "vits_metrics" }, { VbiTable, "vbi" }, { VitcTable, "vitc" },
                { ClosedCaptionTable, "closed_caption" }, { DropOutsTable, "drop_outs" }
            };
            for (const auto &entry : rowTables) {
                if ((tables & entry.table) && !writer.deleteFieldRows(captureId, entry.name)) {
                    writer.rollbackTransaction();
                    return false;
                }
            }
        }

        // Write all fields
        writeFields(writer, captureId, tables);

        if (!writer.commitTransaction()) {
            qCritical() << "Failed to commit transaction";
            return false;
        }

    } catch (SqliteWriter::Error &error) {
        qCritical() << "Writing SQLite file failed:" << error.what();
        return false;
    }

    return true;
}

// Read array of Fields from SQLite (optimized version)
void LdDecodeMetaData::readFields(SqliteReader &reader, int captureId)
{
    QSqlQuery fieldsQuery;
    if (!reader.readFields(captureId, fieldsQuery)) {
        return;
    }

    // Pre-read all optional field data in bulk for performance
    QSqlQuery vitsQuery, vbiQuery, vitcQuery, ccQuery, dropoutsQuery;
    reader.readAllFieldVitsMetrics(captureId, vitsQuery);
    reader.readAllFieldVbi(captureId, vbiQuery);
    reader.readAllFieldVitc(captureId, vitcQuery);
    reader.readAllFieldClosedCaptions(captureId, ccQuery);
    reader.readAllFieldDropouts(captureId, dropoutsQuery);

    // Create lookup maps for fast field data retrieval. The queries give the columns in the order of their SELECT (the field first), and are
    // read by the number of the column: a capture of an hour has millions of rows, and a column found by its name costs more than the row.
    QHash<int, QPair<double, double>> vitsMap;
    QHash<int, std::array<qint32, 3>> vbiMap;
    QHash<int, std::array<qint32, 8>> vitcMap;
    QHash<int, std::array<qint32, 2>> ccMap;
    QHash<int, DropOuts> dropoutsMap;

    // Populate VITS metrics map
    while (vitsQuery.next()) {
        vitsMap[vitsQuery.value(0).toInt()] = qMakePair(vitsQuery.value(1).toDouble(), vitsQuery.value(2).toDouble());
    }

    // Populate VBI map
    while (vbiQuery.next()) {
        vbiMap[vbiQuery.value(0).toInt()] = { vbiQuery.value(1).toInt(), vbiQuery.value(2).toInt(), vbiQuery.value(3).toInt() };
    }

    // Populate VITC map
    while (vitcQuery.next()) {
        std::array<qint32, 8> vitcData;
        for (int i = 0; i < 8; i++) vitcData[i] = vitcQuery.value(1 + i).toInt();
        vitcMap[vitcQuery.value(0).toInt()] = vitcData;
    }

    // Populate closed captions map
    while (ccQuery.next()) {
        ccMap[ccQuery.value(0).toInt()] = { ccQuery.value(1).toInt(), ccQuery.value(2).toInt() };
    }

    // Populate dropouts map: the rows of a field come together (by field, then by start), and the dropouts of a field are kept in that order, the
    // order of the file, as they are when read from JSON
    {
        int currentField = 0;
        bool haveField = false;
        QVector<qint32> startx, endx, fieldLine;
        const auto finishField = [&]() {
            if (!haveField) return;
            dropoutsMap.insert(currentField, DropOuts(startx, endx, fieldLine));
            startx = QVector<qint32>();
            endx = QVector<qint32>();
            fieldLine = QVector<qint32>();
        };
        while (dropoutsQuery.next()) {
            const int fieldId = dropoutsQuery.value(0).toInt();
            if (!haveField || fieldId != currentField) {
                finishField();
                currentField = fieldId;
                haveField = true;
            }
            startx.append(dropoutsQuery.value(1).toInt());
            endx.append(dropoutsQuery.value(2).toInt());
            fieldLine.append(dropoutsQuery.value(3).toInt());
        }
        finishField();
    }

    // The columns of the main field records, by number
    const QSqlRecord record = fieldsQuery.record();
    const int fieldIdColumn = record.indexOf("field_id");
    const int isFirstFieldColumn = record.indexOf("is_first_field");
    const int syncConfColumn = record.indexOf("sync_conf");
    const int medianBurstIreColumn = record.indexOf("median_burst_ire");
    const int fieldPhaseIdColumn = record.indexOf("field_phase_id");
    const int audioSamplesColumn = record.indexOf("audio_samples");
    const int diskLocColumn = record.indexOf("disk_loc");
    const int fileLocColumn = record.indexOf("file_loc");
    const int decodeFaultsColumn = record.indexOf("decode_faults");
    const int efmTValuesColumn = record.indexOf("efm_t_values");
    const int padColumn = record.indexOf("pad");
    const int ntscFmCodeDataValidColumn = record.indexOf("ntsc_is_fm_code_data_valid");
    const int ntscFmCodeDataColumn = record.indexOf("ntsc_fm_code_data");
    const int ntscFieldFlagColumn = record.indexOf("ntsc_field_flag");
    const int ntscVideoIdDataValidColumn = record.indexOf("ntsc_is_video_id_data_valid");
    const int ntscVideoIdDataColumn = record.indexOf("ntsc_video_id_data");
    const int ntscWhiteFlagColumn = record.indexOf("ntsc_white_flag");

    // A NULL is the "missing" of the field (-1 for most, as in the files of ld-decode), not a 0
    const auto intOr = [&](int column, int defaultValue) {
        const QVariant value = fieldsQuery.value(column);
        return value.isNull() ? defaultValue : value.toInt();
    };
    const auto doubleOr = [&](int column, double defaultValue) {
        const QVariant value = fieldsQuery.value(column);
        return value.isNull() ? defaultValue : value.toDouble();
    };
    const auto boolOr = [&](int column, bool defaultValue) {
        const QVariant value = fieldsQuery.value(column);
        return value.isNull() ? defaultValue : value.toInt() == 1;
    };

    fields.reserve(qMax<qint32>(videoParameters.numberOfSequentialFields, 0));

    // Process main field records and apply cached data
    while (fieldsQuery.next()) {
        Field field;

        // Note: field_id in database is 0-indexed, but seqNo should be 1-indexed
        const int fieldId = fieldsQuery.value(fieldIdColumn).toInt();
        field.seqNo = fieldId + 1;
        field.isFirstField = boolOr(isFirstFieldColumn, false);
        field.syncConf = intOr(syncConfColumn, 0);
        field.medianBurstIRE = doubleOr(medianBurstIreColumn, 0.0);
        field.fieldPhaseID = intOr(fieldPhaseIdColumn, -1);
        field.audioSamples = intOr(audioSamplesColumn, -1);
        field.diskLoc = doubleOr(diskLocColumn, -1.0);
        {
            const QVariant value = fieldsQuery.value(fileLocColumn);
            field.fileLoc = value.isNull() ? -1 : value.toLongLong();
        }
        field.decodeFaults = intOr(decodeFaultsColumn, -1);
        field.efmTValues = intOr(efmTValuesColumn, -1);
        field.pad = boolOr(padColumn, false);

        // Read NTSC data from the main field record
        field.ntsc.isFmCodeDataValid = fieldsQuery.value(ntscFmCodeDataValidColumn).toInt() == 1;
        field.ntsc.fmCodeData = fieldsQuery.value(ntscFmCodeDataColumn).toInt();
        field.ntsc.fieldFlag = fieldsQuery.value(ntscFieldFlagColumn).toInt() == 1;
        field.ntsc.isVideoIdDataValid = fieldsQuery.value(ntscVideoIdDataValidColumn).toInt() == 1;
        field.ntsc.videoIdData = fieldsQuery.value(ntscVideoIdDataColumn).toInt();
        field.ntsc.whiteFlag = fieldsQuery.value(ntscWhiteFlagColumn).toInt() == 1;
        field.ntsc.inUse = field.ntsc.isFmCodeDataValid || field.ntsc.isVideoIdDataValid;

        // Apply cached optional field data
        const auto vits = vitsMap.constFind(fieldId);
        if (vits != vitsMap.constEnd()) {
            field.vitsMetrics.wSNR = vits->first;
            field.vitsMetrics.bPSNR = vits->second;
            field.vitsMetrics.inUse = true;
        }

        const auto vbi = vbiMap.constFind(fieldId);
        if (vbi != vbiMap.constEnd()) {
            field.vbi.vbiData[0] = (*vbi)[0];
            field.vbi.vbiData[1] = (*vbi)[1];
            field.vbi.vbiData[2] = (*vbi)[2];
            field.vbi.inUse = true;
        }

        const auto vitc = vitcMap.constFind(fieldId);
        if (vitc != vitcMap.constEnd()) {
            for (int i = 0; i < 8; i++) field.vitc.vitcData[i] = (*vitc)[i];
            field.vitc.inUse = true;
        }

        const auto cc = ccMap.constFind(fieldId);
        if (cc != ccMap.constEnd()) {
            field.closedCaption.data0 = (*cc)[0];
            field.closedCaption.data1 = (*cc)[1];
            field.closedCaption.inUse = true;
        }

        const auto dropouts = dropoutsMap.constFind(fieldId);
        if (dropouts != dropoutsMap.constEnd()) {
            field.dropOuts = *dropouts;
        }

        fields.push_back(field);
    }
}

// Write array of Fields to SQLite
void LdDecodeMetaData::writeFields(SqliteWriter &writer, int captureId, unsigned tables) const
{
    for (const Field &field : fields) {
        field.write(writer, captureId, tables);
    }
}

// This method returns the videoParameters metadata
const LdDecodeMetaData::VideoParameters &LdDecodeMetaData::getVideoParameters()
{
    if (!videoParameters.isValid) {
        throw std::runtime_error("VideoParameters not initialized - metadata file may not have been read successfully");
    }
    return videoParameters;
}

// This method sets the videoParameters metadata
void LdDecodeMetaData::setVideoParameters(const LdDecodeMetaData::VideoParameters &_videoParameters)
{
    videoParameters = _videoParameters;
    videoParameters.isValid = true;
}

// This method returns the pcmAudioParameters metadata
bool LdDecodeMetaData::hasPcmAudioParameters() const
{
    return pcmAudioParameters.isValid;
}

const LdDecodeMetaData::PcmAudioParameters &LdDecodeMetaData::getPcmAudioParameters()
{
    if (!pcmAudioParameters.isValid) {
        throw std::runtime_error("PcmAudioParameters not initialized - metadata file may not have been read successfully");
    }
    return pcmAudioParameters;
}

// This method sets the pcmAudioParameters metadata
void LdDecodeMetaData::setPcmAudioParameters(const LdDecodeMetaData::PcmAudioParameters &_pcmAudioParameters)
{
    pcmAudioParameters = _pcmAudioParameters;
    pcmAudioParameters.isValid = true;
}

// Based on the video system selected, set default values for the members of
// VideoParameters that aren't obtained from the metadata.
void LdDecodeMetaData::initialiseVideoSystemParameters()
{
    const VideoSystemDefaults &defaults = getSystemDefaults(videoParameters);
    videoParameters.fSC = defaults.fSC;

    // Set default LineParameters
    LdDecodeMetaData::LineParameters lineParameters;
    processLineParameters(lineParameters);
}

// Validate LineParameters and apply them to the VideoParameters
void LdDecodeMetaData::processLineParameters(LdDecodeMetaData::LineParameters &lineParameters)
{
    lineParameters.applyTo(videoParameters);
}

// Validate and apply to a set of VideoParameters
void LdDecodeMetaData::LineParameters::applyTo(LdDecodeMetaData::VideoParameters &videoParameters)
{
    const bool firstFieldLineExists = firstActiveFieldLine != -1;
    const bool lastFieldLineExists = lastActiveFieldLine != -1;
    const bool firstFrameLineExists = firstActiveFrameLine != -1;
    const bool lastFrameLineExists = lastActiveFrameLine != -1;

    const VideoSystemDefaults &defaults = getSystemDefaults(videoParameters);
    const qint32 minFirstFrameLine = defaults.minActiveFrameLine;
    const qint32 defaultFirstFieldLine = defaults.firstActiveFieldLine;
    const qint32 defaultLastFieldLine = defaults.lastActiveFieldLine;
    const qint32 defaultFirstFrameLine = defaults.firstActiveFrameLine;
    const qint32 defaultLastFrameLine = defaults.lastActiveFrameLine;

    // Validate and potentially fix the first active field line.
    if (firstActiveFieldLine < 1 || firstActiveFieldLine > defaultLastFieldLine) {
        if (firstFieldLineExists) {
            qInfo().nospace() << "Specified first active field line " << firstActiveFieldLine << " out of bounds (1 to "
                              << defaultLastFieldLine << "), resetting to default (" << defaultFirstFieldLine << ").";
        }
        firstActiveFieldLine = defaultFirstFieldLine;
    }

    // Validate and potentially fix the last active field line.
    if (lastActiveFieldLine < 1 || lastActiveFieldLine > defaultLastFieldLine) {
        if (lastFieldLineExists) {
            qInfo().nospace() << "Specified last active field line " << lastActiveFieldLine << " out of bounds (1 to "
                              << defaultLastFieldLine << "), resetting to default (" << defaultLastFieldLine << ").";
        }
        lastActiveFieldLine = defaultLastFieldLine;
    }

    // Range-check the first and last active field lines.
    if (firstActiveFieldLine > lastActiveFieldLine) {
       qInfo().nospace() << "Specified last active field line " << lastActiveFieldLine << " is before specified first active field line"
                         << firstActiveFieldLine << ", resetting to defaults (" << defaultFirstFieldLine << "-" << defaultLastFieldLine << ").";
        firstActiveFieldLine = defaultFirstFieldLine;
        lastActiveFieldLine = defaultLastFieldLine;
    }

    // Validate and potentially fix the first active frame line.
    if (firstActiveFrameLine < minFirstFrameLine || firstActiveFrameLine > defaultLastFrameLine) {
        if (firstFrameLineExists) {
            qInfo().nospace() << "Specified first active frame line " << firstActiveFrameLine << " out of bounds (" << minFirstFrameLine << " to "
                              << defaultLastFrameLine << "), resetting to default (" << defaultFirstFrameLine << ").";
        }
        firstActiveFrameLine = defaultFirstFrameLine;
    }

    // Validate and potentially fix the last active frame line.
    if (lastActiveFrameLine < minFirstFrameLine || lastActiveFrameLine > defaultLastFrameLine) {
        if (lastFrameLineExists) {
            qInfo().nospace() << "Specified last active frame line " << lastActiveFrameLine << " out of bounds (" << minFirstFrameLine << " to "
                              << defaultLastFrameLine << "), resetting to default (" << defaultLastFrameLine << ").";
        }
        lastActiveFrameLine = defaultLastFrameLine;
    }

    // Range-check the first and last active frame lines.
    if (firstActiveFrameLine > lastActiveFrameLine) {
        qInfo().nospace() << "Specified last active frame line " << lastActiveFrameLine << " is before specified first active frame line"
                          << firstActiveFrameLine << ", resetting to defaults (" << defaultFirstFrameLine << "-" << defaultLastFrameLine << ").";
        firstActiveFrameLine = defaultFirstFrameLine;
        lastActiveFrameLine = defaultLastFrameLine;
    }

    // Store the new values back into videoParameters
    videoParameters.firstActiveFieldLine = firstActiveFieldLine;
    videoParameters.lastActiveFieldLine = lastActiveFieldLine;
    videoParameters.firstActiveFrameLine = firstActiveFrameLine;
    videoParameters.lastActiveFrameLine = lastActiveFrameLine;
}

// This method gets the metadata for the specified sequential field number (indexed from 1 (not 0!))
const LdDecodeMetaData::Field &LdDecodeMetaData::getField(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__);
}

// The field of a sequential field number (from 1). One that is not there is said, and an empty field is given (what is written
// to it is lost): the vector is never indexed out of its bounds
LdDecodeMetaData::Field &LdDecodeMetaData::fieldAt(qint32 sequentialFieldNumber, const char *caller)
{
    const qint32 fieldNumber = sequentialFieldNumber - 1;
    if (fieldNumber < 0 || fieldNumber >= getNumberOfFields()) {
        qCritical() << "LdDecodeMetaData::" << caller << "(): Requested field number" << sequentialFieldNumber << "out of bounds!";
        static thread_local Field missing;
        missing = Field();
        return missing;
    }

    return fields[fieldNumber];
}

// This method gets the VITS metrics metadata for the specified sequential field number
const LdDecodeMetaData::VitsMetrics &LdDecodeMetaData::getFieldVitsMetrics(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).vitsMetrics;
}

// This method gets the VBI metadata for the specified sequential field number
const LdDecodeMetaData::Vbi &LdDecodeMetaData::getFieldVbi(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).vbi;
}

// This method gets the NTSC metadata for the specified sequential field number
const LdDecodeMetaData::Ntsc &LdDecodeMetaData::getFieldNtsc(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).ntsc;
}

// This method gets the VITC metadata for the specified sequential field number
const LdDecodeMetaData::Vitc &LdDecodeMetaData::getFieldVitc(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).vitc;
}

// This method gets the Closed Caption metadata for the specified sequential field number
const LdDecodeMetaData::ClosedCaption &LdDecodeMetaData::getFieldClosedCaption(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).closedCaption;
}

// This method gets the drop-out metadata for the specified sequential field number
const DropOuts &LdDecodeMetaData::getFieldDropOuts(qint32 sequentialFieldNumber)
{
    return fieldAt(sequentialFieldNumber, __func__).dropOuts;
}

// This method sets the field metadata for a field
void LdDecodeMetaData::updateField(const LdDecodeMetaData::Field &field, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__) = field;
}

// This method sets the field VBI metadata for a field
void LdDecodeMetaData::updateFieldVitsMetrics(const LdDecodeMetaData::VitsMetrics &vitsMetrics, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).vitsMetrics = vitsMetrics;
}

// This method sets the field VBI metadata for a field
void LdDecodeMetaData::updateFieldVbi(const LdDecodeMetaData::Vbi &vbi, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).vbi = vbi;
}

// This method sets the field NTSC metadata for a field
void LdDecodeMetaData::updateFieldNtsc(const LdDecodeMetaData::Ntsc &ntsc, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).ntsc = ntsc;
}

// This method sets the VITC metadata for a field
void LdDecodeMetaData::updateFieldVitc(const LdDecodeMetaData::Vitc &vitc, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).vitc = vitc;
}

// This method sets the Closed Caption metadata for a field
void LdDecodeMetaData::updateFieldClosedCaption(const LdDecodeMetaData::ClosedCaption &closedCaption, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).closedCaption = closedCaption;
}

// This method sets the field dropout metadata for a field
void LdDecodeMetaData::updateFieldDropOuts(const DropOuts &dropOuts, qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).dropOuts = dropOuts;
}

// This method clears the field dropout metadata for a field
void LdDecodeMetaData::clearFieldDropOuts(qint32 sequentialFieldNumber)
{
    fieldAt(sequentialFieldNumber, __func__).dropOuts.clear();
}

// This method appends a new field to the existing metadata
void LdDecodeMetaData::appendField(const LdDecodeMetaData::Field &field)
{
    // Ensure sequential numbering stays contiguous when writing out
    LdDecodeMetaData::Field fieldCopy = field;
    fieldCopy.seqNo = fields.size() + 1;
    fields.append(fieldCopy);

    videoParameters.numberOfSequentialFields = fields.size();
}

void LdDecodeMetaData::clearFields()
{
    fields.clear();
    videoParameters.numberOfSequentialFields = 0;
}

// Method to get the available number of fields (according to the metadata)
qint32 LdDecodeMetaData::getNumberOfFields()
{
    return fields.size();
}

// Method to set the available number of fields
// XXX This is unnecessary given appendField
void LdDecodeMetaData::setNumberOfFields(qint32 numberOfFields)
{
    videoParameters.numberOfSequentialFields = numberOfFields;
}

// A note about fields, frames and still-frames:
//
// There is a lot of confusing terminology around fields and the order in which
// they should be combined to make a 'frame'.  Basically, (taking NTSC as an example)
// a frame consists of frame lines numbered from 1 to 525.  A frame is made from two
// fields, one field contains field lines 1 to 262.5 and another 263 to 525 (although
// for convenence the 'half-lines' are usually treated as one full line and ignored
// so both fields contain a total of 263 lines of which 1 is ignored).
//
// When a frame is created, the field containing field lines 1-263 is interlaced
// with the field containing 263-525 creating a frame with field lines 1, 263, 2, 264
// and so on.  This 'frame' is then considered to contain frame lines 1-525.
//
// The field containing the first line of the frame is called the 'first field' and
// the field containing the second line of the frame is called the 'second field'.
//
// However, other names exist:
//
// Even/Odd - where the 'odd' field contains the odd line numbers 1, 3, 5, etc. This is
// the same as the 'first' field so odd = first and even = second.
//
// Upper/lower - where the 'upper' field contains the upper-part of each combination.
// This is the same as the first field so upper = first and lower = second.
//
// With a standard TV, as long as one field is first and the other is second, the only
// thing a TV requires is that the sequence of fields is constant.  They are simply
// displayed one set of fields after another to form a frame which is part of a
// moving image.
//
// This is an issue for 'still-frames' as, if the video sequence consists of
// still images (rather than motion), pausing at any given point can result in a
// frame containing a first field from one image and a second field from another
// as there is no concept of 'frame' in the video (just a sequence of first and
// second fields).
//
// Since digital formats are frame based (not field) this is an issue, as there
// is no way (from the video data) to tell how to combine fields into a
// still-frame (rather than just 'a frame').  The LaserDisc mastering could be
// in the order first field/second field = still-frame or second field/first field
// = still frame.
//
// This is why the following methods use the "isFirstFieldFirst" flag (which is
// a little confusing in itself).
//
// There are two ways to determine the 'isFirstFieldFirst'.  The first method is by
// user observation (it's pretty clear on a still-frame when it is wrong), the other
// (used by LaserDisc players) is to look for a CAV picture number in the VBI data
// of the field.  The IEC specification states that the picture number should only be
// in the first field of a frame. (Note: CLV discs don't really have to follow this
// as there are no 'still-frames' allowed by the original format).
//
// This gets even more confusing for NTSC discs using pull-down, where the sequence
// of fields making up the frames isn't even, so some field-pairs aren't considered
// to contain the first field of a still-frame (and when pausing the LaserDisc
// player will never use certain fields to render the still-frame).  Wikipedia is
// your friend if you want to learn more about it.
//
// Determining the correct setting of 'isFirstFieldFirst' is therefore outside of
// the shared-library scope.

// Method to get the available number of still-frames
qint32 LdDecodeMetaData::getNumberOfFrames()
{
    qint32 frameOffset = 0;

    // If the first field in the TBC input isn't the expected first field,
    // skip it when counting the number of still-frames
    if (isFirstFieldFirst) {
        // Expecting first field first
        if (!getField(1).isFirstField) frameOffset = 1;
    } else {
        // Expecting second field first
        if (getField(1).isFirstField) frameOffset = 1;
    }

    return (getNumberOfFields() / 2) - frameOffset;
}

// Method to get the first and second field numbers based on the frame number
// If field = 1 return the firstField, otherwise return second field
qint32 LdDecodeMetaData::getFieldNumber(qint32 frameNumber, qint32 field)
{
    qint32 firstFieldNumber = 0;
    qint32 secondFieldNumber = 0;

    // Verify the frame number
    if (frameNumber < 1) {
        qCritical() << "Invalid frame number, cannot determine fields";
        return -1;
    }

    // Calculate the first and last fields based on the position in the TBC
    if (isFirstFieldFirst) {
        // Expecting TBC file to provide still-frames as first field / second field
        firstFieldNumber = (frameNumber * 2) - 1;
        secondFieldNumber = firstFieldNumber + 1;
    } else {
        // Expecting TBC file to provide still-frames as second field / first field
        secondFieldNumber = (frameNumber * 2) - 1;
        firstFieldNumber = secondFieldNumber + 1;
    }

    // If the field number pointed to by firstFieldNumber doesn't have
    // isFirstField set, move forward field by field until the current
    // field does
    // (the search is for a few fields only: a capture starts on a second field, or has a skip, and the next first field is near;
    // a metadata that does not have one there is broken, and is said once, not for each of the frames)
    const qint32 maxSearch = 8;
    qint32 searched = 0;
    while (firstFieldNumber >= 1 && firstFieldNumber <= getNumberOfFields() && !getField(firstFieldNumber).isFirstField) {
        firstFieldNumber++;
        secondFieldNumber++;

        // Give up if we reach the end of the available fields, or search for too long
        if (firstFieldNumber > getNumberOfFields() || secondFieldNumber > getNumberOfFields() || ++searched >= maxSearch) {
            if (!isFirstFieldSearchReported) {
                qCritical() << "Attempting to get field number failed - no isFirstField in the metadata near field" << (frameNumber * 2 - 1) << "(said once)";
                isFirstFieldSearchReported = true;
            }
            firstFieldNumber = -1;
            secondFieldNumber = -1;
            break;
        }
    }

    // Range check the first field number
    if (firstFieldNumber > getNumberOfFields()) {
        qCritical() << "LdDecodeMetaData::getFieldNumber(): First field number exceed the available number of fields!";
        firstFieldNumber = -1;
        secondFieldNumber = -1;
    }

    // Range check the second field number
    if (secondFieldNumber > getNumberOfFields()) {
        qCritical() << "LdDecodeMetaData::getFieldNumber(): Second field number exceed the available number of fields!";
        firstFieldNumber = -1;
        secondFieldNumber = -1;
    }

    // Test for a buggy TBC file...
    if (secondFieldNumber >= 1 && getField(secondFieldNumber).isFirstField) {
        qCritical() << "LdDecodeMetaData::getFieldNumber(): Both of the determined fields have isFirstField set - the TBC source video is probably broken...";
    }

    if (field == 1) return firstFieldNumber; else return secondFieldNumber;
}

// Method to get the first field number based on the frame number
qint32 LdDecodeMetaData::getFirstFieldNumber(qint32 frameNumber)
{
    return getFieldNumber(frameNumber, 1);
}

// Method to get the second field number based on the frame number
qint32 LdDecodeMetaData::getSecondFieldNumber(qint32 frameNumber)
{
    return getFieldNumber(frameNumber, 2);
}

// Method to set the isFirstFieldFirst flag
void LdDecodeMetaData::setIsFirstFieldFirst(bool flag)
{
    isFirstFieldFirst = flag;
}

// Method to get the isFirstFieldFirst flag
bool LdDecodeMetaData::getIsFirstFieldFirst()
{
    return isFirstFieldFirst;
}

// Method to convert a CLV time code into an equivalent frame number (to make
// processing the timecodes easier)
qint32 LdDecodeMetaData::convertClvTimecodeToFrameNumber(LdDecodeMetaData::ClvTimecode clvTimeCode)
{
    // Calculate the frame number
    qint32 frameNumber = 0;
    VideoParameters videoParameters = getVideoParameters();

    // Check for invalid CLV timecode
    if (clvTimeCode.hours == -1 || clvTimeCode.minutes == -1 || clvTimeCode.seconds == -1 || clvTimeCode.pictureNumber == -1) {
        return -1;
    }

    const qint32 framesPerSecond = timecodeFramesPerSecond(videoParameters.system);
    frameNumber += clvTimeCode.hours * 3600 * framesPerSecond;
    frameNumber += clvTimeCode.minutes * 60 * framesPerSecond;
    frameNumber += clvTimeCode.seconds * framesPerSecond;
    frameNumber += clvTimeCode.pictureNumber;

    return frameNumber;
}

// Method to convert a frame number into an equivalent CLV timecode
LdDecodeMetaData::ClvTimecode LdDecodeMetaData::convertFrameNumberToClvTimecode(qint32 frameNumber)
{
    ClvTimecode clvTimecode;

    clvTimecode.hours = 0;
    clvTimecode.minutes = 0;
    clvTimecode.seconds = 0;
    clvTimecode.pictureNumber = 0;

    const qint32 framesPerSecond = timecodeFramesPerSecond(getVideoParameters().system);
    clvTimecode.hours = frameNumber / (3600 * framesPerSecond);
    frameNumber -= clvTimecode.hours * (3600 * framesPerSecond);

    clvTimecode.minutes = frameNumber / (60 * framesPerSecond);
    frameNumber -= clvTimecode.minutes * (60 * framesPerSecond);

    clvTimecode.seconds = frameNumber / framesPerSecond;
    frameNumber -= clvTimecode.seconds * framesPerSecond;

    clvTimecode.pictureNumber = frameNumber;

    return clvTimecode;
}

// Method to return a description string for the current video format
QString LdDecodeMetaData::getVideoSystemDescription() const
{
    return getSystemDefaults(videoParameters).name;
}

// Private method to generate a map of the PCM audio data (used by the sourceAudio library)
// Note: That the map unit is "stereo sample pairs"; so each unit represents 2 16-bit samples
// for a total of 4 bytes per unit.
void LdDecodeMetaData::generatePcmAudioMap()
{
    pcmAudioFieldStartSampleMap.clear();
    pcmAudioFieldLengthMap.clear();

    tbcDebugStream() << "LdDecodeMetaData::generatePcmAudioMap(): Generating PCM audio map...";

    // Get the number of fields and resize the maps
    qint32 numberOfFields = getVideoParameters().numberOfSequentialFields;
    pcmAudioFieldStartSampleMap.resize(numberOfFields + 1);
    pcmAudioFieldLengthMap.resize(numberOfFields + 1);

    for (qint32 fieldNo = 0; fieldNo < numberOfFields; fieldNo++) {
        // Each audio sample is 16 bit - and there are 2 samples per stereo pair
        pcmAudioFieldLengthMap[fieldNo] = static_cast<qint32>(getField(fieldNo+1).audioSamples);

        if (fieldNo == 0) {
            // First field starts at 0 units
            pcmAudioFieldStartSampleMap[fieldNo] = 0;
        } else {
            // Every following field's start position is the start+length of the previous
            pcmAudioFieldStartSampleMap[fieldNo] = pcmAudioFieldStartSampleMap[fieldNo - 1] + pcmAudioFieldLengthMap[fieldNo - 1];
        }
    }
}

// Method to get the start sample location of the specified sequential field number
qint32 LdDecodeMetaData::getFieldPcmAudioStart(qint32 sequentialFieldNumber)
{
    if (pcmAudioFieldStartSampleMap.size() < sequentialFieldNumber) return -1;
    // Field numbers are 1 indexed, but our map is 0 indexed
    return pcmAudioFieldStartSampleMap[sequentialFieldNumber - 1];
}

// Method to get the sample length of the specified sequential field number
qint32 LdDecodeMetaData::getFieldPcmAudioLength(qint32 sequentialFieldNumber)
{
    if (pcmAudioFieldLengthMap.size() < sequentialFieldNumber) return -1;
    // Field numbers are 1 indexed, but our map is 0 indexed
    return pcmAudioFieldLengthMap[sequentialFieldNumber - 1];
}
