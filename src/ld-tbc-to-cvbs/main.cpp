/******************************************************************************
 * main.cpp
 * ld-tbc-to-cvbs - TBC to CVBS converter
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
#include <algorithm>
#include <cmath>

#include "tbc/logging.h"
#include "tbc/metadataoptions.h"
#include "cvbs.h"
#include "lddecodemetadata.h"
#include "sourcevideo.h"
#include "targetvideo.h"

namespace {

// The chroma file of a Y/C TBC capture: <name>_chroma.tbc beside <name>.tbc
// (ld-decode and vhs-decode), or <name>.tbcc beside <name>.tbcy (decode-orc)
QString chromaFileFor(const QString &inputFileName)
{
    QString chromaFileName;
    if (inputFileName.endsWith(".tbcy", Qt::CaseInsensitive)) {
        chromaFileName = inputFileName.left(inputFileName.length() - 1) + "c";
    } else if (inputFileName.endsWith(".tbc", Qt::CaseInsensitive)) {
        chromaFileName = inputFileName.left(inputFileName.length() - 4) + "_chroma.tbc";
    }
    return QFileInfo::exists(chromaFileName) ? chromaFileName : QString();
}

// The input file name without its .tbc / .tbcy extension
QString inputBaseName(const QString &inputFileName)
{
    for (const char *extension: {".tbcy", ".tbc"}) {
        if (inputFileName.endsWith(extension, Qt::CaseInsensitive)) {
            return inputFileName.left(inputFileName.length() - static_cast<int>(qstrlen(extension)));
        }
    }
    return inputFileName;
}

// The blanking level of a TBC capture. Metadata from before blanking16bIre
// existed has only the black level, which for NTSC is normally on the 7.5 IRE
// setup pedestal; NTSC-J has no setup, so black is blanking. Pick whichever
// reading lands closer to the nominal blanking level, as decode-orc does.
qint32 tbcBlankingLevel(const LdDecodeMetaData::VideoParameters &videoParameters)
{
    const qint32 black = videoParameters.black16bIre;
    const qint32 white = videoParameters.white16bIre;
    if (videoParameters.blanking16bIre > 0 && videoParameters.blanking16bIre != black) {
        return videoParameters.blanking16bIre;
    }
    if (videoParameters.system == PAL) return black;

    const double setupIre = 7.5;
    const qint32 blankingWithSetup = static_cast<qint32>(std::lround(black - setupIre * (white - black) / (100.0 - setupIre)));
    const qint32 nominalBlanking = Cvbs::standardLevels(videoParameters.system).blanking * 64;
    if (videoParameters.system == NTSC && std::abs(black - nominalBlanking) < std::abs(blankingWithSetup - nominalBlanking)) {
        return black;
    }
    return blankingWithSetup;
}

// Linear map of TBC sample values onto the standard levels of the video system
class LevelMap
{
public:
    LevelMap(qint32 fromBlanking, qint32 fromWhite, qint32 toBlanking, qint32 toWhite)
        : scale(static_cast<double>(toWhite - toBlanking) / (fromWhite - fromBlanking)),
          offset(toBlanking - fromBlanking * scale),
          identity(fromBlanking == toBlanking && fromWhite == toWhite)
    {
    }

    bool isIdentity() const { return identity; }

    qint32 level(qint32 value) const { return static_cast<qint32>(std::lround(value * scale + offset)); }

    // Apply the map as the samples are stored, so nothing is clipped on the
    // way. Chroma is centred on zero (32768): only the gain applies to it.
    void applyTo(TargetVideo &targetVideo, bool isChroma) const
    {
        if (identity) return;
        targetVideo.setLevelMap(scale, isChroma ? 32768.0 * (1.0 - scale) : offset);
    }

private:
    double scale;
    double offset;
    bool identity;
};

} // namespace

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-tbc-to-cvbs");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-tbc-to-cvbs - TBC to CVBS converter for ld-decode\n"
                "\n"
                "Converts <name>.tbc and its metadata to the CVBS file format read by\n"
                "decode-orc: <name>.cvbs, <name>.meta and <name>.dropouts.meta, plus\n"
                "<name>.cvbs.db (or .cvbs.json) keeping the field metadata. A Y/C capture\n"
                "(<name>.tbc with <name>_chroma.tbc) becomes <name>.cvbsy and <name>.cvbsc.\n"
                "Use ld-cvbs-to-tbc for the opposite direction.\n"
                "\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Metadata options: --meta and --input-metadata
    MetadataOptions metadataOptions(MetadataOptions::InputFile);
    metadataOptions.addTo(parser);

    // Option to choose the sample encoding
    QCommandLineOption encodingOption(QStringList() << "e" << "encoding",
                                      QCoreApplication::translate("main", "Sample encoding: u10 (CVBS_U10_4FSC, default), u16, tpg21 or s16"),
                                      QCoreApplication::translate("main", "encoding"));
    parser.addOption(encodingOption);

    // Positional arguments
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC file"));
    parser.addPositionalArgument("output", QCoreApplication::translate("main", "Specify output CVBS file (default input.cvbs)"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return -1;

    const QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() < 1 || positionalArguments.count() > 2) {
        qCritical("You must specify an input TBC file, and optionally the output CVBS file");
        return -1;
    }
    const QString inputFileName = positionalArguments.at(0);
    if (Cvbs::isDataFileName(inputFileName)) {
        qCritical().noquote() << inputFileName << "is already CVBS";
        return -1;
    }
    if (!QFileInfo::exists(inputFileName)) {
        qCritical().noquote() << "Input file" << inputFileName << "does not exist";
        return -1;
    }

    // Sample encoding
    Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
    if (parser.isSet(encodingOption)) {
        const QString name = parser.value(encodingOption);
        if (!Cvbs::parseSampleEncodingName(name, encoding)
            && !Cvbs::parseSampleEncodingName("CVBS_" + name + "_4FSC", encoding)) {
            qCritical().noquote() << "Unknown sample encoding" << name << "- use u10, u16, tpg21 or s16";
            return -1;
        }
    }

    // Composite or Y/C
    const QString chromaFileName = chromaFileFor(inputFileName);
    const bool isYc = !chromaFileName.isEmpty();

    // Output file names
    QString outputBase = inputBaseName(inputFileName);
    if (positionalArguments.count() == 2) {
        const QString outputFileName = positionalArguments.at(1);
        if (!Cvbs::isDataFileName(outputFileName)) {
            qCritical().noquote() << "Output file" << outputFileName << "must have a .cvbs extension (.cvbsy for Y/C)";
            return -1;
        }
        outputBase = Cvbs::baseName(outputFileName);
    }
    const QString outputFileName = Cvbs::dataFileName(outputBase, isYc ? Cvbs::Channel::Luma : Cvbs::Channel::Composite);
    const QString outputChromaFileName = Cvbs::dataFileName(outputBase, Cvbs::Channel::Chroma);

    // Read the TBC metadata
    QString inputMetadataFileName;
    if (!metadataOptions.selectInput(inputFileName, true, inputMetadataFileName)) return -1;
    LdDecodeMetaData inputMetaData;
    if (!inputMetaData.read(inputMetadataFileName)) {
        qCritical().noquote() << "Unable to read the TBC metadata" << inputMetadataFileName;
        return 1;
    }

    LdDecodeMetaData::VideoParameters videoParameters = inputMetaData.getVideoParameters();
    const VideoSystem system = videoParameters.system;
    const Cvbs::Geometry geometry = Cvbs::geometry(system);
    if (videoParameters.fieldWidth != geometry.lineWidth || videoParameters.fieldHeight != geometry.fieldHeight()) {
        qCritical().noquote() << "The TBC fields are" << videoParameters.fieldWidth << "x" << videoParameters.fieldHeight
                              << "- CVBS" << Cvbs::presetName(system) << "needs" << geometry.lineWidth << "x"
                              << geometry.fieldHeight() << "(4fsc sampling)";
        return 1;
    }

    // Never overwrite an existing capture
    const bool jsonCompanion = inputMetaData.getFormat() == LdDecodeMetaData::MetadataFormat::Json;
    QStringList outputFiles = {outputFileName, Cvbs::metaFileName(outputFileName), Cvbs::dropoutsFileName(outputFileName),
                               Cvbs::companionFileName(outputFileName, jsonCompanion)};
    if (isYc) outputFiles.append(outputChromaFileName);
    for (const QString &fileName: outputFiles) {
        if (QFileInfo::exists(fileName)) {
            qCritical().noquote() << "Output file" << fileName << "already exists - will not overwrite it";
            return 1;
        }
    }

    // CVBS samples are at the standard levels: map the TBC levels onto them
    const Cvbs::Levels standard = Cvbs::standardLevels(system);
    const LevelMap levelMap(tbcBlankingLevel(videoParameters), videoParameters.white16bIre,
                            standard.blanking * 64, standard.white * 64);
    if (!levelMap.isIdentity()) {
        qInfo() << "Mapping the TBC levels (blanking" << tbcBlankingLevel(videoParameters) << "white"
                << videoParameters.white16bIre << ") onto the standard ones";
    }
    videoParameters.black16bIre = levelMap.level(videoParameters.black16bIre);
    videoParameters.blanking16bIre = standard.blanking * 64;
    videoParameters.white16bIre = standard.white * 64;

    // Open the video files
    SourceVideo sourceVideo;
    SourceVideo chromaSourceVideo;
    if (!sourceVideo.open(inputFileName, geometry.fieldLength())) return 1;
    if (isYc && !chromaSourceVideo.open(chromaFileName, geometry.fieldLength())) return 1;
    TargetVideo targetVideo;
    TargetVideo chromaTargetVideo;
    if (!targetVideo.open(outputFileName, system, encoding)) return 1;
    if (isYc && !chromaTargetVideo.open(outputChromaFileName, system, encoding)) return 1;
    levelMap.applyTo(targetVideo, false);
    if (isYc) levelMap.applyTo(chromaTargetVideo, true);

    // CVBS frames are the first field, then the second: pair the TBC fields
    // that way, leaving out any field without a partner
    LdDecodeMetaData outputMetaData;
    outputMetaData.setFormat(inputMetaData.getFormat());
    if (inputMetaData.hasPcmAudioParameters()) outputMetaData.setPcmAudioParameters(inputMetaData.getPcmAudioParameters());

    const qint32 numberOfFields = std::min(inputMetaData.getNumberOfFields(), sourceVideo.getNumberOfAvailableFields());
    qint32 numberOfFrames = 0;
    for (qint32 frame = 1; ; frame++) {
        if (frame * 2 > numberOfFields) break;
        const qint32 firstField = inputMetaData.getFirstFieldNumber(frame);
        const qint32 secondField = inputMetaData.getSecondFieldNumber(frame);
        if (firstField < 1 || secondField < 1 || firstField > numberOfFields || secondField > numberOfFields) break;

        for (qint32 fieldNumber: {firstField, secondField}) {
            if (!targetVideo.writeField(sourceVideo.getVideoField(fieldNumber))) {
                qCritical() << "Writing the CVBS output failed";
                return 1;
            }
            if (isYc) {
                if (!chromaTargetVideo.writeField(chromaSourceVideo.getVideoField(fieldNumber))) {
                    qCritical() << "Writing the CVBS chroma output failed";
                    return 1;
                }
            }
            outputMetaData.appendField(inputMetaData.getField(fieldNumber));
        }
        numberOfFrames++;
    }
    targetVideo.close();
    if (isYc) chromaTargetVideo.close();

    if (numberOfFrames * 2 != numberOfFields) {
        qInfo() << "Left out" << numberOfFields - numberOfFrames * 2 << "unpaired field(s): CVBS files hold whole frames";
    }

    // Metadata: the companion, <name>.meta and <name>.dropouts.meta
    videoParameters.numberOfSequentialFields = numberOfFrames * 2;
    outputMetaData.setVideoParameters(videoParameters);

    Cvbs::FileMetadata fileMetadata;
    fileMetadata.system = system;
    fileMetadata.sampleEncoding = encoding;
    fileMetadata.signalStatePreset = videoParameters.isSubcarrierLocked ? "STANDARD_STABLE_LOCKED" : "STANDARD_STABLE_UNLOCKED";
    fileMetadata.signalType = isYc ? "yc" : "composite";
    fileMetadata.gitBranch = videoParameters.gitBranch;
    fileMetadata.gitCommit = videoParameters.gitCommit;
    fileMetadata.captureNotes = videoParameters.tapeFormat;
    outputMetaData.setCvbsFileMetadata(fileMetadata);

    if (!outputMetaData.write(Cvbs::companionFileName(outputFileName, jsonCompanion))) {
        qCritical() << "Writing the CVBS metadata failed";
        return 1;
    }

    // The CVBS format carries audio as 48 kHz WAV channel pairs
    const QString pcmFileName = inputBaseName(inputFileName) + ".pcm";
    if (QFileInfo::exists(pcmFileName)) {
        qInfo().noquote() << "Note:" << pcmFileName << "was not converted - CVBS audio is 48 kHz WAV, not converted yet";
    }

    qInfo().noquote() << "Wrote" << numberOfFrames << "frames to" << outputFileName
                      << (isYc ? "and " + outputChromaFileName : QString()) << "as" << Cvbs::sampleEncodingName(encoding);
    return 0;
}
