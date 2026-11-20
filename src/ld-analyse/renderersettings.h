/******************************************************************************
 * renderersettings.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef RENDERERSETTINGS_H
#define RENDERERSETTINGS_H

#include <QString>
#include <QStringList>
#include <QVariantMap>

// What shows the picture (Config > Video filter...): the filters of ffmpeg that ld-analyse runs itself (the default), a VLC that is run for it with the options
// chosen here, or any other program with a command line (see ExternalRenderer).
struct RendererSettings {
    enum Engine { Ffmpeg = 0, Vlc = 1, Command = 2 };
    int engine = Ffmpeg;

    // VLC
    QString vlcPath;                 // where it is, if it was not found by itself
    QString vout;                    // the video output module (empty: VLC's own choice, or the one of its preferences)
    QString deinterlace;             // empty: the setting of VLC; a mode (yadif, blend...); or "filter:" and the module of a deinterlacer that was added to VLC
    QStringList options;             // the options that are set: "module:name=value" (the filters of the picture, the options of the video output)
    QString extra;                   // other arguments for VLC, as they are written on a command line

    // Another program
    QString command;

    // The command line for the engine (empty for ffmpeg): with {width}, {height}, {fps}, {aspect}, {deinterlace} and {winid} for the renderer to fill in.
    // `vlc` is the VLC that was found.
    QString toCommand(const QString &vlc) const;
    // What the {deinterlace} of the command is: the deinterlacing is for the Deinterlaced view of ld-analyse, and off in the others
    QString deinterlaceArguments(bool deinterlacedView) const;

    // For the libvlc that shows the picture (the engine VLC): the arguments it is started with (the video output, the filters of the picture and their options), and the mode
    // of the deinterlacing to give it (empty for none: it is for the Deinterlaced view only)
    QStringList libvlcArguments() const;
    QString deinterlaceMode(bool deinterlacedView) const;

    // The modules that are filters of the picture (they are put in --video-filter when one of their options is set)
    static bool isFilterModule(const QString &module);

    QVariantMap toMap() const;
    static RendererSettings fromMap(const QVariantMap &map);
    bool operator==(const RendererSettings &other) const;
};

#endif // RENDERERSETTINGS_H
