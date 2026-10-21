/******************************************************************************
 * jsonconverter.cpp
 * ld-json-converter - JSON converter tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "jsonconverter.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>

JsonConverter::JsonConverter(const QString &inputJsonFilename, const QString &outputSqliteFilename)
    : m_inputJsonFilename(inputJsonFilename), m_outputSqliteFilename(outputSqliteFilename)
{

}

JsonConverter::~JsonConverter()
{
}

bool JsonConverter::process()
{
    qInfo() << "Processing JSON file:" << m_inputJsonFilename;
    
    // Check if input file exists
    QFileInfo inputFile(m_inputJsonFilename);
    if (!inputFile.exists()) {
        qCritical() << "Input JSON file does not exist:" << m_inputJsonFilename;
        return false;
    }
    
    // Load the JSON metadata using local TBC library
    LdDecodeMetaData metaData;
    if (!metaData.read(m_inputJsonFilename)) {
        qCritical() << "Failed to read JSON file:" << m_inputJsonFilename;
        return false;
    }
    
    qInfo() << "Successfully loaded JSON metadata";
    
    // Report on the contents
    reportJsonContents(metaData);
    
    qInfo() << "JSON analysis complete. Output SQLite file will be:" << m_outputSqliteFilename;
    
    // A file that is there is replaced, not updated
    if (QFileInfo::exists(m_outputSqliteFilename) && !QDir().remove(m_outputSqliteFilename)) {
        qCritical() << "Failed to remove existing database file:" << m_outputSqliteFilename;
        return false;
    }

    // Write it as SQLite, as every tool of the suite writes it
    metaData.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
    if (!metaData.write(m_outputSqliteFilename)) {
        qCritical() << "Failed to write the SQLite database";
        return false;
    }

    qInfo() << "SQLite database created successfully:" << m_outputSqliteFilename;
    
    return true;
}

void JsonConverter::reportJsonContents(LdDecodeMetaData &metaData)
{
    qInfo() << "=== JSON Content Analysis ===";
    
    // Basic information
    qInfo() << "Video System:" << metaData.getVideoSystemDescription();
    qInfo() << "Number of Fields:" << metaData.getNumberOfFields();
    qInfo() << "Number of Frames:" << metaData.getNumberOfFrames();
    qInfo() << "First Field First:" << (metaData.getIsFirstFieldFirst() ? "Yes" : "No");
    
    // Comprehensive Video parameters matching README schema
    const LdDecodeMetaData::VideoParameters &videoParams = metaData.getVideoParameters();
    qInfo() << "Video Parameters:";
    qInfo() << "  System:" << (videoParams.system == PAL ? "PAL" : 
                                (videoParams.system == NTSC ? "NTSC" : "PAL_M"));
    qInfo() << "  Field Width:" << videoParams.fieldWidth << "pixels";
    qInfo() << "  Field Height:" << videoParams.fieldHeight << "lines";
    qInfo() << "  Video Sample Rate:" << QString::number(videoParams.sampleRate, 'f', 0).toLongLong() << "Hz";
    qInfo() << "  Active Video Start:" << videoParams.activeVideoStart;
    qInfo() << "  Active Video End:" << videoParams.activeVideoEnd;
    qInfo() << "  Colour Burst Start:" << videoParams.colourBurstStart;
    qInfo() << "  Colour Burst End:" << videoParams.colourBurstEnd;
    qInfo() << "  White 16b IRE:" << videoParams.white16bIre;
    qInfo() << "  Black 16b IRE:" << videoParams.black16bIre;
    qInfo() << "  Is Mapped:" << (videoParams.isMapped ? "Yes" : "No");
    qInfo() << "  Is Subcarrier Locked:" << (videoParams.isSubcarrierLocked ? "Yes" : "No");
    qInfo() << "  Is Widescreen:" << (videoParams.isWidescreen ? "Yes" : "No");
    if (!videoParams.gitBranch.isEmpty()) {
        qInfo() << "  Git Branch:" << videoParams.gitBranch;
    }
    if (!videoParams.gitCommit.isEmpty()) {
        qInfo() << "  Git Commit:" << videoParams.gitCommit;
    }
    if (!videoParams.tapeFormat.isEmpty()) {
        qInfo() << "  Tape Format:" << videoParams.tapeFormat;
    }
    
    // PCM Audio parameters (if present)
    const LdDecodeMetaData::PcmAudioParameters &audioParams = metaData.getPcmAudioParameters();
    if (audioParams.sampleRate > 0) {
        qInfo() << "PCM Audio Parameters:";
        qInfo() << "  Sample Rate:" << audioParams.sampleRate << "Hz";
        qInfo() << "  Bits per Sample:" << audioParams.bits;
        qInfo() << "  Is Signed:" << (audioParams.isSigned ? "Yes" : "No");
        qInfo() << "  Is Little Endian:" << (audioParams.isLittleEndian ? "Yes" : "No");
    } else {
        qInfo() << "PCM Audio Parameters: Not present";
    }
    
    // Count different types of data objects as per schema
    qint32 fieldsWithVbi = 0;
    qint32 fieldsWithVitc = 0;
    qint32 fieldsWithClosedCaptions = 0;
    qint32 fieldsWithVitsMetrics = 0;
    qint32 fieldsWithNtsc = 0;
    qint32 totalDropouts = 0;
    qint32 fieldsWithAudio = 0;
    qint32 paddedFields = 0;
    
    // Analyze each field for detailed statistics
    for (qint32 fieldNum = 1; fieldNum <= metaData.getNumberOfFields(); fieldNum++) {
        const LdDecodeMetaData::Field &field = metaData.getField(fieldNum);
        
        // Count padded fields
        if (field.pad) paddedFields++;
        
        // Count fields with audio samples
        if (field.audioSamples > 0) fieldsWithAudio++;
        
        // Count VBI data
        const LdDecodeMetaData::Vbi &vbi = metaData.getFieldVbi(fieldNum);
        if (vbi.inUse) fieldsWithVbi++;
        
        // Count VITC data
        const LdDecodeMetaData::Vitc &vitc = metaData.getFieldVitc(fieldNum);
        if (vitc.inUse) fieldsWithVitc++;
        
        // Count Closed Caption data
        const LdDecodeMetaData::ClosedCaption &cc = metaData.getFieldClosedCaption(fieldNum);
        if (cc.inUse) fieldsWithClosedCaptions++;
        
        // Count VITS Metrics
        const LdDecodeMetaData::VitsMetrics &vits = metaData.getFieldVitsMetrics(fieldNum);
        if (vits.inUse) fieldsWithVitsMetrics++;
        
        // Count NTSC data
        const LdDecodeMetaData::Ntsc &ntsc = metaData.getFieldNtsc(fieldNum);
        if (ntsc.inUse) fieldsWithNtsc++;
        
        // Count dropouts
        const DropOuts &dropouts = metaData.getFieldDropOuts(fieldNum);
        totalDropouts += dropouts.size();
    }
    
    qInfo() << "Field Data Objects Summary:";
    qInfo() << "  Fields with VBI data:" << fieldsWithVbi << "(" << 
               QString::number(100.0 * fieldsWithVbi / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with VITC data:" << fieldsWithVitc << "(" << 
               QString::number(100.0 * fieldsWithVitc / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with Closed Caption data:" << fieldsWithClosedCaptions << "(" << 
               QString::number(100.0 * fieldsWithClosedCaptions / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with VITS Metrics:" << fieldsWithVitsMetrics << "(" << 
               QString::number(100.0 * fieldsWithVitsMetrics / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    if (videoParams.system == NTSC) {
        qInfo() << "  Fields with NTSC data:" << fieldsWithNtsc << "(" << 
                   QString::number(100.0 * fieldsWithNtsc / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    }
    qInfo() << "  Fields with Audio samples:" << fieldsWithAudio << "(" << 
               QString::number(100.0 * fieldsWithAudio / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Padded fields (no valid video):" << paddedFields << "(" << 
               QString::number(100.0 * paddedFields / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Total Dropout objects:" << totalDropouts;
    
    // Summary for conversion planning
    qInfo() << "SQLite Conversion Planning:";
    qInfo() << "  Main capture record: 1 row";
    qInfo() << "  PCM audio parameters:" << (audioParams.sampleRate > 0 ? "1 row" : "0 rows (no audio)");
    qInfo() << "  Field records:" << metaData.getNumberOfFields() << "rows";
    qInfo() << "  VBI rows:" << fieldsWithVbi;
    qInfo() << "  VITC rows:" << fieldsWithVitc;
    qInfo() << "  Closed Caption rows:" << fieldsWithClosedCaptions;
    qInfo() << "  VITS Metrics rows:" << fieldsWithVitsMetrics;
    qInfo() << "  Dropout rows:" << totalDropouts;
    
    qInfo() << "=== End Analysis ===";
}
