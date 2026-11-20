/******************************************************************************
 * libvlcapi.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LIBVLCAPI_H
#define LIBVLCAPI_H

#include <QString>

#include <cstdint>

// The few functions of libvlc (3.x) that ld-analyse uses, found in the libvlc of the VLC that is installed (its libvlc.dll, libvlc.so or libvlc.dylib), at run time: there
// is no VLC SDK to build with, and a build without VLC is the same build. The library is loaded from the folder of the VLC, whose modules (plugins) it then finds: the
// ones that were added to that VLC (an upscaler, a deinterlacer) are its own.
struct LibVlc {
    struct Instance;
    struct Media;
    struct Player;

    Instance *(*newInstance)(int, const char *const *) = nullptr;
    void (*releaseInstance)(Instance *) = nullptr;
    Media *(*mediaNewLocation)(Instance *, const char *) = nullptr;
    void (*mediaAddOption)(Media *, const char *) = nullptr;
    void (*mediaRelease)(Media *) = nullptr;
    Player *(*playerNewFromMedia)(Media *) = nullptr;
    void (*playerRelease)(Player *) = nullptr;
    int (*playerPlay)(Player *) = nullptr;
    void (*playerStop)(Player *) = nullptr;
    void (*playerSetHwnd)(Player *, void *) = nullptr;
    void (*playerSetXwindow)(Player *, uint32_t) = nullptr;
    void (*playerSetNsobject)(Player *, void *) = nullptr;
    void (*setDeinterlace)(Player *, const char *) = nullptr;
    void (*setAspectRatio)(Player *, const char *) = nullptr;
    const char *(*version)() = nullptr;

    // Load it from the folder of the VLC at `vlcExecutable` (the libvlc that is next to it, or the system's): the same one for every call. Null, with the reason, if it
    // cannot be had.
    static const LibVlc *load(const QString &vlcExecutable, QString &error);
};

#endif // LIBVLCAPI_H
