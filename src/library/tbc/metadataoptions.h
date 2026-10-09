/******************************************************************************
 * metadataoptions.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef METADATAOPTIONS_H
#define METADATAOPTIONS_H

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

#include "lddecodemetadata.h"

// Command-line handling of the TBC metadata file shared by the command-line
// tools: the --meta format selection, the --input-metadata/--output-metadata
// file options (with the original --input-json/--output-json names kept as
// hidden aliases), selection between <tbc>.db and <tbc>.json, and the checks
// that keep a tool's output in the same format as its input. Processing tools
// never convert between formats; ld-json-converter and ld-sqlite-to-json do.
class MetadataOptions
{
public:
    enum Option {
        FormatOnly = 0,        // --meta only
        InputFile = 1 << 0,    // --input-metadata
        OutputFile = 1 << 1,   // --output-metadata
    };

    MetadataOptions(int options,
                    const QString &inputDescription = QString(),
                    const QString &outputDescription = QString());

    // Register the options with the parser (before QCommandLineParser::process)
    void addTo(QCommandLineParser &parser);
    // Read and validate the options (after QCommandLineParser::process).
    // Returns false, having logged why, if they are invalid.
    bool process(const QCommandLineParser &parser);

    // --meta was given, and the format it names
    bool isFormatSet() const { return formatSet; }
    LdDecodeMetaData::MetadataFormat format() const { return requestedFormat; }

    bool isInputFileSet() const { return !inputFileName.isEmpty(); }
    bool isOutputFileSet() const { return !outputFileName.isEmpty(); }

    // Choose the metadata file to read for a TBC file: the --input-metadata
    // file if useInputFile is set and it was given, otherwise <tbc>.db or
    // <tbc>.json as selected by --meta, preferring .db when both exist.
    // Returns false, having logged why, if the selection is impossible.
    bool selectInput(const QString &tbcFileName, bool useInputFile, QString &metadataFileName) const;

    // Choose the metadata file to write: the --output-metadata file if given,
    // otherwise defaultFileName. It must be in requiredFormat (the input's):
    // returns false, having logged why, if it would be a conversion.
    bool selectOutput(const QString &defaultFileName, LdDecodeMetaData::MetadataFormat requiredFormat,
                      QString &metadataFileName) const;

    // After updating a TBC file's metadata in place, warn if the other
    // format's file also exists, since it now holds out-of-date metadata.
    static void warnIfOtherFormatStale(const QString &tbcFileName, const QString &updatedFileName);

    static QString formatName(LdDecodeMetaData::MetadataFormat format);

private:
    int options;
    QCommandLineOption metaOption;
    QCommandLineOption inputOption;
    QCommandLineOption inputJsonOption;
    QCommandLineOption outputOption;
    QCommandLineOption outputJsonOption;

    bool formatSet = false;
    LdDecodeMetaData::MetadataFormat requestedFormat = LdDecodeMetaData::MetadataFormat::Sqlite;
    QString inputFileName;
    QString outputFileName;
};

#endif // METADATAOPTIONS_H
