/******************************************************************************
 * libvlcapi.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "libvlcapi.h"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

const LibVlc *LibVlc::load(const QString &vlcExecutable, QString &error)
{
    static LibVlc api;
    static QLibrary *library = nullptr;
    static QString loadedFrom;
    if (library && library->isLoaded()) return &api;

    const QFileInfo vlc(vlcExecutable);
    const QString folder = vlc.absolutePath();
    QStringList candidates;
#if defined(Q_OS_WIN)
    candidates << folder + "/libvlc.dll";
    // (the libvlccore it needs, and the modules, are in that folder: it is where the library looks for them)
    SetDllDirectoryW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(folder).utf16()));
#elif defined(Q_OS_MACOS)
    candidates << folder + "/lib/libvlc.dylib" << folder + "/../lib/libvlc.dylib";
#else
    candidates << folder + "/libvlc.so.5" << QStringLiteral("libvlc.so.5") << QStringLiteral("libvlc");
#endif
    QString lastError;
    for (const QString &name: candidates) {
        auto *candidate = new QLibrary(name);
        if (candidate->load()) {
            library = candidate;
            loadedFrom = name;
            break;
        }
        lastError = candidate->errorString();
        delete candidate;
    }
    if (!library) {
        error = QObject::tr("libvlc could not be loaded from %1: %2").arg(folder, lastError);
        return nullptr;
    }

    bool complete = true;
    const auto symbol = [&](const char *name, auto &pointer, bool required = true) {
        pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(library->resolve(name));
        if (!pointer && required) {
            complete = false;
            error = QObject::tr("libvlc has no %1: it is not a libvlc 3 that ld-analyse can use").arg(QString::fromLatin1(name));
        }
    };
    symbol("libvlc_new", api.newInstance);
    symbol("libvlc_release", api.releaseInstance);
    symbol("libvlc_media_new_location", api.mediaNewLocation);
    symbol("libvlc_media_add_option", api.mediaAddOption);
    symbol("libvlc_media_release", api.mediaRelease);
    symbol("libvlc_media_player_new_from_media", api.playerNewFromMedia);
    symbol("libvlc_media_player_release", api.playerRelease);
    symbol("libvlc_media_player_play", api.playerPlay);
    symbol("libvlc_media_player_stop", api.playerStop);
    symbol("libvlc_media_player_set_hwnd", api.playerSetHwnd, false);
    symbol("libvlc_media_player_set_xwindow", api.playerSetXwindow, false);
    symbol("libvlc_media_player_set_nsobject", api.playerSetNsobject, false);
    symbol("libvlc_video_set_deinterlace", api.setDeinterlace);
    symbol("libvlc_video_set_aspect_ratio", api.setAspectRatio);
    symbol("libvlc_get_version", api.version, false);
    if (!complete) {
        library->unload();
        delete library;
        library = nullptr;
        return nullptr;
    }
    return &api;
}
