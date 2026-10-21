/******************************************************************************
 * dropoutdrawing.cpp
 * ld-decode-tools - the dropouts of a field drawn on the picture of a frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "dropoutdrawing.h"

namespace DropoutDrawing {

// The samples from startx to endx (both drawn) of a row
static void drawRow(QImage &image, qint32 row, qint32 startx, qint32 endx, QRgb pixel)
{
    if (row < 0 || row >= image.height()) return;
    const qint32 from = qMax(0, startx);
    const qint32 to = qMin(image.width() - 1, endx);
    QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(row));
    for (qint32 x = from; x <= to; x++) line[x] = pixel;
}

void draw(QImage &image, const DropOuts &dropOuts, bool secondField, const QColor &color, Layout layout, qint32 frameHeight)
{
    if (image.isNull() || image.format() != QImage::Format_RGB32) return;
    const QRgb pixel = color.rgb();

    for (qint32 i = 0; i < dropOuts.size(); i++) {
        const qint32 startx = dropOuts.startx(i);
        const qint32 endx = dropOuts.endx(i);
        const qint32 fieldLine = dropOuts.fieldLine(i);

        switch (layout) {
        case Layout::Frame:
            drawRow(image, secondField ? (fieldLine * 2) - 1 : (fieldLine - 1) * 2, startx, endx, pixel);
            break;

        case Layout::Split:
            drawRow(image, secondField ? fieldLine + (frameHeight / 2) : fieldLine - 1, startx, endx, pixel);
            break;

        case Layout::Field:
            drawRow(image, fieldLine - 1 + (frameHeight / 4), startx, endx, pixel);
            break;

        case Layout::FieldStretched: {
            // (the bottom field is one line lower: see generateQImage of ld-analyse)
            const qint32 down = secondField ? 1 : 0;
            drawRow(image, (fieldLine - 1) * 2 + down, startx, endx, pixel);
            drawRow(image, (fieldLine - 1) * 2 + 1 + down, startx, endx, pixel);
            break;
        }
        }
    }
}

}
