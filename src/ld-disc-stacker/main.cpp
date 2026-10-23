/************************************************************************

    main.cpp

    ld-disc-stacker - Disc stacking for ld-decode
    Copyright (C) 2020-2025 Simon Inns

    This file is part of ld-decode-tools.

    ld-disc-stacker is free software: you can redistribute it and/or
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
#include <QFile>
#include <QFileInfo>

#include "tbc/logging.h"
#include "tbc/metadataoptions.h"
#include "tbc/commonoptions.h"
#include "tbc/sourcefilechecks.h"
#include "lddecodemetadata.h"
#include "targetvideo.h"
#include "sourcevideo.h"
#include "stackingpool.h"
#include "stackproject.h"
#include "mappedmetadata.h"
#include "fieldaudio.h"
#include "streamcopy.h"
#include "ldxstream.h"
#include "ldxcapture.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-disc-stacker");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser ---------------------------------------------------------------------------------
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-disc-stacker - Disc stacking for ld-decode\n"
                "\n"
                "(c)2020-2025 Simon Inns\n"
                //"2024 updated by Vrunk11\n" - If you want to claim copyright, use a name and (c) format - Simon
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode"
                "\n"
                "For more info on stacking mode, use --help-mode");
    //parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    QCommandLineOption HelpOption(QStringList() << "?" << "h" << "help",
                                       QCoreApplication::translate("main", "Displays help on commandline options."));
    parser.addOption(HelpOption);

    // Option to show more info during stacking
    QCommandLineOption helpModeOption(QStringList() << "help-mode",
                                       QCoreApplication::translate("main", "Show info about stacking mode"));
    parser.addOption(helpModeOption);

    // Option to show more info during stacking
    QCommandLineOption verboseOption(QStringList() << "V" << "verbose",
                                       QCoreApplication::translate("main", "Show more info during stacking"));
    parser.addOption(verboseOption);

    // Metadata options: --meta, --input-metadata and --output-metadata
    MetadataOptions metadataOptions(MetadataOptions::InputFile | MetadataOptions::OutputFile,
        QCoreApplication::translate("main", "Specify the input metadata file for the first input file (default input.db or input.json, see --meta)"),
        QCoreApplication::translate("main", "Specify the output metadata file (default output.db, or output.json for JSON input)"));
    metadataOptions.addTo(parser);

    // Option to reverse the field order (-r)
    ReverseOption setReverseOption;
    setReverseOption.addTo(parser);

    // Option to select the number of threads (-t)
    ThreadsOption threadsOption;
    threadsOption.addTo(parser);

    // Option to select the stacking mode (-m)
    QCommandLineOption modeOption(QStringList() << "m" << "mode",
                                        QCoreApplication::translate(
                                         "main", "Specify the stacking mode to use (default is Auto) -1 = auto / 0 = mean / 1 = median / 2 = smart mean / 3 = smart neighbor / 4 = neighbor"),
                                         QCoreApplication::translate("main", "number"));
    parser.addOption(modeOption);

    // Option to select the smart-threshold (-st)
    QCommandLineOption smartThresholdOption(QStringList() << "st" << "smart-threshold",
                                        QCoreApplication::translate(
                                         "main", "Specify the range of value in 8 bit (0~128) for selecting sample where the distance to the median didn't exceed the selected value for applying mean (default is 15)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(smartThresholdOption);

    // Option to disable differential dropout detection
    QCommandLineOption noDiffDodOption(QStringList() << "no-diffdod",
                                        QCoreApplication::translate(
                                         "main", "Do not use differential dropout detection on low source pixels"));
    parser.addOption(noDiffDodOption);

    // Option to disable differential dropout detection
    QCommandLineOption noMapOption(QStringList() << "no-map",
                                        QCoreApplication::translate(
                                         "main", "Disable mapping requirement"));
    parser.addOption(noMapOption);

    // Leave out what a map did not measure
    IgnoreMapOption ignoreMapOption;
    ignoreMapOption.addTo(parser);

    // The analogue audio of the first source is written with the video in an LDX output
    // Which other streams of the sources are copied into an LDX output
    QCommandLineOption copyStreamsOption("copy-streams",
                                         QCoreApplication::translate("main", "The other streams of the sources (EFM, AC3, RF, 24-bit audio) to copy as they are into an LDX output, with their own times: none, default (the EFM, AC3 and 24-bit audio of the first source), or a list such as 1:efm,1:ac3,2:#5 (the number of the source from 1, then efm, ac3, rf, audio, all or #track)"),
                                         QCoreApplication::translate("main", "streams"), "default");
    parser.addOption(copyStreamsOption);

    QCommandLineOption noAudioOption("no-audio", QCoreApplication::translate("main", "Do not write the analogue audio of the first source with the video (an LDX output has it by default)"));
    parser.addOption(noAudioOption);


    QCommandLineOption measuredOnlyOption("measured-only",
                                          QCoreApplication::translate("main", "For the sources read through a map (a session made by ld-discmap): leave out the frames whose number was not read from their own VBI (read from one line only, replaced by the sequence of the numbers, or guessed)"));
    parser.addOption(measuredOnlyOption);

    // Option to passthrough dropouts present in every source
    QCommandLineOption passthroughOption(QStringList() << "passthrough",
                                        QCoreApplication::translate(
                                         "main", "Pass-through dropouts present on every source"));
    parser.addOption(passthroughOption);

    // Option to check integrity off every source and remove bad frame
    QCommandLineOption integrityOption(QStringList() << "it" << "integrity",
                                        QCoreApplication::translate(
                                         "main", "Check if frames contain skip or sample drop and discard bad source for specific frame"));
    parser.addOption(integrityOption);

    // Positional argument to specify input video file
    parser.addPositionalArgument("inputs", QCoreApplication::translate(
                                     "main", "Specify input TBC files (- as first source for piped input; a .stack session file, or an .ldx file that keeps a session, is read as the stack of its sources; capture.ldx#3 is the track 3 of an LDX file), or one .stack or .ldx session to stack as it says"));

    // Positional argument to specify output video file
    parser.addPositionalArgument("output", QCoreApplication::translate(
                                     "main", "Specify output TBC file (omit or - for piped output; with a .stack file, omit it to use the one in the session)"));

    // Process the command line options and arguments given by the user -----------------------------------------------
    parser.process(a);

    // show info about stacking mode
    if (parser.isSet(helpModeOption)) {
        qInfo() << "ld-disc-stacker - Disc stacking for ld-decode\n";
        qInfo() << "(c)2020-2022 Simon Inns";
        //qInfo() << "2024 updated by Vrunk11"; - If you want to claim copyright, use a name and (c) format - Simon
        qInfo() << "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode";
        qInfo() << "For more info on stacking mode, use --help-mode\n";
        qInfo() << "Mode:\n";
        qInfo() << "(-1) auto            : select mode depending on the number off frame available (2f: mean, 3~4f: smart mean, 5+f: smart-neighbor)\n";
        qInfo() << " (0) mean            : average all samples not marked as dropouts using mean\n";
        qInfo() << " (1) median          : find the median from samples not marked as dropout\n";
        qInfo() << " (2) smart mean      : find the median from samples not marked as dropout then average all value within (median + smartThreshold) or (median - smart Threshold) using mean\n";
        qInfo() << " (3) smart neighbor  : find the median for every surroundings pixel not marked as dropout then find the closest sample to the surrounding median value for each neighbor";
        qInfo() << "                      then take the closest value to the median of the current sample from the different closest value found";
        qInfo() << "                      then average all value within (selectedSample + smartThreshold) or (selectedSample - smart threshold) using mean";
        qInfo() << "                      when only 2 sources are available, it take the closest sample to the neighbor\n";
        qInfo() << "(4) neighbor        : find the median for every surroundings pixel not marked as dropout then find the closest sample to the surrounding median value for each neighbor";
        qInfo() << "                      then take the closest value to the median of the current sample from the different closest value found then average the selected sample with the median";
        qInfo() << "                      when only 2 sources are available, it take the closest sample to the neighbor";
        return 0; // Exit after showing detailed help
    }

    // Check for help options first
    if (parser.isSet(HelpOption)) {
        parser.showHelp(); // This will exit the application
    }

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return 2;

    // Get the options from the parser
    bool reverse = setReverseOption.isSet(parser);
    bool verbose = parser.isSet(verboseOption);
    bool noDiffDod = parser.isSet(noDiffDodOption);
    bool noMap = parser.isSet(noMapOption);
    const bool measuredOnly = parser.isSet(measuredOnlyOption);
    bool passThrough = parser.isSet(passthroughOption);
    bool integrityCheck = parser.isSet(integrityOption);

    // Get the arguments from the parser
    qint32 mode = -1;
    if (parser.isSet(modeOption)) {
        mode = parser.value(modeOption).toInt();

        if (mode > 4 || mode < 0) {
            qInfo() << "Specified mode (" << mode << ") is unknown using auto mode instead";
            mode = -1;
        }
    }

    // Get the arguments from the parser
    qint32 smartThreshold = (15*256);
    if (parser.isSet(smartThresholdOption)) {
        smartThreshold = parser.value(smartThresholdOption).toInt();

        if (smartThreshold > 128 || smartThreshold < 0) {
            qInfo() << "Specified threshold (" << (smartThreshold) << ") is out off range using 15 instead";
            smartThreshold = (15*256);
        }
        else
        {
            smartThreshold = (smartThreshold*256);
        }
    }

    // Get the arguments from the parser
    qint32 maxThreads;
    if (!threadsOption.process(parser, maxThreads)) return 2;

    // Require source and target filenames
    QVector<QString> inputFilenames;
    QString outputFilename = "-";
    QStringList positionalArguments = parser.positionalArguments();

    // A .stack session file (made by ld-stacker-gui) alone, or with the output, gives the sources that are on,
    // the offset of each, the stacking options and the output. What is given on the command line has the last
    // word. (With 2 inputs or more and the output, a .stack file is a source like another: it is read as the
    // stack of its own sources, which are stacked with the others.)
    bool fromSession = false;
    QStringList enabledIds;
    QVector<qint32> offsets;
    QVector<FrameMap> inputMaps;        // for each input: the map the session has for the source (empty: none, the capture is read as it is)
    QVector<bool> inputHasMap;
    QVector<bool> inputIgnoreMap;       // the sources that the session says to read as the capture is (Follow map off)
    QString sessionCopyStreams;         // the streams to copy that the session chose, as --copy-streams says them (empty: the session did not choose)
    if (!positionalArguments.isEmpty() && StackProject::isSession(positionalArguments.at(0))
        && positionalArguments.count() <= 2) {
        StackProject session;
        QString sessionError;
        if (!session.load(positionalArguments.at(0), sessionError)) {
            qCritical().noquote() << sessionError;
            return 1;
        }

        QStringList sessionArguments;
        for (const StackSource &source: session.sources) {
            if (!source.enabled) continue;
            sessionArguments.append(source.resolvedReference());
            offsets.append(source.offset);
            inputHasMap.append(source.activeMap() != nullptr);
            inputIgnoreMap.append(!source.followMap);
            enabledIds.append(source.id);
            inputMaps.append(source.activeMap() ? *source.activeMap() : FrameMap());
        }
        if (sessionArguments.count() < 2) {
            qCritical("The session needs at least 2 sources that are on");
            return 2;
        }

        QString sessionOutput = positionalArguments.count() == 2 ? positionalArguments.at(1) : session.outputFile;
        if (sessionOutput.isEmpty()) {
            qCritical("The session has no output file: give one after it");
            return 2;
        }
        sessionArguments.append(sessionOutput);
        positionalArguments = sessionArguments;
        if (session.stacking.copyStreamsSet) {
            QStringList items;
            for (const auto &entry: session.stacking.copyStreams) {
                const qint32 at = enabledIds.indexOf(entry.first);
                if (at >= 0) items << QStringLiteral("%1:#%2").arg(at + 1).arg(entry.second);
            }
            sessionCopyStreams = items.isEmpty() ? QStringLiteral("none") : items.join(QLatin1Char(','));
        }
        fromSession = true;

        if (!parser.isSet(modeOption)) mode = session.stacking.mode;
        if (!parser.isSet(smartThresholdOption)) smartThreshold = qBound(0, session.stacking.smartThreshold, 128) * 256;
        if (!session.stacking.diffDod) noDiffDod = true;
        if (session.stacking.passthrough) passThrough = true;
        if (session.stacking.integrity) integrityCheck = true;
        if (session.stacking.reverseFields) reverse = true;
        qInfo().noquote() << "Stacking" << (sessionArguments.count() - 1) << "sources of the session, lined up by their offsets";
    }

    qint32 totalNumberOfInputFiles = positionalArguments.count() - 1;

    // Ensure we don't have more than 32 sources
    if (totalNumberOfInputFiles > 32) {
        qCritical() << "A maximum of 32 input TBC files are supported";
        return 2;
    }

    // Get the input TBC sources
    if (positionalArguments.count() >= 3) {
        // Resize the input filenames vector according to the number of input files supplied
        inputFilenames.resize(totalNumberOfInputFiles);

        for (qint32 i = 0; i < positionalArguments.count() - 1; i++) {
            inputFilenames[i] = positionalArguments.at(i);
        }

        // Warn if only 2 sources are used
        if (positionalArguments.count() == 3) {
            qInfo() << "Only 2 input sources specified - stack will be only based on averaging (3 or more sources are recommended)";
        }
    } else {
        // Quit with error
        qCritical("You must specify at least 2 input and 1 output TBC file");
        return 2;
    }

    // Get the output TBC (should be the last argument of the command line
    outputFilename = positionalArguments.at(positionalArguments.count() - 1);

    // If the first input filename is "-" (piped input), it is an LDX stream (it brings its own metadata), unless the input
    // metadata file is given, for the video of a stream that has none
    bool ldxInput = false;
    if (inputFilenames[0] == "-") {
        if (!metadataOptions.isInputFileSet()) {
            QString streamError;
            if (!LdxStream::openStdin(streamError)) {
                qCritical().noquote() << streamError;
                qCritical("With piped video that is not an LDX stream, you must also specify the input metadata file with --input-metadata");
                return 2;
            }
            ldxInput = true;
            qInfo().noquote() << "The first source is an LDX stream that holds" << (LdxStream::instance()->isCvbs() ? "CVBS" : "TBC") << "video";
        }
    }

    // The output "-" is an LDX stream (with the metadata of each field, in the stream), unless an output metadata file is
    // given for a raw TBC
    const bool ldxOutput = outputFilename == "-" && !metadataOptions.isOutputFileSet();

    // No input is the output, and the output stays in the container of the first input (TBC or CVBS)
    if (const int code = SourceFileChecks::outputIsNotAnInput(inputFilenames, outputFilename)) return code;

    // The inputs are all TBC or all CVBS (a .stack input has the format of its first source)
    for (qint32 i = 1; i < totalNumberOfInputFiles; i++) {
        if (Cvbs::isDataFileName(StackProject::referenceFile(inputFilenames[i])) != Cvbs::isDataFileName(StackProject::referenceFile(inputFilenames[0]))) {
            qCritical().noquote() << inputFilenames[i] << "is not of the same format (TBC or CVBS) as" << inputFilenames[0]
                                  << "- TBC and CVBS sources cannot be stacked together";
            return 2;
        }
    }

    // Check that none of the input filenames are repeated
    if (const int code = SourceFileChecks::inputsAreDistinct(inputFilenames)) return code;

    // Check that the output file does not already exist
    if (const int code = SourceFileChecks::outputIsNew(outputFilename)) return code;

    // Prepare for stacking process -----------------------------------------------------------------------------------

    qInfo() << "Starting preparation for disc stacking processes...";
    // Open the source video metadata
    tbcDebugStream() << "main(): Opening source video metadata files..";
    QVector<LdDecodeMetaData *> ldDecodeMetaData;
    ldDecodeMetaData.resize(totalNumberOfInputFiles);
    QVector<LdDecodeMetaData *> captureMetadata(totalNumberOfInputFiles, nullptr);      // (of the captures that are read through a map)
    QVector<std::shared_ptr<const QVector<qint32>>> viewTables(totalNumberOfInputFiles);      // for an input that keeps a map in its metadata
    QVector<FieldAudio::Range> audioRanges0;       // the analogue audio of each field of the first input, when it is read through a map in its metadata
    QVector<QVector<qint32>> captureFrames(totalNumberOfInputFiles);       // for an input that is read through a map: the frame of the capture each of its frames is
    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) {
        // Create an object for the source video
        ldDecodeMetaData[i] = new LdDecodeMetaData;
    }

    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) {
        // Work out the metadata filename
        QString metadataFilename;
        if (i == 0 && ldxInput) metadataFilename = "the LDX stream";
        else if (!metadataOptions.selectInput(inputFilenames[i], i == 0, metadataFilename)) return 1;
        qInfo().nospace().noquote() << "Reading input #" << i << " metadata from " << metadataFilename;

        // Open it (the metadata of an LDX stream are those it began with: stacking needs them for the whole of the source
        // before it begins, so a stream whose metadata come field by field cannot be stacked)
        if (i == 0 && ldxInput) {
            QString streamError;
            if (!LdxStream::instance()->readMetadata(*ldDecodeMetaData[0], streamError) || ldDecodeMetaData[0]->getNumberOfFields() == 0) {
                qCritical("The LDX stream does not send the metadata of all its fields before its video. Stacking needs them, with the VBI "
                          "and the mapping: pack the capture with its whole metadata (ld-container pack) or use a file");
                return 1;
            }
        } else if (i < inputHasMap.size() && inputHasMap[i]) {
            // The capture seen through its map in the session: the metadata of the mapped copy, without the copy
            LdDecodeMetaData *capture = new LdDecodeMetaData;
            if (!capture->read(metadataFilename)) {
                qCritical() << "Unable to open TBC metadata file - cannot continue";
                return 1;
            }
            QString mapError;
            if (!MappedMetadata::build(*capture, inputMaps[i], MappedMetadata::paddedAudioSamples(*capture, metadataFilename), *ldDecodeMetaData[i], mapError, measuredOnly)) {
                qCritical().noquote() << "The map of" << inputFilenames[i] << "cannot be used:" << mapError;
                return 1;
            }
            if (inputMaps[i].captureSize > 0 && QFileInfo(inputFilenames[i]).size() != inputMaps[i].captureSize) {
                qCritical().noquote() << "The map of" << inputFilenames[i] << "was made for another capture (it is not of the same size)";
                return 1;
            }
            captureMetadata[i] = capture;
            for (qint32 place = 1; place <= inputMaps[i].frameCount(); place++) captureFrames[i].append(inputMaps[i].frameAt(place).captureFrame);
            qInfo().noquote() << "Input #" << i << "is read through its map of" << inputMaps[i].frameCount() << "frames";
        } else if (!ldDecodeMetaData[i]->read(metadataFilename)) {
            qCritical() << "Unable to open TBC metadata file - cannot continue";
            return 1;
        }
        if (!(i == 0 && ldxInput) && !(i < inputHasMap.size() && inputHasMap[i]) && !(i < inputIgnoreMap.size() && inputIgnoreMap[i]) && !ignoreMapOption.isSet(parser)) {
            QString mapError;
            viewTables[i] = MappedMetadata::applyEmbeddedMap(*ldDecodeMetaData[i], metadataFilename, inputFilenames[i], mapError, measuredOnly, i == 0 ? &audioRanges0 : nullptr, &captureFrames[i]);
            if (!mapError.isEmpty()) {
                qCritical().noquote() << "Unable to use the map in the metadata of input" << i << ":" << mapError << "(--ignore-map reads the capture as it is)";
                return 1;
            }
            if (viewTables[i]) qInfo().nospace() << "Input #" << i << " is read through the map in its metadata (--ignore-map reads it as it is)";
        }
    }

    // Metadata filename for output TBC, in the same format as the first input
    // (JSON in gives JSON out) unless named explicitly
    QString outputMetadataFilename;
    if (!ldxOutput && !metadataOptions.selectOutput(LdDecodeMetaData::metadataFileName(outputFilename, ldDecodeMetaData[0]->getFormat()),
                                      ldDecodeMetaData[0]->getFormat(), outputMetadataFilename)) {
        return 2;
    }

    // Reverse field order if required
    if (reverse) {
        qInfo() << "Expected field order is reversed to second field/first field";
        for (qint32 i = 0; i < totalNumberOfInputFiles; i++)
            ldDecodeMetaData[i]->setIsFirstFieldFirst(false);
    }

    // Show if DiffDOD is disabled
    if (noDiffDod) {
        qInfo() << "Differential Dropout Detection is disabled";
    }

    // Show if pass-through is enabled
    if (passThrough) {
        qInfo() << "Passing through dropouts present on every input source";
    }

    // Show and open input source TBC files
    tbcDebugStream() << "Opening source video files...";
    QVector<SourceVideo *> sourceVideos;
    sourceVideos.resize(totalNumberOfInputFiles);
    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) {
        // Create an object for the source video
        sourceVideos[i] = new SourceVideo;
    }

    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) {
        LdDecodeMetaData::VideoParameters videoParameters = ldDecodeMetaData[i]->getVideoParameters();

        qInfo().nospace() << "Opening input #" << i << ": " << videoParameters.fieldWidth << "x" << videoParameters.fieldHeight <<
                    " - input filename is " << inputFilenames[i];

        // Open the source TBC
        const bool throughMap = i < inputHasMap.size() && inputHasMap[i];
        const qint32 fieldLength = videoParameters.fieldWidth * videoParameters.fieldHeight;
        if (!(throughMap ? sourceVideos[i]->openMapped(inputFilenames[i], fieldLength, -1, inputMaps[i], *captureMetadata[i])
                         : (viewTables[i] ? sourceVideos[i]->openMappedFields(inputFilenames[i], fieldLength, -1, viewTables[i])
                                          : sourceVideos[i]->open(inputFilenames[i], fieldLength)))) {
            // Could not open source video file
            qInfo() << "Unable to open input source" << i;
            qInfo() << "Please verify that the specified source video files exist with the correct file permissions";
            return 1;
        }

        // Verify TBC and metadata input fields match
        if (sourceVideos[i]->getNumberOfAvailableFields() != ldDecodeMetaData[i]->getNumberOfFields()) {
            qInfo() << "Warning: TBC file contains" << sourceVideos[i]->getNumberOfAvailableFields() <<
                       "fields but the metadata indicates" << ldDecodeMetaData[i]->getNumberOfFields() <<
                       "fields - some fields will be ignored";
            qInfo() << "Update your copy of ld-decode and try again, this shouldn't happen unless the metadata has been corrupted";
        }

        // Ensure source video has VBI data
        if (!fromSession && !ldDecodeMetaData[i]->getFieldVbi(1).inUse) {
            qInfo() << "Source video" << i << "does not appear to have valid VBI data in the metadata.";
            qInfo() << "Please try running ld-process-vbi on the source video and then try again";
            return 1;
        }

        // Ensure that the video source standard matches the primary source
        if (ldDecodeMetaData[0]->getVideoParameters().system != videoParameters.system) {
            qInfo() << "All additional input sources must have the same video system as the initial source!";
            qInfo() << "Initial source is" << ldDecodeMetaData[0]->getVideoSystemDescription() << " and current source is" <<
                       ldDecodeMetaData[i]->getVideoSystemDescription();
            return 1;
        }

        if (!fromSession && !videoParameters.isMapped && !noMap) {
            qInfo() << "Source video" << i << "has not been mapped - run ld-discmap on all source videos and try again or use option \"no-map\"";
            qInfo() << "Disc stacking relies on accurate VBI frame numbering to match source frames together";
            return 1;
        }
        else if(noMap && !fromSession)
        {
            if(!videoParameters.isMapped)
            {
                qInfo() << "Source video" << i << "has not been mapped - be careful using option no-map";
            }
        }
    }

    // Perform the disc stacking processes ----------------------------------------------------------------------------
    qInfo() << "Initial source checks are ok and sources are loaded";
    qint32 result = 0;
    StackingPool stackingPool(outputFilename, outputMetadataFilename, maxThreads,
                                ldDecodeMetaData, sourceVideos, mode, smartThreshold, reverse, noDiffDod, passThrough, integrityCheck, verbose);
    if (fromSession) stackingPool.setOffsets(offsets);
    stackingPool.setLdx(ldxOutput, LdxCapture::historyOf(inputFilenames[0]), TargetVideo::sourceIsCvbs(inputFilenames[0]));
    // The other streams of the sources (EFM, AC3, 24-bit audio...) are copied as they are into an LDX file output, and the history says so
    if (ldxOutput || LdxCapture::isLdxName(outputFilename)) {
        QStringList sourceNames;
        for (qint32 i = 0; i < totalNumberOfInputFiles; i++) sourceNames << inputFilenames[i];
        const QVector<StreamCopy::Candidate> available = StreamCopy::candidates(sourceNames);
        QVector<StreamCopy::Candidate> chosen;
        QString copyError;
        if (!StreamCopy::parseChoice(parser.isSet(copyStreamsOption) || sessionCopyStreams.isEmpty() ? parser.value(copyStreamsOption) : sessionCopyStreams, available, chosen, copyError)) {
            qCritical().noquote() << "--copy-streams:" << copyError;
            return 2;
        }
        stackingPool.setCopiedStreams(chosen, sourceNames);
        stackingPool.setCaptureFrames(captureFrames);
        for (const StreamCopy::Candidate &candidate : chosen) qInfo().noquote() << "Copying the stream" << candidate.description() << "of source" << (candidate.source + 1);
    } else if (parser.isSet(copyStreamsOption)) {
        qWarning() << "--copy-streams: only an LDX output (a file, or the stream "-") takes the other streams of the sources";
    }

    // The analogue audio of the first source (the reference) goes with the video in an LDX output: the audio of the fields of the capture, or of the capture
    // seen through its map
    if (!parser.isSet(noAudioOption) && !ldxInput && !TargetVideo::sourceIsCvbs(inputFilenames[0])) {
        QVector<FieldAudio::Range> ranges;
        if (inputHasMap.size() > 0 && inputHasMap[0]) {
            QString viewError;
            const auto table = MappedMetadata::viewFields(*captureMetadata[0], inputMaps[0], viewError);
            if (table) ranges = FieldAudio::rangesOfView(*captureMetadata[0], *table, MappedMetadata::paddedAudioSamples(*captureMetadata[0], QString()));
        } else if (viewTables[0]) {
            ranges = audioRanges0;
        } else {
            ranges = FieldAudio::rangesOf(*ldDecodeMetaData[0]);
        }
        auto fieldAudio = std::make_shared<FieldAudio>();
        if (fieldAudio->open(inputFilenames[0], ranges)) stackingPool.setAudio(fieldAudio);
        else if (ldxOutput) qInfo() << "The first source has no analogue audio to write with the video";
    }
    if (!stackingPool.process()) result = 1;

    // Close open source video files
    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) sourceVideos[i]->close();

    // Remove metadata objects
    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) delete ldDecodeMetaData[i];

    // Remove source video objects
    for (qint32 i = 0; i < totalNumberOfInputFiles; i++) delete sourceVideos[i];

    // Quit
    return result;
}
