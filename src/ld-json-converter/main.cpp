/******************************************************************************
 * main.cpp
 * ld-json-converter - JSON converter tool for ld-decode
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

#include "tbc/logging.h"
#include "jsonconverter.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-json-converter");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-json-converter - JSON converter tool for ld-decode\n"
                "\n"
                "(c)2025 Simon Inns\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Option to specify a different JSON input file (--input-metadata, as in the other tools; the original --input-json is accepted, and not shown)
    QCommandLineOption inputMetadataOption(QStringList() << "input-metadata",
                                           QCoreApplication::translate("main", "Specify the input JSON file"),
                                           QCoreApplication::translate("main", "filename"));
    parser.addOption(inputMetadataOption);
    QCommandLineOption inputJsonOption(QStringList() << "input-json",
                                       QCoreApplication::translate("main", "Original name of --input-metadata"),
                                       QCoreApplication::translate("main", "filename"));
    inputJsonOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(inputJsonOption);

    // Option to specify a different SQLite DB output file (--output-metadata; the original --output-sqlite is accepted, and not shown)
    QCommandLineOption outputMetadataOption(QStringList() << "output-metadata",
                                            QCoreApplication::translate("main", "Specify the output SQLite file (default same as input but with .db extension)"),
                                            QCoreApplication::translate("main", "filename"));
    parser.addOption(outputMetadataOption);
    QCommandLineOption outputJsonOption(QStringList() << "output-sqlite",
                                        QCoreApplication::translate("main", "Original name of --output-metadata"),
                                        QCoreApplication::translate("main", "filename"));
    outputJsonOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(outputJsonOption);

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    if ((parser.isSet(inputMetadataOption) && parser.isSet(inputJsonOption)) || (parser.isSet(outputMetadataOption) && parser.isSet(outputJsonOption))) {
        qCritical("--input-metadata is the same option as --input-json, and --output-metadata the same as --output-sqlite - give only one of each");
        return 2;
    }
    const QCommandLineOption &inputGiven = parser.isSet(inputMetadataOption) ? inputMetadataOption : inputJsonOption;
    const QCommandLineOption &outputGiven = parser.isSet(outputMetadataOption) ? outputMetadataOption : outputJsonOption;

    // Get the input JSON filename from the parser
    QString inputJsonFilename;
    if (parser.isSet(inputGiven)) {
        // Use the --input-metadata option if provided
        inputJsonFilename = parser.value(inputGiven);
    } else {
        // Fall back to positional arguments
        QStringList positionalArguments = parser.positionalArguments();
        if (positionalArguments.count() == 1) {
            inputJsonFilename = positionalArguments.at(0);
        } else {
            // Quit with error
            qCritical("You must specify an input JSON file using --input-metadata or as a positional argument");
            return 2;
        }
    }

    // Work out the output SQLite filename
    QString outputSqliteFilename;
    if (parser.isSet(outputGiven)) {
        outputSqliteFilename = parser.value(outputGiven);
    } else {
        // Remove .json extension if present, then add .db
        if (inputJsonFilename.endsWith(".json", Qt::CaseInsensitive)) {
            outputSqliteFilename = inputJsonFilename.left(inputJsonFilename.length() - 5) + ".db";
        } else {
            outputSqliteFilename = inputJsonFilename + ".db";
        }
    }

    // Perform the conversion processing
    qInfo() << "Beginning JSON to SQLite DB processing...";
    JsonConverter jsonConverter(inputJsonFilename, outputSqliteFilename);
    if (!jsonConverter.process()) return 1;

    // Quit with success
    return 0;
}
