/******************************************************************************
 * aspectratio.h
 * ld-decode-tools - how much wider or narrower than its samples a picture is shown
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef ASPECTRATIO_H
#define ASPECTRATIO_H

#include <QtGlobal>

#include "lddecodemetadata.h"

namespace AspectRatio {

// The change of width, in pixels, that gives the picture of a video system its 4:3 or 16:9 shape (625 lines for PAL,
// 525 lines for NTSC and PAL-M); a picture shown with square pixels has none
inline qint32 widthAdjustment(VideoSystem system, bool widescreen)
{
    if (system == PAL) return widescreen ? 103 : -196;
    return widescreen ? 122 : -150;
}

}

#endif // ASPECTRATIO_H
