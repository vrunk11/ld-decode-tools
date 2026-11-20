/******************************************************************************
 * i420.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "i420.h"

#include <QRgb>

// The picture as I420: the luma of each pixel, then the chroma of each block of four (the colour differences, BT.601, the range of video). The size is the one of
// the frames that are sent (even, the picture's edge repeated); a program that is given RGB has to convert it itself, which it does slower than the picture comes
// (VLC with a picture of this size fell behind by seconds), so the conversion is made here.
QByteArray toI420(const QImage &image, qint32 outWidth, qint32 outHeight)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    const qint32 w = rgb.width();
    const qint32 h = rgb.height();
    const qsizetype lumaSize = static_cast<qsizetype>(outWidth) * outHeight;
    const qsizetype chromaWidth = outWidth / 2;
    const qsizetype chromaSize = chromaWidth * (outHeight / 2);
    QByteArray frame(static_cast<qsizetype>(lumaSize + 2 * chromaSize), Qt::Uninitialized);
    uchar *yPlane = reinterpret_cast<uchar *>(frame.data());
    uchar *uPlane = yPlane + lumaSize;
    uchar *vPlane = uPlane + chromaSize;

    const auto pixel = [&](qint32 x, qint32 y) { return reinterpret_cast<const QRgb *>(rgb.constScanLine(qMin(y, h - 1)))[qMin(x, w - 1)]; };
    for (qint32 by = 0; by < outHeight; by += 2) {
        for (qint32 bx = 0; bx < outWidth; bx += 2) {
            qint32 sumR = 0, sumG = 0, sumB = 0;
            for (qint32 dy = 0; dy < 2; dy++) {
                for (qint32 dx = 0; dx < 2; dx++) {
                    const QRgb p = pixel(bx + dx, by + dy);
                    const qint32 r = qRed(p), g = qGreen(p), b = qBlue(p);
                    yPlane[static_cast<qsizetype>(by + dy) * outWidth + bx + dx] = static_cast<uchar>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    sumR += r;
                    sumG += g;
                    sumB += b;
                }
            }
            const qint32 r = (sumR + 2) >> 2, g = (sumG + 2) >> 2, b = (sumB + 2) >> 2;
            const qsizetype at = static_cast<qsizetype>(by / 2) * chromaWidth + bx / 2;
            uPlane[at] = static_cast<uchar>(qBound(0, ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 255));
            vPlane[at] = static_cast<uchar>(qBound(0, ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 255));
        }
    }
    return frame;
}

