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
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>

#include "tbc/logging.h"
#include "tbc/metadataoptions.h"
#include "commonoptions.h"
#include "cvbs.h"
#include "cvbsaudio.h"
#include "fieldaudio.h"
#include "lddecodemetadata.h"
#include "mappedmetadata.h"
#include "sourcevideo.h"
#include "targetvideo.h"
#include "tbcfiles.h"

namespace {

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

    // A capture that keeps a map in its metadata is converted in the order of the map (the frames of the disc, the missing ones blank)
    IgnoreMapOption ignoreMapOption;
    ignoreMapOption.addTo(parser);

    // Option to choose the sample encoding
    QCommandLineOption encodingOption(QStringList() << "e" << "encoding",
                                      QCoreApplication::translate("main", "Sample encoding: u10 (CVBS_U10_4FSC, default), u16, tpg21 or s16"),
                                      QCoreApplication::translate("main", "encoding"));
    parser.addOption(encodingOption);

    // Option to declare the sample rate of the audio
    QCommandLineOption sampleRateOption(QStringList() << "r" << "samplerate",
                                        QCoreApplication::translate("main", "Sample rate of the audio in Hz (default: from the metadata, else 44100). The audio is copied as it is, not resampled"),
                                        QCoreApplication::translate("main", "Hz"));
    parser.addOption(sampleRateOption);

