/******************************************************************************
 * coverimage.cpp
 * ld-decode-tools - the cover of an LDX file as the programs show it and put it in a file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "coverimage.h"

#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <QObject>

#ifdef LDDECODE_WITH_LDX
#include "ldxpicture.h"
#endif

namespace CoverImage {

QImage fromData(const QByteArray &data, QString *error)
{
    // Qt first (it reads what it has the plugin for), then the LDX library (ffmpeg and its own PNG)
    QImage image = QImage::fromData(data);
    if (!image.isNull()) return image;
#ifdef LDDECODE_WITH_LDX
    Ldx::Picture picture;
    QString why;
    if (Ldx::decodePicture(data, picture, why)) {
        QImage copy(reinterpret_cast<const uchar *>(picture.rgba.constData()), picture.width, picture.height, picture.width * 4, QImage::Format_RGBA8888);
        return copy.copy();     // (the copy owns its pixels)
    }
    if (error) *error = why;
#else
    if (error) *error = QObject::tr("This build of Qt cannot read this picture");
#endif
    return QImage();
}

namespace {

bool qtWrites(const char *format)
{
    return QImageWriter::supportedImageFormats().contains(format);
}

// The image as PNG or JPEG data: Qt writes what it has the plugin for, the LDX library the rest
bool write(const QImage &image, bool jpeg, QByteArray &data)
{
    if (qtWrites(jpeg ? "jpeg" : "png")) {
        data.clear();
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        if (image.save(&buffer, jpeg ? "JPEG" : "PNG", jpeg ? 88 : -1)) return true;
    }
#ifdef LDDECODE_WITH_LDX
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    Ldx::Picture picture;
    picture.width = rgba.width();
    picture.height = rgba.height();
    picture.rgba = QByteArray(reinterpret_cast<const char *>(rgba.constBits()), static_cast<int>(rgba.sizeInBytes()));
    QString why;
    return Ldx::encodePicture(picture, jpeg, 88, data, why);
#else
    return false;
#endif
}

}

bool prepare(const QByteArray &fileData, QByteArray &cover, QString &suffix, QString *error)
{
    QString why;
    const QImage image = fromData(fileData, &why);
    if (image.isNull()) {
        if (error) *error = why.isEmpty() ? QObject::tr("The picture cannot be read") : why;
        return false;
    }
    // A photograph (a JPEG, a WebP) stays a JPEG when a JPEG can be written; the others are PNG
    QString format;
#ifdef LDDECODE_WITH_LDX
    format = Ldx::pictureFormat(fileData);
#endif
    if (format.isEmpty()) {
        QBuffer buffer;
        buffer.setData(fileData);
        buffer.open(QIODevice::ReadOnly);
        format = QString::fromLatin1(QImageReader(&buffer).format());
    }
    bool jpeg = format == QLatin1String("jpeg") || format == QLatin1String("jpg") || format == QLatin1String("webp");
#ifdef LDDECODE_WITH_LDX
    if (jpeg && !qtWrites("jpeg") && !Ldx::pictureAvailable()) jpeg = false;
#else
    if (jpeg && !qtWrites("jpeg")) jpeg = false;
#endif
    int height = std::min(image.height(), MaxHeight);
    for (;;) {
        const QImage scaled = image.height() > height ? image.scaledToHeight(height, Qt::SmoothTransformation) : image;
        if (!write(scaled, jpeg, cover)) {
            if (error) *error = QObject::tr("The picture cannot be written");
            return false;
        }
        if (cover.size() <= MaxBytes || height <= 200) break;
        height = height * 4 / 5;
    }
    suffix = jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    return cover.size() <= MaxBytes;
}

}
