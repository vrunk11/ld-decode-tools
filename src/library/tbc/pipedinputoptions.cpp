/******************************************************************************
 * pipedinputoptions.cpp
 * ld-decode-tools - the options for video that comes in a pipe
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "pipedinputoptions.h"

#include <QCoreApplication>
#include <QDebug>

PipedInputOptions::PipedInputOptions()
    : formatOption(QStringList() << "input-format",
                   QCoreApplication::translate("main", "Piped input: ldx (an LDX stream, which says what it holds: the default when no video system is said), tbc (fields) or cvbs (frames, as decode-orc writes them)"),
                   QCoreApplication::translate("main", "format")),
      systemOption(QStringList() << "system",
                   QCoreApplication::translate("main", "Piped input without metadata: the video system (pal, ntsc or pal-m)"),
                   QCoreApplication::translate("main", "system")),
      blackLevelOption(QStringList() << "black-level",
                       QCoreApplication::translate("main", "Piped TBC input without metadata: the black16bIre of the capture (default: the standard level of the system)"),
                       QCoreApplication::translate("main", "number")),
      whiteLevelOption(QStringList() << "white-level",
                       QCoreApplication::translate("main", "Piped TBC input without metadata: the white16bIre of the capture (default: the standard level of the system)"),
                       QCoreApplication::translate("main", "number")),
      subcarrierLockedOption(QStringList() << "subcarrier-locked",
                             QCoreApplication::translate("main", "Piped input without metadata: whether PAL was sampled locked to the subcarrier, which puts the second field 2 samples off (yes or no; default yes for CVBS, no for TBC)"),
                             QCoreApplication::translate("main", "yes|no")),
      sampleEncodingOption(QStringList() << "sample-encoding",
                           QCoreApplication::translate("main", "Piped CVBS input: u10 (CVBS_U10_4FSC, the default), u16, tpg21 or s16"),
                           QCoreApplication::translate("main", "encoding"))
{
}

void PipedInputOptions::addTo(QCommandLineParser &parser)
{
    parser.addOption(formatOption);
    parser.addOption(systemOption);
    parser.addOption(blackLevelOption);
    parser.addOption(whiteLevelOption);
    parser.addOption(subcarrierLockedOption);
    parser.addOption(sampleEncodingOption);
}

bool PipedInputOptions::format(const QCommandLineParser &parser, QString &format) const
{
    format.clear();
    if (!parser.isSet(formatOption)) return true;
    format = parser.value(formatOption).toLower();
    if (format != "tbc" && format != "cvbs" && format != "ldx") {
        qCritical().noquote() << "Unknown input format" << format << "- use ldx, tbc or cvbs";
        return false;
    }
    return true;
}

bool PipedInputOptions::sampleEncoding(const QCommandLineParser &parser, Cvbs::SampleEncoding &encoding) const
{
    const QString name = parser.value(sampleEncodingOption);
    if (!Cvbs::parseSampleEncodingName(name, encoding)
        && !Cvbs::parseSampleEncodingName("CVBS_" + name + "_4FSC", encoding)) {
        qCritical().noquote() << "Unknown sample encoding" << name << "- use u10, u16, tpg21 or s16";
        return false;
    }
    return true;
}

bool PipedInputOptions::videoSystem(const QCommandLineParser &parser, VideoSystem &system) const
{
    if (!parseVideoSystemName(parser.value(systemOption).toUpper().replace('-', '_'), system)) {
        qCritical().noquote() << "Unknown video system" << parser.value(systemOption) << "- use pal, ntsc or pal-m";
        return false;
    }
    return true;
}

bool PipedInputOptions::levels(const QCommandLineParser &parser, bool isCvbs, qint32 &blackLevel, qint32 &whiteLevel) const
{
    blackLevel = -1;
    whiteLevel = -1;
    if (parser.isSet(blackLevelOption)) blackLevel = parser.value(blackLevelOption).toInt();
    if (parser.isSet(whiteLevelOption)) whiteLevel = parser.value(whiteLevelOption).toInt();
    if ((parser.isSet(blackLevelOption) && blackLevel < 0) || (parser.isSet(whiteLevelOption) && whiteLevel < 1)) {
        qCritical("The black and white levels must be positive");
        return false;
    }
    if (isCvbs && isLevelSet(parser)) {
        qWarning() << "--black-level and --white-level apply to TBC input, not CVBS (which has standard levels): ignored";
        blackLevel = whiteLevel = -1;
    }
    return true;
}

bool PipedInputOptions::subcarrierLocked(const QCommandLineParser &parser, bool defaultValue, bool &locked) const
{
    locked = defaultValue;
    if (parser.isSet(subcarrierLockedOption)) {
        const QString value = parser.value(subcarrierLockedOption).toLower();
        if (value != "yes" && value != "no") {
            qCritical().noquote() << "--subcarrier-locked must be yes or no, not" << value;
            return false;
        }
        locked = value == "yes";
    }
    return true;
}
