/******************************************************************************
 * sqliteio.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SQLITEIO_H
#define SQLITEIO_H

#include <QString>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <stdexcept>

namespace SqliteValue
{
    int toIntOrDefault(const QSqlQuery &query, const char *column, int defaultValue = -1);
    qint64 toLongLongOrDefault(const QSqlQuery &query, const char *column, qint64 defaultValue = -1);
    double toDoubleOrDefault(const QSqlQuery &query, const char *column, double defaultValue = -1.0);
    bool toBoolOrDefault(const QSqlQuery &query, const char *column, bool defaultValue = false);
}

class SqliteReader
{
public:
    SqliteReader(const QString &fileName);
    ~SqliteReader();
    
    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when parsing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Read capture-level metadata
    bool readCaptureMetadata(int &captureId, QString &system, QString &decoder,
                           QString &gitBranch, QString &gitCommit,
                           double &videoSampleRate, int &activeVideoStart, int &activeVideoEnd,
                           int &fieldWidth, int &fieldHeight, int &numberOfSequentialFields,
                           int &colourBurstStart, int &colourBurstEnd,
                           bool &isMapped, bool &isSubcarrierLocked, bool &isWidescreen,
                           int &white16bIre, int &black16bIre, int &blanking16bIre, QString &captureNotes);

    // Read PCM audio parameters
    bool readPcmAudioParameters(int captureId, int &bits, bool &isSigned,
                              bool &isLittleEndian, double &sampleRate);
    // The map of the capture (a FrameMap as JSON), when the file has one: it is in the table disc_map, which only a file with a map has
    bool readDiscMap(int captureId, QByteArray &json);
    // A table of the file that holds one JSON text for the capture (disc_map and its column map, decoding and its column parameters): not there, or empty, is false
    bool readJsonTable(int captureId, const QString &table, const QString &column, QByteArray &json);

    // Read field metadata
    bool readFields(int captureId, QSqlQuery &fieldsQuery);

    // Read field-specific data (individual queries - slower)
    bool readFieldVitsMetrics(int captureId, int fieldId, double &wSnr, double &bPsnr);
    bool readFieldVbi(int captureId, int fieldId, int &vbi0, int &vbi1, int &vbi2);
    bool readFieldVitc(int captureId, int fieldId, int vitcData[8]);
    bool readFieldClosedCaption(int captureId, int fieldId, int &data0, int &data1);
    bool readFieldDropouts(int captureId, int fieldId, QSqlQuery &dropoutsQuery);

    // Optimized bulk read methods for all fields (much faster)
    bool readAllFieldVitsMetrics(int captureId, QSqlQuery &vitsQuery);
    bool readAllFieldVbi(int captureId, QSqlQuery &vbiQuery);
    bool readAllFieldVitc(int captureId, QSqlQuery &vitcQuery);
    bool readAllFieldClosedCaptions(int captureId, QSqlQuery &ccQuery);
    bool readAllFieldDropouts(int captureId, QSqlQuery &dropoutsQuery);

private:
    QSqlDatabase db;
    QString connectionName;
};

class SqliteWriter
{
public:
    SqliteWriter(const QString &fileName);
    ~SqliteWriter();
    
    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when writing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Initialize database with schema
    bool createSchema();

    // Write capture-level metadata
    int writeCaptureMetadata(const QString &system, const QString &decoder,
                           const QString &gitBranch, const QString &gitCommit,
                           double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                           int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                           int colourBurstStart, int colourBurstEnd,
                           bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                           int white16bIre, int black16bIre, int blanking16bIre, const QString &captureNotes);

    // Update existing capture metadata  
    bool updateCaptureMetadata(int captureId, const QString &system, const QString &decoder,
                             const QString &gitBranch, const QString &gitCommit,
                             double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                             int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                             int colourBurstStart, int colourBurstEnd,
                             bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                             int white16bIre, int black16bIre, int blanking16bIre, const QString &captureNotes);

    // Write the map of the capture (the table disc_map is made when it is first needed); empty json takes the map away
    bool writeDiscMap(int captureId, const QByteArray &json);
    // The same, for any such table; empty json takes the row away
    bool writeJsonTable(int captureId, const QString &table, const QString &column, const QByteArray &json);

    // Write PCM audio parameters
    bool writePcmAudioParameters(int captureId, int bits, bool isSigned,
                               bool isLittleEndian, double sampleRate);

    // Write field metadata
    bool writeField(int captureId, int fieldId, int audioSamples, int decodeFaults,
                   double diskLoc, int efmTValues, int fieldPhaseId, int fileLoc,
                   bool isFirstField, double medianBurstIre, bool pad, int syncConf,
                   bool ntscIsFmCodeDataValid, int ntscFmCodeData, bool ntscFieldFlag,
                   bool ntscIsVideoIdDataValid, int ntscVideoIdData, bool ntscWhiteFlag);

    // Write field-specific data
    bool writeFieldVitsMetrics(int captureId, int fieldId, double wSnr, double bPsnr);
    bool writeFieldVbi(int captureId, int fieldId, int vbi0, int vbi1, int vbi2);
    bool writeFieldVitc(int captureId, int fieldId, const int vitcData[8]);
    bool writeFieldClosedCaption(int captureId, int fieldId, int data0, int data1);
    bool writeFieldDropouts(int captureId, int fieldId, int startx, int endx, int fieldLine);

    // Take away the rows of one of the tables of the fields (field_record, vits_metrics, vbi, vitc, closed_caption, drop_outs) for a capture, before
    // its rows are written again: a row that the new metadata no longer have does not stay
    bool deleteFieldRows(int captureId, const QString &table);

    // Transaction support
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    QSqlDatabase db;
    QString connectionName;

    // The statements, prepared once for the writer (a row is written by binding and executing, not by preparing again)
    QSqlQuery fieldQuery, vitsQuery, vbiQuery, vitcQuery, closedCaptionQuery, dropoutsQuery;
    QSqlQuery &prepared(QSqlQuery &query, const char *sql);
};

#endif // SQLITEIO_H