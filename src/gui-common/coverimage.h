/******************************************************************************
 * coverimage.h
 * ld-decode-tools - the cover of an LDX file as the programs show it and put it in a file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef COVERIMAGE_H
#define COVERIMAGE_H

#include <QByteArray>
#include <QImage>
#include <QString>

// The cover of an LDX file (section 9 of the specification) is a PNG or a JPEG, and the pictures people give are of the common formats (JPEG, WebP, PNG, GIF,
// BMP). Qt reads those it has the plugins for (the build of vcpkg has PNG only); the LDX library reads the others with ffmpeg, so a program shows and makes a
// cover whatever Qt was built with.
namespace CoverImage {

// The most a cover is high, and the most it weighs, as the specification says
constexpr int MaxHeight = 800;
constexpr qint64 MaxBytes = 2 * 1024 * 1024;

// The picture in the data (a file of any of those formats); a null image when it cannot be read
QImage fromData(const QByteArray &data, QString *error = nullptr);

// The picture of a file as the data of a cover: scaled down to at most MaxHeight pixels high, keeping its shape, and written as a JPEG (when it was a JPEG or a WebP
// and this build can write one) or as a PNG, in at most MaxBytes. `suffix` is "jpg" or "png". False, with the reason, when it cannot be done.
bool prepare(const QByteArray &fileData, QByteArray &cover, QString &suffix, QString *error = nullptr);

}

#endif // COVERIMAGE_H
