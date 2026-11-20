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

// The words of the options that are set ("module:name=value"): a switch is true or false (--name or --no-name), any other value is given as it is. The modules
// of the picture among them are added to filters.
static QStringList optionWords(const QStringList &options, QStringList &filters)
{
    QStringList words;
    for (const QString &entry: options) {
        const qsizetype colon = entry.indexOf(':');
        const qsizetype equals = entry.indexOf('=');
        if (colon < 0 || equals < colon) continue;
        const QString module = entry.left(colon);
        const QString name = entry.mid(colon + 1, equals - colon - 1);
        const QString value = entry.mid(equals + 1);
        if (value == "true") words << "--" + name;
        else if (value == "false") words << "--no-" + name;
        else words << QString("--%1=%2").arg(name, value);
        if (RendererSettings::isFilterModule(module) && !filters.contains(module)) filters << module;
    }
    return words;
}

// A deinterlacer that was added to VLC ("filter:" and its module) is not one of VLC's modes: VLC tries it before its own whatever the mode (it takes the picture or
// leaves it, by its own options), so the deinterlacing is asked in VLC's own mode, auto
static bool isAddedDeinterlacer(const QString &deinterlace)
{
    return deinterlace.startsWith(QLatin1String("filter:"));
}

QString RendererSettings::deinterlaceArguments(bool deinterlacedView) const
{
    if (engine != Vlc) return QString();
    // Only the Deinterlaced view is deinterlaced (the frame view is shown as it is, interlaced, and a field or the two fields side by side are not interlaced)
    if (!deinterlacedView) return "--deinterlace=0";
    // (with the mode that was chosen, or VLC's own)
    if (deinterlace.isEmpty()) return "--deinterlace=1";
    if (isAddedDeinterlacer(deinterlace)) return "--deinterlace=1 --deinterlace-mode=auto";
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
    const QStringList moduleWords = optionWords(options, filters);
    if (!filters.isEmpty()) words << "--video-filter=" + filters.join(':');
    words << moduleWords;
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
    // (there was an "off": the deinterlacing is for the Deinterlaced view only, which is off when it is not wanted)
    if (settings.deinterlace == "off") settings.deinterlace.clear();
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
    const QStringList moduleWords = optionWords(options, filters);
    if (!filters.isEmpty()) words << "--video-filter=" + filters.join(':');
    words << moduleWords;
    if (!extra.trimmed().isEmpty()) words << QProcess::splitCommand(extra.trimmed());
    return words;
}

QString RendererSettings::deinterlaceMode(bool deinterlacedView) const
{
    if (engine != Vlc || !deinterlacedView) return QString();
    // (VLC's own mode is what it calls auto; a deinterlacer that was added is asked in that mode)
    return deinterlace.isEmpty() || isAddedDeinterlacer(deinterlace) ? QStringLiteral("auto") : deinterlace;
}
