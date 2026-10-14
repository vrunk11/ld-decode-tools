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

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "ld-stacker-gui - Disc stacking\n"
        "\n"
        "Compare several captures of the same disc and the stack of them.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("files", QCoreApplication::translate("main", "A .stack session file, or the TBC sources to stack"), "[files...]");
    parser.process(application);

    MainWindow window(parser.positionalArguments());
    window.show();
    return application.exec();
}
