/******************************************************************************
 * ldxpicture.h
 * ld-decode-tools LDX library - the picture that is the cover of a file (JPEG, WebP, PNG, GIF, BMP), through ffmpeg
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXPICTURE_H
#define LDXPICTURE_H

#include <QByteArray>
#include <QString>

// The cover of an LDX file is a PNG or a JPEG (section 9 of the specification), but the pictures people have are of the common formats: JPEG, WebP, PNG, GIF,
// BMP. Qt reads them when it has the plugins for them (the build of vcpkg has PNG only), and ffmpeg (libavcodec) reads them all, so the tools turn to it
// when Qt cannot: these functions decode a picture, scale it and write it as a PNG or a JPEG. Without ffmpeg none of them is available.
namespace Ldx {

struct Picture {
    int width = 0;
    int height = 0;
    QByteArray rgba;        // 4 bytes for each pixel (red, green, blue, alpha), line after line
    bool isNull() const { return width <= 0 || height <= 0 || rgba.size() != width * height * 4; }
};

// Whether this build can decode and encode pictures
bool pictureAvailable();

// What the data are, from their first bytes: "png", "jpeg", "webp", "gif" or "bmp"; empty when they are none of those
QString pictureFormat(const QByteArray &data);

// The first picture of the data (a file of any of those formats). False, with the reason, when it cannot be read.
bool decodePicture(const QByteArray &data, Picture &picture, QString &error);

// The picture scaled down, keeping its shape, so that it is at most `maxHeight` pixels high (it is as it was when it is not higher)
Picture scaledToHeight(const Picture &picture, int maxHeight);

// The picture as a PNG, or as a JPEG (quality from 1 to 100; the transparent parts are made white)
bool encodePicture(const Picture &picture, bool jpeg, int quality, QByteArray &data, QString &error);

}

#endif // LDXPICTURE_H
