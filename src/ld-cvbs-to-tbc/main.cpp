/******************************************************************************
 * main.cpp
 * ld-cvbs-to-tbc - CVBS to TBC converter
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
#include "tbc/metadataoptions.h"
#include "cvbs.h"
#include "cvbsaudio.h"
#include "lddecodemetadata.h"
#include "sourcevideo.h"
#include "targetvideo.h"
#include "tbcfiles.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-cvbs-to-tbc");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "ld-cvbs-to-tbc - CVBS to TBC converter for ld-decode\n"
                "\n"
                "Converts a CVBS capture (<name>.cvbs, as written by ld-decode or\n"
                "decode-orc) to <name>.tbc with <name>.tbc.db metadata (or .tbc.json).\n"
                "A Y/C pair (<name>.cvbsy and <name>.cvbsc) becomes <name>.tbc and\n"
                "<name>_chroma.tbc, or <name>.tbcy and <name>.tbcc (as decode-orc names\n"
                "them) if the output file is named <name>.tbcy.\n"
                "Use ld-tbc-to-cvbs for the opposite direction.\n"
                "\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Metadata options: --meta picks the output format (the companion's, by default)
    MetadataOptions metadataOptions(MetadataOptions::FormatOnly | MetadataOptions::CreatesMetadata);
    metadataOptions.addTo(parser);

    // Option to declare the sample rate of the audio
    QCommandLineOption sampleRateOption(QStringList() << "r" << "samplerate",
                                        QCoreApplication::translate("main", "Sample rate of the audio in Hz (default: that of the WAV file, normally 48000). The audio is copied as it is, not resampled"),
                                        QCoreApplication::translate("main", "Hz"));
    parser.addOption(sampleRateOption);

    // Positional arguments
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input CVBS file (.cvbs, or .cvbsy for Y/C)"));
    parser.addPositionalArgument("output", QCoreApplication::translate("main", "Specify output TBC file (default input.tbc)"));

    // Process the command line options and arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);
    if (!metadataOptions.process(parser)) return 2;

    const QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() < 1 || positionalArguments.count() > 2) {
        qCritical("You must specify an input CVBS file, and optionally the output TBC file");
        return 2;
    }

    QString inputFileName = positionalArguments.at(0);
    if (!Cvbs::isDataFileName(inputFileName)) {
        qCritical().noquote() << inputFileName << "is not a CVBS file (.cvbs, .cvbsy or .cvbsc)";
        return 2;
    }
    // Either file of a Y/C pair names the pair: start from the luma
    if (Cvbs::channel(inputFileName) == Cvbs::Channel::Chroma) inputFileName = Cvbs::pairedDataFileName(inputFileName);
    if (!QFileInfo::exists(inputFileName)) {
        qCritical().noquote() << "Input file" << inputFileName << "does not exist";
        return 1;
    }
    const bool isYc = Cvbs::channel(inputFileName) == Cvbs::Channel::Luma;
    const QString chromaFileName = isYc ? Cvbs::pairedDataFileName(inputFileName) : QString();
    if (isYc && !QFileInfo::exists(chromaFileName)) {
        qCritical().noquote() << "The chroma file" << chromaFileName << "of the Y/C pair does not exist";
        return 1;
    }

    quint32 sampleRate = 0;
    if (parser.isSet(sampleRateOption)) {
        bool ok;
        sampleRate = parser.value(sampleRateOption).toUInt(&ok);
        if (!ok || sampleRate < 1000 || sampleRate > 384000) {
            qCritical().noquote() << "Invalid sample rate" << parser.value(sampleRateOption);
            return 2;
        }
    }

    // Output file names
    QString outputFileName = Cvbs::baseName(inputFileName) + ".tbc";
    if (positionalArguments.count() == 2) outputFileName = positionalArguments.at(1);
    if (Cvbs::isDataFileName(outputFileName)) {
        qCritical().noquote() << "Output file" << outputFileName << "is a CVBS file name - give a .tbc file name";
        return 1;
    }
    // A Y/C capture is named <name>.tbc and <name>_chroma.tbc, as ld-decode and vhs-decode
    // do; or <name>.tbcy and <name>.tbcc, as decode-orc does, if the output is named so
    if (outputFileName.endsWith(".tbcc", Qt::CaseInsensitive)) outputFileName = TbcFiles::lumaFileName(outputFileName);
    const QString outputChromaFileName = TbcFiles::isOrcYcFile(outputFileName)
            ? TbcFiles::baseName(outputFileName) + ".tbcc"
            : TbcFiles::baseName(outputFileName) + "_chroma.tbc";

    // Read the metadata: the companion if there is one, otherwise <name>.meta
    const QString inputMetadataFileName = LdDecodeMetaData::findMetadataFile(inputFileName);
    if (!QFileInfo::exists(inputMetadataFileName)) {
        qCritical().noquote() << inputFileName << "has no metadata (" + Cvbs::metaFileName(inputFileName) + ")";
        return 1;
    }
    LdDecodeMetaData metaData;
    if (!metaData.read(inputMetadataFileName)) {
        qCritical().noquote() << "Unable to read the CVBS metadata" << inputMetadataFileName;
        return 1;
    }

    // The TBC metadata format: --meta, or that of the companion (SQLite if none)
    const LdDecodeMetaData::MetadataFormat outputFormat = metadataOptions.isFormatSet() ? metadataOptions.format()
                                                                                       : metaData.getFormat();
    const QString outputMetadataFileName = LdDecodeMetaData::metadataFileName(outputFileName, outputFormat);

    const QString outputBase = TbcFiles::baseName(outputFileName);
    const QString wavFileName = CvbsAudio::wavFileName(Cvbs::baseName(inputFileName), 0);
    const QString outputPcmFileName = outputBase + ".pcm";
    const bool hasAudio = QFileInfo::exists(wavFileName);

    QStringList outputFiles = {outputFileName, outputMetadataFileName};
    if (hasAudio) outputFiles.append(outputPcmFileName);
    if (isYc) outputFiles.append(outputChromaFileName);
    for (const QString &fileName: outputFiles) {
        if (QFileInfo::exists(fileName)) {
            qCritical().noquote() << "Output file" << fileName << "already exists - will not overwrite it";
            return 1;
        }
    }

    // Copy the fields across: the library reads CVBS frames as TBC fields
    const LdDecodeMetaData::VideoParameters &videoParameters = metaData.getVideoParameters();
    const qint32 fieldLength = videoParameters.fieldWidth * videoParameters.fieldHeight;
    SourceVideo sourceVideo;
    SourceVideo chromaSourceVideo;
    if (!sourceVideo.open(inputFileName, fieldLength)) return 1;
    if (isYc && !chromaSourceVideo.open(chromaFileName, fieldLength)) return 1;
    TargetVideo targetVideo;
    TargetVideo chromaTargetVideo;
    if (!targetVideo.open(outputFileName, videoParameters.system)) return 1;
    if (isYc && !chromaTargetVideo.open(outputChromaFileName, videoParameters.system)) return 1;

    const qint32 numberOfFields = std::min(metaData.getNumberOfFields(), sourceVideo.getNumberOfAvailableFields());
    if (numberOfFields != metaData.getNumberOfFields()) {
        qWarning() << "The metadata describes" << metaData.getNumberOfFields() << "fields but the file holds"
                   << sourceVideo.getNumberOfAvailableFields() << "- converting" << numberOfFields;
        metaData.setNumberOfFields(numberOfFields);
    }

    for (qint32 fieldNumber = 1; fieldNumber <= numberOfFields; fieldNumber++) {
        if (!targetVideo.writeField(sourceVideo.getVideoField(fieldNumber))) {
            qCritical() << "Writing the TBC output failed";
            return 1;
        }
        if (isYc && !chromaTargetVideo.writeField(chromaSourceVideo.getVideoField(fieldNumber))) {
            qCritical() << "Writing the TBC chroma output failed";
            return 1;
        }
    }
    targetVideo.close();
    if (isYc) chromaTargetVideo.close();

    // The audio of channel pair 0 becomes the PCM, copied as it is (16 bits, not
    // resampled) and declared to be at the sample rate asked for, else the WAV's
    if (hasAudio) {
        CvbsAudio::WavInfo wavInfo;
        if (!CvbsAudio::readWavInfo(wavFileName, wavInfo)) return 1;
        if (sampleRate == 0) sampleRate = wavInfo.sampleRate;

        if (CvbsAudio::wavToPcm(wavFileName, outputPcmFileName) < 0) {
            qCritical() << "Writing the PCM audio failed";
            return 1;
        }

        // Each field gets its share of the audio, as the TBC metadata keeps count of it
        for (qint32 fieldNumber = 1; fieldNumber <= numberOfFields; fieldNumber++) {
            LdDecodeMetaData::Field field = metaData.getField(fieldNumber);
            field.audioSamples = static_cast<qint32>(wavInfo.numberOfSamples * fieldNumber / numberOfFields
                                                     - wavInfo.numberOfSamples * (fieldNumber - 1) / numberOfFields);
            metaData.updateField(field, fieldNumber);
        }

        LdDecodeMetaData::PcmAudioParameters audioParameters;
        audioParameters.sampleRate = sampleRate;
        audioParameters.bits = 16;
        audioParameters.isSigned = true;
        audioParameters.isLittleEndian = true;
        audioParameters.isValid = true;
        metaData.setPcmAudioParameters(audioParameters);

        qInfo().noquote() << "Wrote" << wavInfo.numberOfSamples << "audio samples to" << outputPcmFileName
                          << "declared as" << sampleRate << "Hz (not resampled)";
        if (QFileInfo::exists(CvbsAudio::wavFileName(Cvbs::baseName(inputFileName), 1))) {
            qWarning() << "Only the first audio channel pair is converted: a TBC capture has one audio track";
        }
    }

    metaData.setFormat(outputFormat);
    if (!metaData.write(outputMetadataFileName)) {
        qCritical() << "Writing the TBC metadata failed";
        return 1;
    }

    qInfo().noquote() << "Wrote" << numberOfFields << "fields to" << outputFileName
                      << (isYc ? "and " + outputChromaFileName : QString()) << "with" << outputMetadataFileName;
    return 0;
}
