/******************************************************************************
 * renderersettings.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "renderersettings.h"

#include <QProcess>

bool RendererSettings::isFilterModule(const QString &module)
{
    return module == "adjust" || module == "sharpen" || module == "gradfun" || module == "grain";
}

QString RendererSettings::deinterlaceArguments(bool deinterlacedView) const
{
    if (engine != Vlc) return QString();
    // Only the Deinterlaced view is deinterlaced (the frame view is shown as it is, interlaced, and a field or the two fields side by side are not interlaced)
    if (deinterlace == "off" || !deinterlacedView) return "--deinterlace=0";
    // (with the mode that was chosen, or VLC's own)
    if (deinterlace.isEmpty()) return "--deinterlace=1";
    return QString("--deinterlace=1 --deinterlace-mode=%1").arg(deinterlace);
}

QString RendererSettings::toCommand(const QString &vlc) const
{
    if (engine == Command) return command.trimmed();
    if (engine != Vlc || vlc.isEmpty()) return QString();

    QStringList words;
    words << "\"" + vlc + "\"";
    // (VLC without its interface draws in the window it is given)
    words << "-I" << "dummy";
    words << "-" << "--demux=rawvideo" << "--rawvid-width={width}" << "--rawvid-height={height}" << "--rawvid-fps={fps}" << "--rawvid-chroma=I420"
          << "--no-audio" << "--aspect-ratio={aspect}" << "{deinterlace}";
    if (!vout.isEmpty()) words << "--vout=" + vout;

    // The options that are set: the filters of the picture are named in --video-filter, the others (a video output's) go as they are
    QStringList filters;
    QStringList optionWords;
    for (const QString &entry: options) {
        const qsizetype colon = entry.indexOf(':');
        const qsizetype equals = entry.indexOf('=');
        if (colon < 0 || equals < colon) continue;
        const QString module = entry.left(colon);
        optionWords << QString("--%1=%2").arg(entry.mid(colon + 1, equals - colon - 1), entry.mid(equals + 1));
        if (isFilterModule(module) && !filters.contains(module)) filters << module;
    }
    if (!filters.isEmpty()) words << "--video-filter=" + filters.join(':');
    words << optionWords;
    if (!extra.trimmed().isEmpty()) words << extra.trimmed();
    // (VLC draws in the window of ld-analyse, in the place of the picture)
#if defined(Q_OS_WIN)
    words << "--drawable-hwnd={winid}";
#elif defined(Q_OS_MACOS)
    words << "--drawable-nsobject={winid}";
#else
    words << "--drawable-xid={winid}";
#endif
    return words.join(' ');
}

QVariantMap RendererSettings::toMap() const
{
    return { { "engine", engine }, { "vlcPath", vlcPath }, { "vout", vout }, { "deinterlace", deinterlace },
             { "options", options }, { "extra", extra }, { "command", command } };
}

RendererSettings RendererSettings::fromMap(const QVariantMap &map)
{
    RendererSettings settings;
    settings.engine = qBound(0, map.value("engine", Ffmpeg).toInt(), 2);
    settings.vlcPath = map.value("vlcPath").toString();
    settings.vout = map.value("vout").toString();
    settings.deinterlace = map.value("deinterlace").toString();
    settings.options = map.value("options").toStringList();
    settings.extra = map.value("extra").toString();
    settings.command = map.value("command").toString();
    return settings;
}

bool RendererSettings::operator==(const RendererSettings &other) const
{
    return toMap() == other.toMap();
}

QStringList RendererSettings::libvlcArguments() const
{
    QStringList words;
    if (!vout.isEmpty()) words << "--vout=" + vout;
    // The options that are set: the filters of the picture are named in --video-filter, the others (a video output's) go as they are
    QStringList filters;
    QStringList optionWords;
    for (const QString &entry: options) {
        const qsizetype colon = entry.indexOf(':');
        const qsizetype equals = entry.indexOf('=');
        if (colon < 0 || equals < colon) continue;
        const QString module = entry.left(colon);
        optionWords << QString("--%1=%2").arg(entry.mid(colon + 1, equals - colon - 1), entry.mid(equals + 1));
        if (isFilterModule(module) && !filters.contains(module)) filters << module;
    }
    if (!filters.isEmpty()) words << "--video-filter=" + filters.join(':');
    words << optionWords;
    if (!extra.trimmed().isEmpty()) words << QProcess::splitCommand(extra.trimmed());
    return words;
}

QString RendererSettings::deinterlaceMode(bool deinterlacedView) const
{
    if (engine != Vlc || deinterlace == "off" || !deinterlacedView) return QString();
    // (VLC's own mode is what it calls auto)
    return deinterlace.isEmpty() ? QStringLiteral("auto") : deinterlace;
}
