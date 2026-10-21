/******************************************************************************
 * dropoutdrawing.h
 * ld-decode-tools - the dropouts of a field drawn on the picture of a frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef DROPOUTDRAWING_H
#define DROPOUTDRAWING_H

#include <QColor>
#include <QImage>

#include "dropouts.h"

// Where the two GUI programs draw the dropouts of a field on a picture: ld-analyse and ld-stacker-gui say the colour and the layout, this puts the pixels
namespace DropoutDrawing {

// Where the lines of the fields are on the picture
enum class Layout {
    Frame,              // the fields woven: the lines of the first field are the even rows, those of the second the odd ones
    Split,              // the first field above the second
    Field,              // one field shown at its own height, in the middle of the picture
    FieldStretched      // one field shown with each of its lines twice; the second field one row lower, as it is in the frame
};

// Draw the dropouts of a field (the first or the second of its frame) in the colour: for each, the samples from startx to endx on the row (or rows) of its
// line. The image is a Format_RGB32 one of a frame (frameHeight rows, 2 * the field height - 1), which the layouts Split and Field need to be given. A dropout
// that is outside the picture is not drawn.
void draw(QImage &image, const DropOuts &dropOuts, bool secondField, const QColor &color, Layout layout = Layout::Frame, qint32 frameHeight = 0);

}

#endif // DROPOUTDRAWING_H
