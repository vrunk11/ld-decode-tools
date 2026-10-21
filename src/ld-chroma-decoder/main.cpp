/************************************************************************

    main.cpp

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2018-2025 Simon Inns
    Copyright (C) 2019-2022 Adam Sampson
    Copyright (C) 2021 Chad Page
    Copyright (C) 2021 Phillip Blucas

    This file is part of ld-decode-tools.

    ld-chroma-decoder is free software: you can redistribute it and/or
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
#include <QFileInfo>
#include <fstream>
#include <memory>

#include "decoderpool.h"
#include "lddecodemetadata.h"
#include "ldxstream.h"
#include "cvbs.h"
#include "tbc/logging.h"
#include "tbc/metadataoptions.h"
#include "tbc/pipedinputoptions.h"
#include "tbc/commonoptions.h"

#include "comb.h"
#include "monodecoder.h"
#include "ntscdecoder.h"
#include "outputwriter.h"
#include "palcolour.h"
#include "paldecoder.h"
#include "transformpal.h"

// Load the thresholds file for the Transform decoders, if specified. We must
// do this after PalColour has been configured, so we know how many values to
// expect.
//
// Return true on success; on failure, print a message and return false.
static bool loadTransformThresholds(QCommandLineParser &parser, QCommandLineOption &transformThresholdsOption, PalColour::Configuration &palConfig)
{
    if (!parser.isSet(transformThresholdsOption)) {
        // Nothing to load
        return true;
    }

    // Open the file
    QString filename = parser.value(transformThresholdsOption);
    std::ifstream thresholdsFile(filename.toStdString());
    if (thresholdsFile.fail()) {
        qCritical() << "Transform thresholds file could not be opened:" << filename;
        return false;
    }

    // Read threshold values from the file
    palConfig.transformThresholds.clear();
    while (true) {
        double value;
        thresholdsFile >> value;
        if (thresholdsFile.eof()) {
            break;
        }
        if (value < 0.0 || value > 1.0) {
            qCritical() << "Values in Transform thresholds file must be between 0 and 1:" << filename;
            return false;
        }
        if (thresholdsFile.fail()) {
            qCritical() << "Couldn't parse Transform thresholds file:" << filename;
            return false;
        }
        palConfig.transformThresholds.push_back(value);
    }

    // Check we've read the right number
    if (palConfig.transformThresholds.size() != palConfig.getThresholdsSize()) {
        qCritical() << "Transform thresholds file contained" << palConfig.transformThresholds.size()
                    << "values, expecting" << palConfig.getThresholdsSize() << "values:" << filename;
        return false;
    }

    thresholdsFile.close();
    return true;
}

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-chroma-decoder");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-chroma-decoder - Colourisation filter for ld-decode\n"
                "\n"
                "(c)2018-2020 Simon Inns\n"
                "(c)2019-2021 Adam Sampson\n"
                "(c)2018-2021 Chad Page\n"
                "(c)2021 Phillip Blucas\n"
                "Contains PALcolour: Copyright (c)2018 William Andrew Steer\n"
                "Contains Transform PAL: Copyright (c)2014 Jim Easterbrook\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // -- General options --

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Metadata options: --meta and --input-metadata
    MetadataOptions metadataOptions(MetadataOptions::InputFile);
    metadataOptions.addTo(parser);

    // -- Input from a pipe --

    // Options for video that comes in a pipe: what it carries, and for piped input without metadata the video system and its levels
    PipedInputOptions pipedOptions;
    pipedOptions.addTo(parser);

    QCommandLineOption bufferFramesOption(QStringList() << "buffer-frames",
                                          QCoreApplication::translate("main", "Piped input: frames read ahead of the one being decoded, and kept behind it (default 32)"),
                                          QCoreApplication::translate("main", "number"));
    parser.addOption(bufferFramesOption);

    // Option to select start frame (sequential) (-s)
    QCommandLineOption startFrameOption(QStringList() << "s" << "start",
                                        QCoreApplication::translate("main", "Specify the start frame number"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(startFrameOption);

    // Option to select length (-l)
    QCommandLineOption lengthOption(QStringList() << "l" << "length",
                                        QCoreApplication::translate("main", "Specify the length (number of frames to process)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(lengthOption);

    // Option to reverse the field order (-r)
    ReverseOption setReverseOption;
    setReverseOption.addTo(parser);

    // Option to specify chroma gain
    QCommandLineOption chromaGainOption(QStringList() << "chroma-gain",
                                        QCoreApplication::translate("main", "Gain factor applied to chroma components (default 1.0)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(chromaGainOption);

    // Option to specify chroma phase
    QCommandLineOption chromaPhaseOption(QStringList() << "chroma-phase",
                                        QCoreApplication::translate("main", "Phase rotation applied to chroma components (degrees; default 0.0)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(chromaPhaseOption);

    // Option to select the output format (-p)
    QCommandLineOption outputFormatOption(QStringList() << "p" << "output-format",
                                       QCoreApplication::translate("main", "Output format (rgb, yuv, y4m; default rgb); RGB48, YUV444P16, GRAY16 pixel formats are supported"),
                                       QCoreApplication::translate("main", "output-format"));
    parser.addOption(outputFormatOption);

    // Option to set the black and white output flag (causes output to be black and white) (-b)
    QCommandLineOption setBwModeOption(QStringList() << "b" << "blackandwhite",
                                       QCoreApplication::translate("main", "Output in black and white"));
    parser.addOption(setBwModeOption);

    // Option to select output padding (-pad)
    QCommandLineOption outputPaddingOption(QStringList() << "pad" << "output-padding",
                                       QCoreApplication::translate("main", "Pad the output frame to a multiple of this many pixels on both axes (1 means no padding, maximum is 32)"),
                                       QCoreApplication::translate("main", "number"));
    parser.addOption(outputPaddingOption);

    // Option to select which decoder to use (-f)
    QCommandLineOption decoderOption(QStringList() << "f" << "decoder",
                                     QCoreApplication::translate("main", "Decoder to use (pal2d, transform2d, transform3d, ntsc1d, ntsc2d, ntsc3d, ntsc3dnoadapt, mono; default automatic)"),
                                     QCoreApplication::translate("main", "decoder"));
    parser.addOption(decoderOption);

    // Option to select the number of threads (-t)
    ThreadsOption threadsOption;
    threadsOption.addTo(parser);

    // Option to override calculated firstActiveFieldLine in our video parameters (-ffll)
    QCommandLineOption firstFieldLineOption(QStringList() << "ffll" << "first-active-field-line" << "first_active_field_line",
                                            QCoreApplication::translate("main", "The first visible line of a field. Range 1-259 for NTSC (default: 20), 2-308 for PAL (default: 22)"),
                                            QCoreApplication::translate("main", "number"));
    parser.addOption(firstFieldLineOption);

    // Option to override calculated lastActiveFieldLine in our video parameters (-lfll)
    QCommandLineOption lastFieldLineOption(QStringList() << "lfll" << "last-active-field-line" << "last_active_field_line",
                                           QCoreApplication::translate("main", "The last visible line of a field. Range 1-259 for NTSC (default: 259), 2-308 for PAL (default: 308)"),
                                           QCoreApplication::translate("main", "number"));
    parser.addOption(lastFieldLineOption);

    // Option to override calculated firstActiveFrameLine in our video parameters (-ffrl)
    QCommandLineOption firstFrameLineOption(QStringList() << "ffrl" << "first-active-frame-line" << "first_active_frame_line",
                                            QCoreApplication::translate("main", "The first visible line of a frame. Range 1-525 for NTSC (default: 40), 1-620 for PAL (default: 44)"),
                                            QCoreApplication::translate("main", "number"));
    parser.addOption(firstFrameLineOption);

    // Option to override calculated lastActiveFieldLine in our video parameters (-lfll)
    QCommandLineOption lastFrameLineOption(QStringList() << "lfrl" << "last-active-frame-line" << "last_active_frame_line",
                                           QCoreApplication::translate("main", "The last visible line of a frame. Range 1-525 for NTSC (default: 525), 1-620 for PAL (default: 620)"),
                                           QCoreApplication::translate("main", "number"));
    parser.addOption(lastFrameLineOption);

    // -- NTSC decoder options --

    // Option to overlay the adaptive filter map
    QCommandLineOption showMapOption(QStringList() << "o" << "oftest",
                                     QCoreApplication::translate("main", "NTSC: Overlay the adaptive filter map (only used for testing)"));
    parser.addOption(showMapOption);

    // Option to set the chroma noise reduction level
    QCommandLineOption chromaNROption(QStringList() << "chroma-nr",
                                      QCoreApplication::translate("main", "NTSC: Chroma noise reduction level in IRE (default 0.0)"),
                                      QCoreApplication::translate("main", "number"));
    parser.addOption(chromaNROption);

    // Option to set the luma noise reduction level
    QCommandLineOption lumaNROption(QStringList() << "luma-nr",
                                    QCoreApplication::translate("main", "Luma noise reduction level in IRE (default 0.0)"),
                                    QCoreApplication::translate("main", "number"));
    parser.addOption(lumaNROption);

    // Option to use phase compensating decoder
    QCommandLineOption ntscPhaseCompOption(QStringList() << "ntsc-phase-comp",
                                           QCoreApplication::translate("main", "NTSC: Adjust phase per-line using burst phase"));
    parser.addOption(ntscPhaseCompOption);

    // Option to set the 3D adaptive filter threshold
    QCommandLineOption adaptThresholdOption(QStringList() << "adapt-threshold",
                                            QCoreApplication::translate("main", "NTSC: 3D adaptive filter threshold (default 1.0, higher = more 3D)"),
                                            QCoreApplication::translate("main", "number"));
    parser.addOption(adaptThresholdOption);

    // Option to set the chroma weight for 3D adaptive filter
    QCommandLineOption chromaWeightOption(QStringList() << "chroma-weight",
                                          QCoreApplication::translate("main", "NTSC: Chroma weight for 3D adaptive filter (default 1.0, higher = more 2D)"),
                                          QCoreApplication::translate("main", "number"));
    parser.addOption(chromaWeightOption);

    // -- PAL decoder options --

    // Option to use Simple PAL UV filter
    QCommandLineOption simplePALOption(QStringList() << "simple-pal",
                                           QCoreApplication::translate("main", "Transform: Use 1D UV filter (default 2D)"));
    parser.addOption(simplePALOption);

    // Option to select the Transform PAL threshold
    QCommandLineOption transformThresholdOption(QStringList() << "transform-threshold",
                                                QCoreApplication::translate("main", "Transform: Uniform similarity threshold (default 0.4)"),
                                                QCoreApplication::translate("main", "number"));
    parser.addOption(transformThresholdOption);

    // Option to select the Transform PAL thresholds file
    QCommandLineOption transformThresholdsOption(QStringList() << "transform-thresholds",
                                                 QCoreApplication::translate("main", "Transform: File containing per-bin similarity thresholds"),
                                                 QCoreApplication::translate("main", "file"));
    parser.addOption(transformThresholdsOption);

    // Option to overlay the FFTs
    QCommandLineOption showFFTsOption(QStringList() << "show-ffts",
                                      QCoreApplication::translate("main", "Transform: Overlay the input and output FFTs"));
    parser.addOption(showFFTsOption);

    // -- Positional arguments --

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC file (- for piped input)"));

    // Positional argument to specify output video file
    parser.addPositionalArgument("output", QCoreApplication::translate("main", "Specify output file (omit or - for piped output)"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return 2;

    // Get the arguments from the parser
    QString inputFileName;
    QString outputFileName = "-";
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 2) {
        inputFileName = positionalArguments.at(0);
        outputFileName = positionalArguments.at(1);
    } else if (positionalArguments.count() == 1) {
        inputFileName = positionalArguments.at(0);
    } else {
        // Quit with error
        qCritical("You must specify the input TBC and output files");
        return 2;
    }

    // Piped input is standard input or a named pipe. It can come without metadata, when
    // the video system is given (the levels and the rest have defaults, as decode-orc's
    // stream sources have), and then runs until the input ends.
    const bool isPipe = inputFileName == "-"
                        || (QFileInfo::exists(inputFileName) && !QFileInfo(inputFileName).isFile());
    // An LDX stream on the standard input says by itself what it holds (the video system, the metadata): it is what a pipe
    // is when no other format is asked for, no metadata is given and no video system is said
    QString inputFormat;
    if (!pipedOptions.format(parser, inputFormat)) return 2;
    const bool formatGiven = !inputFormat.isEmpty();
    // (with no format said and no metadata given, the way the input begins tells an LDX stream from raw video)
    const bool ldxInput = inputFileName == "-" && (formatGiven ? inputFormat == "ldx"
                                                               : (!metadataOptions.isInputFileSet() && LdxStream::stdinIsLdx()));
    if (ldxInput) {
        QString streamError;
        if (!LdxStream::openStdin(streamError)) {
            qCritical().noquote() << streamError;
            return 1;
        }
    }
    const bool hasNoMetadata = isPipe && !metadataOptions.isInputFileSet() && !ldxInput;

    StreamReader::Options streamOptions;
    bool encodingGiven = false;
    if (!inputFormat.isEmpty()) streamOptions.cvbs = inputFormat == "cvbs";
    if (pipedOptions.isSampleEncodingSet(parser)) {
        if (!pipedOptions.sampleEncoding(parser, streamOptions.encoding)) return 2;
        encodingGiven = true;
    }
    if (parser.isSet(bufferFramesOption)) {
        streamOptions.bufferFrames = parser.value(bufferFramesOption).toInt();
        if (streamOptions.bufferFrames < 1) {
            qCritical("The buffer must hold at least 1 frame");
            return 2;
        }
    }
    if (!isPipe && (pipedOptions.isFormatSet(parser) || parser.isSet(bufferFramesOption))) {
        qWarning() << "--input-format and --buffer-frames only apply to piped input";
    }
    if (inputFileName == outputFileName && outputFileName != "-") {
        // Quit with error
        qCritical("Input and output files cannot be the same");
        return 2;
    }

    qint32 startFrame = -1;
    qint32 length = -1;
    qint32 maxThreads = QThread::idealThreadCount();
    PalColour::Configuration palConfig;
    Comb::Configuration combConfig;
    MonoDecoder::MonoConfiguration monoConfig;
    OutputWriter::Configuration outputConfig;

    if (parser.isSet(startFrameOption)) {
        startFrame = parser.value(startFrameOption).toInt();

        if (startFrame < 1) {
            // Quit with error
            qCritical("Specified startFrame must be at least 1");
            return 2;
        }
    }

    if (parser.isSet(lengthOption)) {
        length = parser.value(lengthOption).toInt();

        if (length < 1) {
            // Quit with error
            qCritical("Specified length must be greater than zero frames");
            return 2;
        }
    }

    if (!threadsOption.process(parser, maxThreads)) return 2;

    if (parser.isSet(chromaGainOption)) {
        const double value = parser.value(chromaGainOption).toDouble();
        palConfig.chromaGain = value;
        combConfig.chromaGain = value;

        if (value < 0.0) {
            // Quit with error
            qCritical("Chroma gain must not be less than 0");
            return 2;
        }
    }

    if (parser.isSet(chromaPhaseOption)) {
        const double value = parser.value(chromaPhaseOption).toDouble();
        palConfig.chromaPhase = value;
        combConfig.chromaPhase = value;
    }

    bool bwMode = parser.isSet(setBwModeOption);
    if (bwMode) {
        palConfig.chromaGain = 0.0;
        combConfig.chromaGain = 0.0;
    }

    if (parser.isSet(showMapOption)) {
        combConfig.showMap = true;
    }

    if (parser.isSet(chromaNROption)) {
        combConfig.cNRLevel = parser.value(chromaNROption).toDouble();

        if (combConfig.cNRLevel < 0.0) {
            // Quit with error
            qCritical("Chroma noise reduction cannot be negative");
            return 2;
        }
    }

    if (parser.isSet(lumaNROption)) {
        combConfig.yNRLevel = parser.value(lumaNROption).toDouble();
        palConfig.yNRLevel = parser.value(lumaNROption).toDouble();
        monoConfig.yNRLevel = parser.value(lumaNROption).toDouble();

        if (combConfig.yNRLevel < 0.0) {
            // Quit with error
            qCritical("Luma noise reduction cannot be negative");
            return 2;
        }
    }

    if (parser.isSet(ntscPhaseCompOption)) {
        combConfig.phaseCompensation = true;
    }

    if (parser.isSet(adaptThresholdOption)) {
        combConfig.adaptThreshold = parser.value(adaptThresholdOption).toDouble();

        if (combConfig.adaptThreshold <= 0.0) {
            qCritical("Adapt threshold must be greater than 0");
            return 2;
        }
    }

    if (parser.isSet(chromaWeightOption)) {
        combConfig.chromaWeight = parser.value(chromaWeightOption).toDouble();

        if (combConfig.chromaWeight < 0.0) {
            qCritical("Chroma weight must be greater than or equal to 0");
            return 2;
        }
    }

    if (parser.isSet(simplePALOption)) {
        palConfig.simplePAL = true;
    }

    if (parser.isSet(transformThresholdOption)) {
        palConfig.transformThreshold = parser.value(transformThresholdOption).toDouble();

        if (palConfig.transformThreshold < 0.0 || palConfig.transformThreshold > 1.0) {
            // Quit with error
            qCritical("Transform threshold must be between 0 and 1");
            return 2;
        }
    }

    LdDecodeMetaData::LineParameters lineParameters;
    if (parser.isSet(firstFieldLineOption)) {
        lineParameters.firstActiveFieldLine = parser.value(firstFieldLineOption).toInt();
    }
    
    if (parser.isSet(lastFieldLineOption)) {
        lineParameters.lastActiveFieldLine = parser.value(lastFieldLineOption).toInt();
    }
    
    if (parser.isSet(firstFrameLineOption)) {
        lineParameters.firstActiveFrameLine = parser.value(firstFrameLineOption).toInt();
    }
    
    if (parser.isSet(lastFrameLineOption)) {
        lineParameters.lastActiveFrameLine = parser.value(lastFrameLineOption).toInt();
    }

    // Work out the metadata filename
    LdDecodeMetaData metaData;
    if (ldxInput) {
        // The metadata the stream began with, or, if it sent none, the defaults of the video it holds
        LdxStream *ldxStream = LdxStream::instance();
        QString metadataError;
        if (ldxStream->readMetadata(metaData, metadataError)) {
            qInfo().noquote() << "The LDX stream holds" << (ldxStream->isCvbs() ? "CVBS" : "TBC") << "video, named" << ldxStream->videoName() << "- its metadata are used";
        } else {
            metaData.initialiseDefault(ldxStream->system(), -1, -1, ldxStream->isCvbs());
            qInfo().noquote() << "The LDX stream sent no metadata: assuming" << Cvbs::presetName(ldxStream->system())
                              << "video at the standard levels that runs until the input ends";
        }
        // Metadata without fields is the start of metadata that come field by field with the video
        if (metaData.getNumberOfFields() == 0) metaData.makeLive();
        ldxStream->provideFields(metaData);
        streamOptions.cvbs = ldxStream->isCvbs();
    } else if (hasNoMetadata) {
        // Piped input with no metadata: the video system must be said, the rest has defaults
        if (!pipedOptions.isSystemSet(parser)) {
            qCritical("Piped input without metadata needs the video system (--system pal, ntsc or pal-m), "
                      "or the input metadata file (--input-metadata)");
            return 2;
        }
        VideoSystem system;
        if (!pipedOptions.videoSystem(parser, system)) return 2;

        qint32 blackLevel = -1;
        qint32 whiteLevel = -1;
        if (!pipedOptions.levels(parser, streamOptions.cvbs, blackLevel, whiteLevel)) return 2;

        // ld-decode 8 and decode-orc mark the CVBS they write as locked
        bool isSubcarrierLocked;
        if (!pipedOptions.subcarrierLocked(parser, streamOptions.cvbs, isSubcarrierLocked)) return 2;

        metaData.initialiseDefault(system, blackLevel, whiteLevel, isSubcarrierLocked);
        qInfo().noquote() << "No metadata: assuming" << Cvbs::presetName(system) << "video at the standard levels"
                          << (blackLevel >= 0 || whiteLevel > 0 ? "(except those given)" : "")
                          << "that runs until the input ends";
    } else {
        QString inputMetadataFileName;
        if (!metadataOptions.selectInput(inputFileName, true, inputMetadataFileName)) return 1;

        // Load the source video metadata
        if (!metaData.read(inputMetadataFileName)) {
            qCritical() << "Unable to open ld-decode metadata file:" << inputMetadataFileName;
            return 1;
        }

        // CVBS core metadata says how its samples are stored
        if (metaData.isCvbs() && !encodingGiven) streamOptions.encoding = metaData.getCvbsFileMetadata().sampleEncoding;
        if (pipedOptions.isSystemSet(parser) || pipedOptions.isLevelSet(parser)) {
            qWarning() << "--system, --black-level and --white-level are for input without metadata: the metadata is used";
        }
    }
    streamOptions.system = metaData.getVideoParameters().system;

    metaData.processLineParameters(lineParameters);
    
    // Reverse field order if required
    if (setReverseOption.isSet(parser)) {
        qInfo() << "Expected field order is reversed to second field/first field";
        metaData.setIsFirstFieldFirst(false);
    }

    // Work out which decoder to use
    QString decoderName;
    if (parser.isSet(decoderOption)) {
        decoderName = parser.value(decoderOption);
    } else {
        // Check if video parameters are valid before accessing them
        try {
            if (metaData.getVideoParameters().system == NTSC) {
                decoderName = "ntsc2d";
            } else {
                decoderName = "pal2d";
            }
        } catch (const std::exception& e) {
            qCritical() << "Failed to access video parameters from metadata file:" << e.what();
            return 1;
        }
    }

    // Require ntsc3d if the map overlay is selected
    if (combConfig.showMap && decoderName != "ntsc3d") {
        qCritical() << "Can only show adaptive filter map with the ntsc3d decoder";
        return 2;
    }

    // Require transform2d/3d if the FFT overlay is selected
    if (palConfig.showFFTs && decoderName != "transform2d" && decoderName != "transform3d") {
        qCritical() << "Can only show FFTs with the transform2d/transform3d decoders";
        return 2;
    }

    // Select the decoder
    std::unique_ptr<Decoder> decoder;
    if (decoderName == "pal2d") {
        decoder = std::make_unique<PalDecoder>(palConfig);
    } else if (decoderName == "transform2d") {
        palConfig.chromaFilter = PalColour::transform2DFilter;
        if (!loadTransformThresholds(parser, transformThresholdsOption, palConfig)) {
            return 1;
        }
        decoder = std::make_unique<PalDecoder>(palConfig);
    } else if (decoderName == "transform3d") {
        palConfig.chromaFilter = PalColour::transform3DFilter;
        if (!loadTransformThresholds(parser, transformThresholdsOption, palConfig)) {
            return 1;
        }
        decoder = std::make_unique<PalDecoder>(palConfig);
    } else if (decoderName == "ntsc1d") {
        combConfig.dimensions = 1;
        decoder = std::make_unique<NtscDecoder>(combConfig);
    } else if (decoderName == "ntsc2d") {
        combConfig.dimensions = 2;
        decoder = std::make_unique<NtscDecoder>(combConfig);
    } else if (decoderName == "ntsc3d") {
        combConfig.dimensions = 3;
        decoder = std::make_unique<NtscDecoder>(combConfig);
    } else if (decoderName == "ntsc3dnoadapt") {
        combConfig.dimensions = 3;
        combConfig.adaptive = false;
        decoder = std::make_unique<NtscDecoder>(combConfig);
    } else if (decoderName == "mono") {
        decoder = std::make_unique<MonoDecoder>(monoConfig);
    } else {
        qCritical() << "Unknown decoder" << decoderName;
        return 2;
    }

    // Select the output format
    QString outputFormatName;
    if (parser.isSet(outputFormatOption)) {
        outputFormatName = parser.value(outputFormatOption);
    } else {
        outputFormatName = "rgb";
    }
    if (outputFormatName == "yuv" || outputFormatName == "y4m") {
        if (outputFormatName == "y4m") {
            outputConfig.outputY4m = true;
        }
        if (bwMode || decoderName == "mono") {
            outputConfig.pixelFormat = OutputWriter::PixelFormat::GRAY16;
        } else {
            outputConfig.pixelFormat = OutputWriter::PixelFormat::YUV444P16;
        }
    } else if (outputFormatName == "rgb") {
        outputConfig.pixelFormat = OutputWriter::PixelFormat::RGB48;
    } else {
        qCritical() << "Unknown output format" << outputFormatName;
        return 2;
    }

    if (parser.isSet(outputPaddingOption)) {
        outputConfig.paddingAmount = parser.value(outputPaddingOption).toInt();
        if (outputConfig.paddingAmount < 1 || outputConfig.paddingAmount > 32) {
            qInfo() << "Invalid value" << outputConfig.paddingAmount << "specified for padding amount, defaulting to 8.";
            outputConfig.paddingAmount = 8;
        }
    }
    
    // Perform the processing
    DecoderPool decoderPool(*decoder, inputFileName, metaData, outputConfig, outputFileName, startFrame, length, maxThreads);
    decoderPool.setStreamOptions(streamOptions);
    if (!decoderPool.process()) {
        return 1;
    }

    // Quit with success
    return 0;
}