    // Positional arguments
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC file"));
    parser.addPositionalArgument("output", QCoreApplication::translate("main", "Specify output CVBS file (default input.cvbs)"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return 2;

    const QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() < 1 || positionalArguments.count() > 2) {
        qCritical("You must specify an input TBC file, and optionally the output CVBS file");
        return 2;
    }
    QString inputFileName = positionalArguments.at(0);
    if (Cvbs::isDataFileName(inputFileName)) {
        qCritical().noquote() << inputFileName << "is already CVBS";
        return 2;
    }
    if (!QFileInfo::exists(inputFileName)) {
        qCritical().noquote() << "Input file" << inputFileName << "does not exist";
        return 1;
    }

    // Either file of a Y/C pair (_chroma.tbc, .tbcc) names the pair: start from the luma
    if (TbcFiles::isChromaFile(inputFileName)) {
        const QString lumaFileName = TbcFiles::lumaFileName(inputFileName);
        if (!QFileInfo::exists(lumaFileName)) {
            qCritical().noquote() << "The luma file" << lumaFileName << "of the Y/C pair does not exist";
            return 1;
        }
        inputFileName = lumaFileName;
    }

    // Sample encoding
    Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC;
    if (parser.isSet(encodingOption)) {
        const QString name = parser.value(encodingOption);
        if (!Cvbs::parseSampleEncodingName(name, encoding)
            && !Cvbs::parseSampleEncodingName("CVBS_" + name + "_4FSC", encoding)) {
            qCritical().noquote() << "Unknown sample encoding" << name << "- use u10, u16, tpg21 or s16";
            return 2;
        }
    }

    // Sample rate the audio is declared to have
    quint32 sampleRate = 0;
    if (parser.isSet(sampleRateOption)) {
        bool ok;
        sampleRate = parser.value(sampleRateOption).toUInt(&ok);
        if (!ok || sampleRate < 1000 || sampleRate > 384000) {
            qCritical().noquote() << "Invalid sample rate" << parser.value(sampleRateOption);
            return 2;
        }
    }

    // Composite, or Y/C: <name>_chroma.tbc beside <name>.tbc (ld-decode, vhs-decode),
    // or <name>.tbcc beside <name>.tbcy (decode-orc)
    const QString chromaFileName = TbcFiles::chromaFileName(inputFileName);
    const bool isYc = !chromaFileName.isEmpty();

    // Output file names
    QString outputBase = TbcFiles::baseName(inputFileName);
    if (positionalArguments.count() == 2) {
        const QString outputFileName = positionalArguments.at(1);
        if (!Cvbs::isDataFileName(outputFileName)) {
            qCritical().noquote() << "Output file" << outputFileName << "must have a .cvbs extension (.cvbsy for Y/C)";
            return 1;
        }
        outputBase = Cvbs::baseName(outputFileName);
    }
    const QString outputFileName = Cvbs::dataFileName(outputBase, isYc ? Cvbs::Channel::Luma : Cvbs::Channel::Composite);
    const QString outputChromaFileName = Cvbs::dataFileName(outputBase, Cvbs::Channel::Chroma);

    // Read the TBC metadata
    QString inputMetadataFileName;
    if (!metadataOptions.selectInput(inputFileName, true, inputMetadataFileName)) return 1;
    LdDecodeMetaData inputMetaData;
    if (!inputMetaData.read(inputMetadataFileName)) {
        qCritical().noquote() << "Unable to read the TBC metadata" << inputMetadataFileName;
        return 1;
    }

    // The capture seen through the map its metadata keep: its fields (and those of the chroma file of a pair) are read in that order, and the metadata and the
    // audio are those of the view
    QVector<FieldAudio::Range> audioRanges;
    std::shared_ptr<const QVector<qint32>> viewTable;
    if (!ignoreMapOption.isSet(parser)) {
        QString mapError;
        viewTable = MappedMetadata::applyEmbeddedMap(inputMetaData, inputMetadataFileName, inputFileName, mapError, false, &audioRanges);
        if (!mapError.isEmpty()) {
            qCritical().noquote() << "Unable to use the map in the metadata:" << mapError << "(--ignore-map reads the capture as it is)";
            return 1;
        }
        if (viewTable) qInfo() << "The capture is read through the map in its metadata (--ignore-map reads it as it is)";
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
    if (!(viewTable ? sourceVideo.openMappedFields(inputFileName, geometry.fieldLength(), -1, viewTable) : sourceVideo.open(inputFileName, geometry.fieldLength()))) return 1;
    if (isYc && !(viewTable ? chromaSourceVideo.openMappedFields(chromaFileName, geometry.fieldLength(), -1, viewTable) : chromaSourceVideo.open(chromaFileName, geometry.fieldLength()))) return 1;
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

    // The audio: the CVBS format carries it as a 24-bit WAV channel pair. The
    // samples are copied as they are, declared to be at the sample rate asked
    // for, else the one in the metadata, else ld-decode's 44.1 kHz.
    const QString pcmFileName = TbcFiles::baseName(inputFileName) + ".pcm";
    if (QFileInfo::exists(pcmFileName)) {
        const bool hasParameters = inputMetaData.hasPcmAudioParameters();
        if (hasParameters && (inputMetaData.getPcmAudioParameters().bits != 16
                              || !inputMetaData.getPcmAudioParameters().isSigned
                              || !inputMetaData.getPcmAudioParameters().isLittleEndian)) {
            qWarning().noquote() << pcmFileName << "is not 16-bit signed little-endian audio - not converted";
        } else {
            if (sampleRate == 0) {
                sampleRate = hasParameters ? static_cast<quint32>(inputMetaData.getPcmAudioParameters().sampleRate)
                                           : CvbsAudio::TBC_SAMPLE_RATE;
            }
            // All of the audio is kept, even any that belongs to fields left out; for a capture read through its map it is the audio of the fields of the
            // view, in their order (silence for a padded field), put in a file on the way
            QString audioFileName = pcmFileName;
            QTemporaryFile viewAudio;
            if (viewTable) {
                FieldAudio fieldAudio;
                if (fieldAudio.open(inputFileName, audioRanges) && viewAudio.open()) {
                    for (qint32 fieldNumber = 1; fieldNumber <= audioRanges.size(); fieldNumber++) viewAudio.write(fieldAudio.audioOf(fieldNumber));
                    viewAudio.flush();
                    audioFileName = viewAudio.fileName();
                } else {
                    qWarning().noquote() << "The audio of" << pcmFileName << "could not be put in the order of the map: it is copied as it is";
                }
            }
            const qint64 audioSamples = QFileInfo(audioFileName).size() / 4;

            const QString wavFileName = CvbsAudio::wavFileName(outputBase, 0);
            if (QFileInfo::exists(wavFileName)) {
                qCritical().noquote() << "Output file" << wavFileName << "already exists - the audio was not converted";
            } else {
                const qint64 written = CvbsAudio::pcmToWav(audioFileName, 0, audioSamples, wavFileName, sampleRate);
                if (written < 0 || !Cvbs::setAudioChannelPair(Cvbs::metaFileName(outputFileName), 0, "Analogue audio")) {
                    qCritical() << "Writing the CVBS audio failed";
                    return 1;
                }
                qInfo().noquote() << "Wrote" << written << "audio samples to" << wavFileName << "declared as" << sampleRate
                                  << "Hz (not resampled)";
                if (sampleRate != CvbsAudio::CVBS_SAMPLE_RATE) {
                    qWarning().noquote() << "decode-orc only accepts CVBS audio at" << CvbsAudio::CVBS_SAMPLE_RATE
                                         << "Hz and refuses the whole capture otherwise; use --samplerate"
                                         << CvbsAudio::CVBS_SAMPLE_RATE << "to declare that (the audio is not resampled,"
                                         << "so it then plays faster)";
                }
            }
        }
    }

    qInfo().noquote() << "Wrote" << numberOfFrames << "frames to" << outputFileName
                      << (isYc ? "and " + outputChromaFileName : QString()) << "as" << Cvbs::sampleEncodingName(encoding);
    return 0;
}
