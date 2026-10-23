/************************************************************************

    main.cpp

    ld-export-metadata - Export ld-decode metadata into other formats
    Copyright (C) 2020-2023 Adam Sampson
    Copyright (C) 2021-2025 Simon Inns

    This file is part of ld-decode-tools.

    ld-export-metadata is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include <QCoreApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>

#include "audacity.h"
#include "csv.h"
#include "ffmetadata.h"
#include "closedcaptions.h"

#include "tbc/logging.h"
#include "commonoptions.h"
#include "lddecodemetadata.h"
#include "mappedmetadata.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-export-metadata");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-export-metadata - Export ld-decode metadata into other formats\n"
                "\n"
                "(c)2020-2023 Adam Sampson\n"
                "(c)2021-2025 Simon Inns\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // -- General options --

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // -- Output types --

    QCommandLineOption writeVitsCsvOption("vits-csv",
                                          QCoreApplication::translate("main", "Write VITS information as CSV"),
                                          QCoreApplication::translate("main", "file"));
    parser.addOption(writeVitsCsvOption);

    QCommandLineOption writeVbiCsvOption("vbi-csv",
                                          QCoreApplication::translate("main", "Write VBI information as CSV"),
                                          QCoreApplication::translate("main", "file"));
    parser.addOption(writeVbiCsvOption);

    QCommandLineOption writeAudacityLabelsOption("audacity-labels",
                                                 QCoreApplication::translate("main", "Write navigation information as Audacity labels"),
                                                 QCoreApplication::translate("main", "file"));
    parser.addOption(writeAudacityLabelsOption);

    QCommandLineOption writeFfmetadataOption("ffmetadata",
                                             QCoreApplication::translate("main", "Write navigation information as FFMETADATA1"),
                                             QCoreApplication::translate("main", "file"));
    parser.addOption(writeFfmetadataOption);

    QCommandLineOption writeClosedCaptionsOption("closed-captions",
                                             QCoreApplication::translate("main", "Write closed captions as Scenarist SCC V1.0 format"),
                                             QCoreApplication::translate("main", "file"));
    parser.addOption(writeClosedCaptionsOption);

    // A capture that keeps a map in its metadata is exported in the order of the map, as every tool that reads a capture reads it
    IgnoreMapOption ignoreMapOption;
    ignoreMapOption.addTo(parser);

    // -- Positional arguments --

    // Positional argument to specify input metadata file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input metadata file"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // Get the arguments from the parser
    QString inputFileName;
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 1) {
        inputFileName = positionalArguments.at(0);
    } else {
        qCritical("You must specify the input metadata file");
        return 2;
    }

    // Load the source video metadata
    LdDecodeMetaData metaData;
    if (!metaData.read(inputFileName)) {
        qInfo() << "Unable to read metadata file";
        return 1;
    }

    // The metadata of a capture that keeps a map are those of the capture seen through it (the frames of the disc, the missing ones padded)
    if (!ignoreMapOption.isSet(parser)) {
        // (the video of the capture is the file the metadata are named for, when it is there: the map says which capture it was made for)
        QString videoFileName = inputFileName;
        for (const char *suffix : { ".db", ".json" }) {
            if (videoFileName.endsWith(suffix, Qt::CaseInsensitive)) { videoFileName.chop(static_cast<qsizetype>(qstrlen(suffix))); break; }
        }
        QString mapError;
        const auto viewTable = MappedMetadata::applyEmbeddedMap(metaData, inputFileName, videoFileName, mapError);
        if (!mapError.isEmpty()) {
            qCritical().noquote() << "Unable to use the map in the metadata:" << mapError << "(--ignore-map reads the capture as it is)";
            return 1;
        }
        if (viewTable) qInfo() << "The metadata are read through the map they keep (--ignore-map reads them as they are)";
    }

    // Write the selected output files
    if (parser.isSet(writeVitsCsvOption)) {
        const QString &fileName = parser.value(writeVitsCsvOption);
        if (!writeVitsCsv(metaData, fileName)) {
            qCritical() << "Failed to write output file:" << fileName;
            return 1;
        }
    }
    if (parser.isSet(writeVbiCsvOption)) {
        const QString &fileName = parser.value(writeVbiCsvOption);
        if (!writeVbiCsv(metaData, fileName)) {
            qCritical() << "Failed to write output file:" << fileName;
            return 1;
        }
    }
    if (parser.isSet(writeAudacityLabelsOption)) {
        const QString &fileName = parser.value(writeAudacityLabelsOption);
        if (!writeAudacityLabels(metaData, fileName)) {
            qCritical() << "Failed to write output file:" << fileName;
            return 1;
        }
    }
    if (parser.isSet(writeFfmetadataOption)) {
        const QString &fileName = parser.value(writeFfmetadataOption);
        if (!writeFfmetadata(metaData, fileName)) {
            qCritical() << "Failed to write output file:" << fileName;
            return 1;
        }
    }
    if (parser.isSet(writeClosedCaptionsOption)) {
        const QString &fileName = parser.value(writeClosedCaptionsOption);
        if (!writeClosedCaptions(metaData, fileName)) {
            qCritical() << "Failed to write output file:" << fileName;
            return 1;
        }
    }

    // Quit with success
    return 0;
}
