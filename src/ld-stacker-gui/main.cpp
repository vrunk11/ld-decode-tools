/******************************************************************************
 * main.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

#include "mainwindow.h"
#include "tbc/logging.h"

int main(int argc, char *argv[])
{
    qInstallMessageHandler(debugOutputHandler);

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("ld-stacker-gui");
    QCoreApplication::setApplicationVersion(QString("ld-decode-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");
    QCoreApplication::setOrganizationName("domesday86.com");

    // The icon: a disc, an arrow and the layers, drawn on the grid of the pixels for the small sizes
    QIcon icon;
    for (const int size: { 16, 24, 32, 48, 64, 128, 256 }) icon.addFile(QString(":/icons/app-%1.png").arg(size), QSize(size, size));
    application.setWindowIcon(icon);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "ld-stacker-gui - Disc stacking\n"
        "\n"
        "Compare several captures of the same disc and the stack of them.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption("force-dark-theme", "Force dark theme regardless of system settings"));
    parser.addPositionalArgument("files", QCoreApplication::translate("main", "A .stack session file, or the TBC sources to stack"), "[files...]");
    parser.process(application);
    // The theme (View > Theme) is applied by MainWindow from the settings; this overrides it for this session
    application.setProperty("forceDarkTheme", parser.isSet("force-dark-theme"));

    MainWindow window(parser.positionalArguments());
    window.show();
    return application.exec();
}
