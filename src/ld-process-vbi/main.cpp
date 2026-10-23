/************************************************************************

    main.cpp

    ld-process-vbi - VBI and IEC NTSC specific processor for ld-decode
    Copyright (C) 2018-2025 Simon Inns

    This file is part of ld-decode-tools.

    ld-process-vbi is free software: you can redistribute it and/or
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
#include <QThread>

#include "tbc/logging.h"
#include "ldxfilter.h"
#include "fieldvbi.h"
#include "vbilinedecoder.h"
#include "tbc/metadataoptions.h"
#include "tbc/commonoptions.h"
#include "stackproject.h"
#include "tbc/fieldpool.h"
#include "vbilinedecoder.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-process-vbi");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-process-vbi - VBI and IEC NTSC specific processor for ld-decode\n"
                "\n"
                "(c)2018-2025 Simon Inns\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Metadata options: --meta, --input-metadata and --output-metadata
    MetadataOptions metadataOptions(MetadataOptions::InputFile | MetadataOptions::OutputFile);
    metadataOptions.addTo(parser);

    // Option to disable metadata back-up (-n)
    NoBackupOption showNoBackupOption;
    showNoBackupOption.addTo(parser);
    LegacyBackupOption legacyOption;
    legacyOption.addTo(parser);

    // Option to select the number of threads (-t)
    ThreadsOption threadsOption;
    threadsOption.addTo(parser);

    // Positional argument to specify input TBC file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC file"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return 2;

    // Get the options from the parser
    bool noBackup = showNoBackupOption.isSet(parser);

    qint32 maxThreads;
    if (!threadsOption.process(parser, maxThreads)) return 2;

    // Get the arguments from the parser
    QString inputFilename;
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 1) {
        inputFilename = positionalArguments.at(0);
    } else {
        // Quit with error
        qCritical("You must specify an input TBC file");
        return 2;
    }

    // An LDX stream on the standard input: the metadata of each field are made as the field goes by, and the stream
    // goes on down the pipe
    if (inputFilename == "-") {
        LdxFieldFilter::Options options;
        options.tool = "ld-process-vbi";
        options.version = APP_COMMIT;
        options.detail = "VBI";
        options.threads = maxThreads;
        QString streamError;
        if (!LdxFieldFilter::run(options, [](qint32, const SourceVideo::Data &data, const LdDecodeMetaData::VideoParameters &videoParameters,
                                              LdDecodeMetaData::Field &field) {
            const qint32 first = VbiLineDecoder::startFieldLine;
            const SourceVideo::Data lines = data.mid((first - 1) * videoParameters.fieldWidth,
                                                     (VbiLineDecoder::endFieldLine - first + 1) * videoParameters.fieldWidth);
            decodeFieldVbi(lines, first, videoParameters, field);
        }, streamError)) {
            qCritical().noquote() << streamError;
            return 1;
        }
        return 0;
    }

    // A session is its capture when it has one source on: the tool works on the capture itself (and keeps the map in its metadata as it is)
    if (inputFilename.endsWith(".stack", Qt::CaseInsensitive)) {
        QString captureFile, sessionError;
        if (!StackProject::singleCapture(inputFilename, captureFile, sessionError)) {
            qCritical().noquote() << sessionError;
            return 2;
        }
        qInfo().noquote() << inputFilename << "is read as its capture" << captureFile;
        inputFilename = captureFile;
    }

    // Work out the metadata filenames
    QString inputMetadataFilename;
    if (!metadataOptions.selectInput(inputFilename, true, inputMetadataFilename)) return 1;

    // Open the source video metadata
    LdDecodeMetaData metaData;
    qInfo().nospace().noquote() << "Reading metadata from " << inputMetadataFilename;
    if (!metaData.read(inputMetadataFilename)) {
        qCritical() << "Unable to open TBC metadata file";
        return 1;
    }

    // The output defaults to updating the input in place, in the same format
    QString outputMetadataFilename;
    if (!metadataOptions.selectOutput(inputMetadataFilename, metaData.getFormat(), outputMetadataFilename)) return 2;

    // If we're overwriting the input metadata file, back it up first
    if (!NoBackupOption::backUp(inputMetadataFilename, outputMetadataFilename, noBackup, ".bup", legacyOption.isSet(parser))) return 1;

    // Perform the processing
    qInfo() << "Beginning VBI processing...";
    FieldPool::Settings pool;
    pool.name = "VBI";
    pool.firstLine = VbiLineDecoder::startFieldLine;
    pool.lastLine = VbiLineDecoder::endFieldLine;
    // (written alone only over the file that was read: another file gets everything)
    if (outputMetadataFilename == inputMetadataFilename) {
        pool.tables = LdDecodeMetaData::FieldTable | LdDecodeMetaData::VbiTable | LdDecodeMetaData::VitcTable | LdDecodeMetaData::ClosedCaptionTable;
    }
    pool.createWorker = [](QAtomicInt &abort, FieldPool &fieldPool) -> QThread * { return new VbiLineDecoder(abort, fieldPool); };
    // Only the VBI, NTSC, VITC and closed caption data of a field change
    pool.storeField = [](LdDecodeMetaData &metadata, qint32 fieldNumber, const LdDecodeMetaData::Field &field) {
        metadata.updateFieldVbi(field.vbi, fieldNumber);
        metadata.updateFieldNtsc(field.ntsc, fieldNumber);
        metadata.updateFieldVitc(field.vitc, fieldNumber);
        metadata.updateFieldClosedCaption(field.closedCaption, fieldNumber);
    };
    if (!FieldPool(pool, inputFilename, outputMetadataFilename, maxThreads, metaData).process()) return 1;

    if (outputMetadataFilename == inputMetadataFilename) {
        MetadataOptions::warnIfOtherFormatStale(inputFilename, outputMetadataFilename);
    }

    // Quit with success
    return 0;
}
