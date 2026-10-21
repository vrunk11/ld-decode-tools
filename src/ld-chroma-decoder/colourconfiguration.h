/******************************************************************************
 * colourconfiguration.h
 * ld-chroma-decoder - which colour decoder a video system has, and setting it up
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef COLOURCONFIGURATION_H
#define COLOURCONFIGURATION_H

#include "comb.h"
#include "lddecodemetadata.h"
#include "palcolour.h"

// The colour of PAL and of PAL-M (a 525-line PAL) is decoded by PalColour, that of NTSC by Comb
inline bool usesPalColour(VideoSystem system)
{
    return system == PAL || system == PAL_M;
}

// Set up the colour decoder that the video has, for its parameters: the other one is left as it is
inline void updateColourConfiguration(const LdDecodeMetaData::VideoParameters &videoParameters,
                                      PalColour &palColour, const PalColour::Configuration &palConfiguration,
                                      Comb &ntscColour, const Comb::Configuration &ntscConfiguration)
{
    if (usesPalColour(videoParameters.system)) {
        palColour.updateConfiguration(videoParameters, palConfiguration);
    } else {
        ntscColour.updateConfiguration(videoParameters, ntscConfiguration);
    }
}

#endif // COLOURCONFIGURATION_H
