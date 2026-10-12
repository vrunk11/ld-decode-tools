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
#include <QApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QLoggingCategory>


#include "tbc/logging.h"

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
    QCommandLineOption inputFormatOption(QStringList() << "input-format",
                                         QCoreApplication::translate("main", "Piped input: tbc (fields, the default) or cvbs (frames, as decode-orc writes them)"),
                                         QCoreApplication::translate("main", "format"));
    parser.addOption(inputFormatOption);

    QCommandLineOption systemOption(QStringList() << "system",
                                    QCoreApplication::translate("main", "Piped input: the video system (pal, ntsc or pal-m)"),
                                    QCoreApplication::translate("main", "system"));
    parser.addOption(systemOption);

    QCommandLineOption blackLevelOption(QStringList() << "black-level",
                                        QCoreApplication::translate("main", "Piped TBC input: the black16bIre of the capture (default: the standard level of the system)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(blackLevelOption);

    QCommandLineOption whiteLevelOption(QStringList() << "white-level",
                                        QCoreApplication::translate("main", "Piped TBC input: the white16bIre of the capture (default: the standard level of the system)"),
                                        QCoreApplication::translate("main", "number"));
    parser.addOption(whiteLevelOption);

    QCommandLineOption subcarrierLockedOption(QStringList() << "subcarrier-locked",
                                              QCoreApplication::translate("main", "Piped input: whether PAL was sampled locked to the subcarrier, which puts the second field 2 samples off (yes or no; default yes for CVBS, no for TBC)"),
                                              QCoreApplication::translate("main", "yes|no"));
    parser.addOption(subcarrierLockedOption);

    QCommandLineOption sampleEncodingOption(QStringList() << "sample-encoding",
                                            QCoreApplication::translate("main", "Piped CVBS input: u10 (CVBS_U10_4FSC, the default), u16, tpg21 or s16"),
                                            QCoreApplication::translate("main", "encoding"));
    parser.addOption(sampleEncodingOption);

    QCommandLineOption bufferFramesOption(QStringList() << "buffer-frames",
                                          QCoreApplication::translate("main", "Piped input: the size of each of the two buffers, in frames (default: 5 seconds of video). A paused input can be gone through in the frozen one while the other goes on filling"),
                                          QCoreApplication::translate("main", "number"));
    parser.addOption(bufferFramesOption);

    QCommandLineOption throttleOption(QStringList() << "throttle",
                                      QCoreApplication::translate("main", "Piped input: hold the producer back to the pace of the viewer, instead of overwriting old video. For video that arrives faster than it plays (a file, or a decoder that is not live); pausing then holds the input up"));
    parser.addOption(throttleOption);

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC or CVBS file, or - for video piped in"));

    // Process the command line arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // The theme (View > Theme) is applied by MainWindow from the configuration;
    // --force-dark-theme overrides it for this session only
    a.setProperty("forceDarkTheme", parser.isSet("force-dark-theme"));

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
            return -1;
        }
        setBinaryMode();

        if (parser.isSet(inputFormatOption)) {
            const QString format = parser.value(inputFormatOption).toLower();
            if (format != "tbc" && format != "cvbs") {
                qCritical().noquote() << "Unknown input format" << format << "- use tbc or cvbs";
                return -1;
            }
            streamInput.cvbs = format == "cvbs";
        }

        if (!parser.isSet(systemOption)) {
            qCritical("Piped input has no metadata: say the video system with --system pal, ntsc or pal-m "
                      "(and --input-format cvbs if it is CVBS rather than TBC)");
            return -1;
        }
        if (!parseVideoSystemName(parser.value(systemOption).toUpper().replace('-', '_'), streamInput.system)) {
            qCritical().noquote() << "Unknown video system" << parser.value(systemOption) << "- use pal, ntsc or pal-m";
            return -1;
        }

        if (parser.isSet(sampleEncodingOption)) {
            const QString name = parser.value(sampleEncodingOption);
            if (!Cvbs::parseSampleEncodingName(name, streamInput.encoding)
                && !Cvbs::parseSampleEncodingName("CVBS_" + name + "_4FSC", streamInput.encoding)) {
                qCritical().noquote() << "Unknown sample encoding" << name << "- use u10, u16, tpg21 or s16";
                return -1;
            }
        }
        if (parser.isSet(blackLevelOption)) streamInput.blackLevel = parser.value(blackLevelOption).toInt();
        if (parser.isSet(whiteLevelOption)) streamInput.whiteLevel = parser.value(whiteLevelOption).toInt();
        if ((parser.isSet(blackLevelOption) && streamInput.blackLevel < 0)
            || (parser.isSet(whiteLevelOption) && streamInput.whiteLevel < 1)) {
            qCritical("The black and white levels must be positive");
            return -1;
        }
        if (streamInput.cvbs && (parser.isSet(blackLevelOption) || parser.isSet(whiteLevelOption))) {
            qWarning() << "--black-level and --white-level apply to TBC input, not CVBS (which has standard levels): ignored";
            streamInput.blackLevel = streamInput.whiteLevel = -1;
        }

        // ld-decode 8 and decode-orc mark the CVBS they write as locked
        streamInput.isSubcarrierLocked = streamInput.cvbs;
        if (parser.isSet(subcarrierLockedOption)) {
            const QString value = parser.value(subcarrierLockedOption).toLower();
            if (value != "yes" && value != "no") {
                qCritical().noquote() << "--subcarrier-locked must be yes or no, not" << value;
                return -1;
            }
            streamInput.isSubcarrierLocked = value == "yes";
        }
        // Each of the two buffers holds about 5 seconds of video: how far back a paused input can be gone through
        streamInput.bufferFrames = streamInput.system == PAL ? 125 : 150;
        streamInput.throttle = parser.isSet(throttleOption);
        if (parser.isSet(bufferFramesOption)) {
            streamInput.bufferFrames = parser.value(bufferFramesOption).toInt();
            if (streamInput.bufferFrames < 1) {
                qCritical("The buffer must hold at least 1 frame");
                return -1;
            }
        }
    } else if (parser.isSet(inputFormatOption) || parser.isSet(systemOption) || parser.isSet(bufferFramesOption) || parser.isSet(throttleOption)) {
        qWarning() << "--input-format, --system and the other piped input options only apply to piped input (ld-analyse -)";
    }

    // Start the GUI application
    MainWindow w(inputFileName, isStream ? &streamInput : nullptr);
    w.show();

    return a.exec();
}
