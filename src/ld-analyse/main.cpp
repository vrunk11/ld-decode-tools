/******************************************************************************
 * main.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mainwindow.h"
#include "tbcsource.h"
#include "ldxstream.h"
#include <QApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QLoggingCategory>


#include "tbc/logging.h"
#include "tbc/pipedinputoptions.h"

// Custom message handler that filters out harmless Qt system warnings
void filteredDebugOutputHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Filter out harmless Qt system warnings that don't affect functionality
    if (msg.contains("Wayland does not support QWindow::requestActivate()") ||
        msg.contains("QSocketNotifier: Can only be used with threads started with QThread")) {
        return; // Don't output these warnings
    }
    
    // Call the original handler for all other messages
    debugOutputHandler(type, context, msg);
}

int main(int argc, char *argv[])
{
    // Install the local debug message handler with Qt system warning filtering
    qInstallMessageHandler(filteredDebugOutputHandler);

    QApplication a(argc, argv);

    // Set application name and version
    QCoreApplication::setApplicationName("ld-analyse");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set desktop file name for proper GNOME integration
    // This must match the installed .desktop file name (without .desktop extension)
    QGuiApplication::setDesktopFileName("ld-analyse");
    
    // Set application icon (for window decorations and taskbar)
    // QIcon::fromTheme will find the icon we installed to /usr/local/share/icons/hicolor/
    a.setWindowIcon(QIcon::fromTheme("ld-analyse"));

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "ld-analyse - TBC output analysis\n"
        "\n"
        "(c)2018-2025 Simon Inns\n"
        "(c)2020-2022 Adam Sampson\n"
        "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Add theme override option
    parser.addOption(QCommandLineOption("force-dark-theme", "Force dark theme regardless of system settings"));

    // -- Input from a pipe: ld-analyse - (or a named pipe) shows video as it arrives --
    PipedInputOptions pipedOptions;
    pipedOptions.addTo(parser);

    QCommandLineOption bufferFramesOption(QStringList() << "buffer-frames",
                                          QCoreApplication::translate("main", "Piped input: the size of each of the two buffers, in frames (default: 5 seconds of video). A paused input can be gone through in the frozen one while the other goes on filling"),
                                          QCoreApplication::translate("main", "number"));
    parser.addOption(bufferFramesOption);

    QCommandLineOption throttleOption(QStringList() << "throttle",
                                      QCoreApplication::translate("main", "Piped input: hold the producer back to the pace of the viewer, instead of overwriting old video. For video that arrives faster than it plays (a file, or a decoder that is not live); pausing then holds the input up"));
    parser.addOption(throttleOption);

    QCommandLineOption ignoreMapOption("ignore-map", QCoreApplication::translate("main", "Read the capture as it is, though its metadata keep a map (the frames in the order of the disc, from ld-discmap)"));
    parser.addOption(ignoreMapOption);

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC or CVBS file, or - for video piped in"));

    // Process the command line arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // The theme (View > Theme) is applied by MainWindow from the configuration;
    // --force-dark-theme overrides it for this session only
    a.setProperty("forceDarkTheme", parser.isSet("force-dark-theme"));
    TbcSource::ignoreEmbeddedMap = parser.isSet(ignoreMapOption);

    // Get the arguments from the parser
    QString inputFileName;
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 1) {
        inputFileName = positionalArguments.at(0);
    } else {
        inputFileName.clear();
    }

    // Piped input is standard input ("-") or a named pipe. It has no metadata, so the video
    // system must be said; it may never end, and plays as it arrives.
    bool isStream = inputFileName == "-" || (QFileInfo::exists(inputFileName) && !QFileInfo(inputFileName).isFile());
    TbcSource::StreamInput streamInput;
    if (isStream) {
        if (inputFileName != "-") {
            qCritical().noquote() << "ld-analyse reads a pipe from standard input: use -, as in `producer | ld-analyse -`";
            return 2;
        }
        setBinaryMode();

        QString inputFormat;
        if (!pipedOptions.format(parser, inputFormat)) return 2;
        if (!inputFormat.isEmpty()) streamInput.cvbs = inputFormat == "cvbs";

        // An LDX stream says by itself what it holds (the video system, TBC or CVBS): it is what a pipe is when no format is asked
        // for and no video system is said. Its video is shown as the video of any pipe is, as it arrives.
        const bool formatGiven = !inputFormat.isEmpty();
        // (with no format said, the way the input begins tells an LDX stream from raw video, whatever --system says)
        const bool ldxInput = formatGiven ? inputFormat == "ldx" : LdxStream::stdinIsLdx();
        if (ldxInput) {
            QString streamError;
            if (!LdxStream::openStdin(streamError)) {
                qCritical().noquote() << streamError;
                return 1;
            }
            streamInput.cvbs = LdxStream::instance()->isCvbs();
            streamInput.system = LdxStream::instance()->system();
#ifdef LDDECODE_HAVE_AUDIO
            LdxStream::instance()->keepOtherTracks();        // (the audio and the EFM of the stream are for the waveform and the sound)
#endif
            qInfo().noquote() << "The LDX stream holds" << Cvbs::presetName(streamInput.system) << (streamInput.cvbs ? "CVBS" : "TBC") << "video";
        } else {
            if (!pipedOptions.isSystemSet(parser)) {
                qCritical("Piped input has no metadata: say the video system with --system pal, ntsc or pal-m "
                          "(and --input-format cvbs if it is CVBS rather than TBC), or pipe an LDX stream");
                return 2;
            }
            if (!pipedOptions.videoSystem(parser, streamInput.system)) return 2;
        }

        if (pipedOptions.isSampleEncodingSet(parser) && !pipedOptions.sampleEncoding(parser, streamInput.encoding)) return 2;
        if (!pipedOptions.levels(parser, streamInput.cvbs, streamInput.blackLevel, streamInput.whiteLevel)) return 2;

        // ld-decode 8 and decode-orc mark the CVBS they write as locked
        if (!pipedOptions.subcarrierLocked(parser, streamInput.cvbs, streamInput.isSubcarrierLocked)) return 2;
        // Each of the two buffers holds about 5 seconds of video: how far back a paused input can be gone through
        streamInput.bufferFrames = streamInput.system == PAL ? 125 : 150;
        streamInput.throttle = parser.isSet(throttleOption);
        if (parser.isSet(bufferFramesOption)) {
            streamInput.bufferFrames = parser.value(bufferFramesOption).toInt();
            if (streamInput.bufferFrames < 1) {
                qCritical("The buffer must hold at least 1 frame");
                return 2;
            }
        }
    } else if (pipedOptions.isFormatSet(parser) || pipedOptions.isSystemSet(parser) || parser.isSet(bufferFramesOption) || parser.isSet(throttleOption)) {
        qWarning() << "--input-format, --system and the other piped input options only apply to piped input (ld-analyse -)";
    }

    // Start the GUI application
    MainWindow w(inputFileName, isStream ? &streamInput : nullptr);
    w.show();

    return a.exec();
}
