/******************************************************************************
 * metadataoptions.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "metadataoptions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>

using MetadataFormat = LdDecodeMetaData::MetadataFormat;

static const char *const conversionHint =
    "the tools do not convert between metadata formats - use ld-json-converter (JSON to SQLite) "
    "or ld-sqlite-to-json (SQLite to JSON)";

MetadataOptions::MetadataOptions(int _options, const QString &inputDescription, const QString &outputDescription)
    : options(_options),
      metaOption(QStringList() << "meta" << "metadata-format",
                 (_options & CreatesMetadata)
                     ? QCoreApplication::translate("main", "Metadata format to create: db (SQLite <output>.db, the "
                                                           "default) or json (<output>.json)")
                     : QCoreApplication::translate("main", "Metadata format to use: db (SQLite <input>.db) or json "
                                                           "(<input>.json). Default: db if present, otherwise json"),
                 QCoreApplication::translate("main", "db|json")),
      inputOption(QStringList() << "input-metadata",
                  inputDescription.isEmpty()
                      ? QCoreApplication::translate("main", "Specify the input metadata file (default input.db or input.json, see --meta)")
                      : inputDescription,
                  QCoreApplication::translate("main", "filename")),
      inputJsonOption(QStringList() << "input-json",
                      QCoreApplication::translate("main", "Original name of --input-metadata"),
                      QCoreApplication::translate("main", "filename")),
      outputOption(QStringList() << "output-metadata",
                   outputDescription.isEmpty()
                       ? QCoreApplication::translate("main", "Specify the output metadata file (default same as input)")
                       : outputDescription,
                   QCoreApplication::translate("main", "filename")),
      outputJsonOption(QStringList() << "output-json",
                       QCoreApplication::translate("main", "Original name of --output-metadata"),
                       QCoreApplication::translate("main", "filename"))
{
    // The original option names stay accepted for existing scripts, but only
    // one way of doing it is shown in --help
    inputJsonOption.setFlags(QCommandLineOption::HiddenFromHelp);
    outputJsonOption.setFlags(QCommandLineOption::HiddenFromHelp);
}

void MetadataOptions::addTo(QCommandLineParser &parser)
{
    parser.addOption(metaOption);
    if (options & InputFile) {
        parser.addOption(inputOption);
        parser.addOption(inputJsonOption);
    }
    if (options & OutputFile) {
        parser.addOption(outputOption);
        parser.addOption(outputJsonOption);
    }
}

// Read one file option given under either its current or its original name
static bool readFileOption(const QCommandLineParser &parser, const QCommandLineOption &option,
                           const QCommandLineOption &aliasOption, QString &fileName)
{
    const bool isSet = parser.isSet(option);
    const bool isAliasSet = parser.isSet(aliasOption);

    if (isSet && isAliasSet) {
        qCritical().nospace().noquote() << "--" << option.names().first() << " and --" << aliasOption.names().first()
                                        << " are the same option - give only one";
        return false;
    }

    if (isSet) fileName = parser.value(option);
    else if (isAliasSet) fileName = parser.value(aliasOption);
    return true;
}

bool MetadataOptions::process(const QCommandLineParser &parser)
{
    if (parser.isSet(metaOption)) {
        const QString value = parser.value(metaOption).toLower();
        if (value == "db" || value == "sqlite") requestedFormat = MetadataFormat::Sqlite;
        else if (value == "json") requestedFormat = MetadataFormat::Json;
        else {
            qCritical().noquote() << "Unknown --meta value" << parser.value(metaOption) << "- use db or json";
            return false;
        }
        formatSet = true;
    }

    if ((options & InputFile) && !readFileOption(parser, inputOption, inputJsonOption, inputFileName)) return false;
    if ((options & OutputFile) && !readFileOption(parser, outputOption, outputJsonOption, outputFileName)) return false;

    return true;
}

bool MetadataOptions::selectInput(const QString &tbcFileName, bool useInputFile, QString &metadataFileName) const
{
    // An explicitly named file is used as given; --meta, if also given, must agree with it
    if (useInputFile && isInputFileSet()) {
        if (!QFileInfo::exists(inputFileName)) {
            qCritical().noquote() << "Input metadata file" << inputFileName << "does not exist";
            return false;
        }
        const MetadataFormat fileFormat = LdDecodeMetaData::detectFormat(inputFileName);
        if (formatSet && fileFormat != requestedFormat) {
            qCritical().noquote() << "--meta" << formatName(requestedFormat) << "was given but" << inputFileName
                                  << "is" << formatName(fileFormat) << "metadata";
            return false;
        }
        metadataFileName = inputFileName;
        return true;
    }

    const QString sqliteFileName = LdDecodeMetaData::metadataFileName(tbcFileName, MetadataFormat::Sqlite);
    const QString jsonFileName = LdDecodeMetaData::metadataFileName(tbcFileName, MetadataFormat::Json);
    const bool hasSqlite = QFileInfo::exists(sqliteFileName);
    const bool hasJson = QFileInfo::exists(jsonFileName);

    if (formatSet) {
        metadataFileName = LdDecodeMetaData::metadataFileName(tbcFileName, requestedFormat);
        if (!QFileInfo::exists(metadataFileName)) {
            qCritical().noquote() << formatName(requestedFormat) << "metadata requested (--meta"
                                  << (requestedFormat == MetadataFormat::Json ? "json)" : "db)")
                                  << "but" << metadataFileName << "does not exist";
            return false;
        }
        if (hasSqlite && hasJson) {
            qWarning().noquote() << "2 metadata sources detected (" + sqliteFileName + ", " + jsonFileName + "), using"
                                 << metadataFileName << "as requested by --meta";
        }
        return true;
    }

    if (hasSqlite && hasJson) {
        qWarning().noquote() << "2 metadata sources detected (" + sqliteFileName + ", " + jsonFileName + "), using"
                             << sqliteFileName << "(use --meta json to use the JSON file)";
        metadataFileName = sqliteFileName;
    } else if (hasJson) {
        metadataFileName = jsonFileName;
    } else {
        // Neither exists: name the default so that the read reports it
        metadataFileName = sqliteFileName;
    }
    return true;
}

bool MetadataOptions::selectOutput(const QString &defaultFileName, MetadataFormat requiredFormat,
                                   QString &metadataFileName) const
{
    metadataFileName = isOutputFileSet() ? outputFileName : defaultFileName;

    // The output's format, where it is already decided: by the contents of an
    // existing file, or by a .db/.json extension. Any other new file is simply
    // written in the input's format.
    bool isKnown = true;
    MetadataFormat outputFormat = requiredFormat;
    if (QFileInfo::exists(metadataFileName)) {
        outputFormat = LdDecodeMetaData::detectFormat(metadataFileName);
    } else if (metadataFileName.endsWith(".json", Qt::CaseInsensitive)) {
        outputFormat = MetadataFormat::Json;
    } else if (metadataFileName.endsWith(".db", Qt::CaseInsensitive)) {
        outputFormat = MetadataFormat::Sqlite;
    } else {
        isKnown = false;
    }

    if (isKnown && outputFormat != requiredFormat) {
        qCritical().noquote() << "Output metadata" << metadataFileName << "would be" << formatName(outputFormat)
                              << "but the input is" << formatName(requiredFormat) << "-" << conversionHint;
        return false;
    }

    return true;
}

void MetadataOptions::warnIfOtherFormatStale(const QString &tbcFileName, const QString &updatedFileName)
{
    const MetadataFormat updatedFormat = LdDecodeMetaData::detectFormat(updatedFileName);
    const MetadataFormat otherFormat = (updatedFormat == MetadataFormat::Json) ? MetadataFormat::Sqlite
                                                                                : MetadataFormat::Json;
    const QString otherFileName = LdDecodeMetaData::metadataFileName(tbcFileName, otherFormat);

    if (QFileInfo::exists(otherFileName) && QFileInfo(otherFileName) != QFileInfo(updatedFileName)) {
        qWarning().noquote() << otherFileName << "was not updated and is now out of date";
    }
}

QString MetadataOptions::formatName(MetadataFormat format)
{
    return format == MetadataFormat::Json ? QStringLiteral("JSON") : QStringLiteral("SQLite");
}
