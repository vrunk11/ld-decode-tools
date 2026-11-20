/******************************************************************************
 * i420.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef I420_H
#define I420_H

#include <QByteArray>
#include <QImage>

// The picture as I420 (YUV 4:2:0, 8 bits, the range of video, BT.601), `outWidth` by `outHeight` (both even; the picture's edge is repeated to fill them): the format that
// a video player takes without converting it, which a program that is given RGB does slower than the pictures come
QByteArray toI420(const QImage &image, qint32 outWidth, qint32 outHeight);

#endif // I420_H
