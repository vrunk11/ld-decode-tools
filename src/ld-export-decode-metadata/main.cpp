/******************************************************************************
 * main.cpp
 * ld-export-decode-metadata - metadata export tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QThread>

#include "logging.h"
#include "metadataconverter.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-export-decode-metadata");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-export-decode-metadata - exports the metadata of a capture to a versioned JSON file for external tools\n"
                "\n"
                "(c)2025 Simon Inns\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Option to specify the SQLite input file (--input-metadata, as in the other tools; the original --input-sqlite is accepted, and not shown)
    QCommandLineOption inputMetadataOption(QStringList() << "input-metadata",
                                           QCoreApplication::translate("main", "Specify the input SQLite file"),
                                           QCoreApplication::translate("main", "filename"));
    parser.addOption(inputMetadataOption);
    QCommandLineOption inputSqliteOption(QStringList() << "input-sqlite",
                                         QCoreApplication::translate("main", "Original name of --input-metadata"),
                                         QCoreApplication::translate("main", "filename"));
    inputSqliteOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(inputSqliteOption);

    // Option to specify the JSON output file (--output-metadata; the original --output-json is accepted, and not shown)
    QCommandLineOption outputMetadataOption(QStringList() << "output-metadata",
                                            QCoreApplication::translate("main", "Specify the output JSON file (default same as input but with the extension .export.json)"),
                                            QCoreApplication::translate("main", "filename"));
    parser.addOption(outputMetadataOption);
    QCommandLineOption outputJsonOption(QStringList() << "output-json",
                                        QCoreApplication::translate("main", "Original name of --output-metadata"),
                                        QCoreApplication::translate("main", "filename"));
    outputJsonOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(outputJsonOption);

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    if ((parser.isSet(inputMetadataOption) && parser.isSet(inputSqliteOption)) || (parser.isSet(outputMetadataOption) && parser.isSet(outputJsonOption))) {
        qCritical("--input-metadata is the same option as --input-sqlite, and --output-metadata the same as --output-json - give only one of each");
        return 2;
    }
    const QCommandLineOption &inputGiven = parser.isSet(inputMetadataOption) ? inputMetadataOption : inputSqliteOption;
    const QCommandLineOption &outputGiven = parser.isSet(outputMetadataOption) ? outputMetadataOption : outputJsonOption;

    // Get the input SQLite filename from the parser
    QString inputSqliteFilename;
    if (parser.isSet(inputGiven)) {
        // Use the --input-metadata option if provided
        inputSqliteFilename = parser.value(inputGiven);
    } else {
        // Fall back to positional arguments
        QStringList positionalArguments = parser.positionalArguments();
        if (positionalArguments.count() == 1) {
            inputSqliteFilename = positionalArguments.at(0);
        } else {
            // Quit with error
            qCritical("You must specify an input SQLite file using --input-metadata or as a positional argument");
            return 2;
        }
    }

    // Work out the output SQLite filename
    QString outputJsonFilename;
    if (parser.isSet(outputGiven)) {
        outputJsonFilename = parser.value(outputGiven);
    } else {
        // Remove .json extension if present, then add .db
        if (inputSqliteFilename.endsWith(".db", Qt::CaseInsensitive)) {
            outputJsonFilename = inputSqliteFilename.left(inputSqliteFilename.length() - 3) + ".export.json";
        } else {
            outputJsonFilename = inputSqliteFilename + ".export.json";
        }
    }

    // Perform the conversion processing
    qInfo() << "Beginning SQLite DB to export JSON processing...";
    MetadataConverter metadataConverter(inputSqliteFilename, outputJsonFilename);
    if (!metadataConverter.process()) return 1;

    // Quit with success
    return 0;
}
