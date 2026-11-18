/******************************************************************************
 * main.cpp
 * ldx-packer - make an .ldx file from captures, or change the captures of one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mainwindow.h"
#include "tbc/logging.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>

#include <cstdio>

namespace {

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(debugOutputHandler);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ldx-packer"));
    Q_INIT_RESOURCE(ldx_packer_resources);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Graphics/256-ldx-packer.png")));
    QCoreApplication::setOrganizationDomain(QStringLiteral("domesday86.com"));
    QCoreApplication::setOrganizationName(QStringLiteral("domesday86.com"));
    QCoreApplication::setApplicationVersion(QStringLiteral("ld-decode-tools - Branch: %1 / Commit: %2").arg(QStringLiteral(APP_BRANCH), QStringLiteral(APP_COMMIT)));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Make an .ldx file from captures, or open one to see what it holds and change it: add and remove captures, keep the samples "
                                                    "in another codec, add audio, EFM and AC3 tracks and notes."));
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Write what the window shows to a PNG file and quit (for documentation and tests)."),
                                        QStringLiteral("file.png"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Size of the window, as 900x640."), QStringLiteral("WxH"));
    parser.addOption(screenshot);
    parser.addOption(QCommandLineOption(QStringLiteral("pick-track"), QStringLiteral("With --screenshot: pick this track (its place in the tracks that go with the first capture, from 0)."), QStringLiteral("n")));
    parser.addOption(size);
    parser.addOption(QCommandLineOption(QStringLiteral("force-dark-theme"), QStringLiteral("Force the dark theme, whatever the system and the setting say.")));
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("An .ldx file to open, and captures (.tbc .cvbs .cvbsy) to add to the list."), QStringLiteral("[file...]"));
    parser.process(app);
    processStandardDebugOptions(parser);
    app.setProperty("forceDarkTheme", parser.isSet(QStringLiteral("force-dark-theme")));

    MainWindow window;
    if (parser.isSet(size)) {
        const QStringList parts = parser.value(size).split(QLatin1Char('x'));
        bool okW = false;
        bool okH = false;
        const int w = parts.value(0).toInt(&okW);
        const int h = parts.value(1).toInt(&okH);
        if (!okW || !okH || w < 400 || h < 300) {
            std::fprintf(stderr, "ldx-packer: --size wants WxH, such as 900x640\n");
            return kExitUsage;
        }
        window.resize(w, h);
    }
    window.show();

    QStringList captures;
    bool opened = false;        // (the first .ldx file is opened; the next ones are put in the list, as the captures are)
    for (const QString &path : parser.positionalArguments()) {
        if (!opened && path.endsWith(QStringLiteral(".ldx"), Qt::CaseInsensitive)) {
            opened = true;
            if (!window.openArchive(path) && parser.isSet(screenshot)) return kExitFailed;
        } else {
            captures << path;
        }
    }
    if (!captures.isEmpty() && !window.addCaptures(captures) && parser.isSet(screenshot)) return kExitFailed;
    if (parser.isSet(QStringLiteral("pick-track"))) {
        QCoreApplication::processEvents();      // (the list of tracks is filled when the events are handled)
        window.pickTrack(0, parser.value(QStringLiteral("pick-track")).toInt());
    }
    if (parser.isSet(screenshot)) return window.saveScreenshot(parser.value(screenshot)) ? 0 : kExitFailed;
    return app.exec();
}
