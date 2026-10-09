/******************************************************************************
 * main.cpp
 * ld-sqlite-to-json - SQLite to JSON metadata converter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QFileInfo>

#include "tbc/logging.h"
#include "lddecodemetadata.h"

// The reverse of ld-json-converter: reads SQLite (.tbc.db) metadata, as written
// by current ld-decode, and writes the same metadata as JSON (.tbc.json). The
// processing tools never convert between formats; the two converters do.
int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-sqlite-to-json");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-sqlite-to-json - SQLite to JSON metadata converter for ld-decode\n"
                "\n"
                "Converts <name>.tbc.db metadata to <name>.tbc.json.\n"
                "Use ld-json-converter for the opposite direction.\n"
                "\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Option to specify the SQLite input file
    QCommandLineOption inputSqliteOption(QStringList() << "input-sqlite",
                                         QCoreApplication::translate("main", "Specify the input SQLite file"),
                                         QCoreApplication::translate("main", "filename"));
    parser.addOption(inputSqliteOption);

    // Option to specify the JSON output file
    QCommandLineOption outputJsonOption(QStringList() << "output-json",
                                        QCoreApplication::translate("main", "Specify the output JSON file (default same as input but with .json extension)"),
                                        QCoreApplication::translate("main", "filename"));
    parser.addOption(outputJsonOption);

    // Positional argument, as an alternative to --input-sqlite
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Input SQLite file (if --input-sqlite is not given)"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // Get the input SQLite filename
    QString inputSqliteFilename;
    const QStringList positionalArguments = parser.positionalArguments();
    if (parser.isSet(inputSqliteOption) && positionalArguments.isEmpty()) {
        inputSqliteFilename = parser.value(inputSqliteOption);
    } else if (!parser.isSet(inputSqliteOption) && positionalArguments.count() == 1) {
        inputSqliteFilename = positionalArguments.at(0);
    } else {
        qCritical("You must specify one input SQLite file, using --input-sqlite or as a positional argument");
        return -1;
    }

    if (!QFileInfo::exists(inputSqliteFilename)) {
        qCritical().noquote() << "Input file" << inputSqliteFilename << "does not exist";
        return -1;
    }
    if (LdDecodeMetaData::detectFormat(inputSqliteFilename) != LdDecodeMetaData::MetadataFormat::Sqlite) {
        qCritical().noquote() << inputSqliteFilename << "is not SQLite metadata (it is probably JSON already)";
        return -1;
    }

    // Work out the output JSON filename: capture.tbc.db -> capture.tbc.json
    QString outputJsonFilename;
    if (parser.isSet(outputJsonOption)) {
        outputJsonFilename = parser.value(outputJsonOption);
    } else if (inputSqliteFilename.endsWith(".db", Qt::CaseInsensitive)) {
        outputJsonFilename = inputSqliteFilename.left(inputSqliteFilename.length() - 3) + ".json";
    } else {
        outputJsonFilename = inputSqliteFilename + ".json";
    }

    // The library writes SQLite for a .db name, whatever is asked for
    if (outputJsonFilename.endsWith(".db", Qt::CaseInsensitive)) {
        qCritical().noquote() << "Output file" << outputJsonFilename << "has a .db extension - give a .json file name";
        return -1;
    }

    // An existing .tbc.json may hold metadata not in the SQLite file: never overwrite one
    if (QFileInfo::exists(outputJsonFilename)) {
        qCritical().noquote() << "Output file" << outputJsonFilename
                              << "already exists - will not overwrite it (remove it first, or use --output-json)";
        return -1;
    }

    // Read the SQLite metadata and write it out as JSON
    LdDecodeMetaData metaData;
    qInfo().noquote() << "Reading SQLite metadata from" << inputSqliteFilename;
    if (!metaData.read(inputSqliteFilename)) {
        qCritical() << "Unable to read the SQLite metadata";
        return 1;
    }

    qInfo().noquote() << "Writing JSON metadata to" << outputJsonFilename;
    metaData.setFormat(LdDecodeMetaData::MetadataFormat::Json);
    if (!metaData.write(outputJsonFilename)) {
        qCritical() << "Unable to write the JSON metadata";
        return 1;
    }

    qInfo() << "Converted" << metaData.getNumberOfFields() << "fields";
    return 0;
}
