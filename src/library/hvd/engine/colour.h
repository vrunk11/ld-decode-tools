// SPDX-License-Identifier: GPL-3.0-or-later
//
// colour.h — IRE-domain YUV -> RGB and Automatic Color Control gain.
//
// Ports `yuv_to_rgb16` and the ACC gain rule from `reference/hvd/decoder.py`.
//
// IMPORTANT — this module is NOT used by the default stage output. The default
// HVD stage emits a *lossless Y/C split* (luma Y and modulated chroma C = S - Y)
// that reconstructs the composite exactly; applying a saturation gain or a
// colour matrix would break that invariant. Saturation / RGB conversion is a
// separate colour-render concern (handled downstream, or by a future RGB-sink
// variant of this plugin). It is provided here, fully ported, so that variant
// can be built without re-deriving the maths, and so the preview path has a
// reference conversion.

#ifndef LDDECODE_HVD_ENGINE_COLOUR_H_
#define LDDECODE_HVD_ENGINE_COLOUR_H_

#include <array>
#include <cstdint>

#include "engine/plane.h"

namespace hvd {

constexpr float kIreBlack = 7.5F;    // NTSC-M setup pedestal
constexpr float kIreWhite = 100.0F;

// Automatic Color Control gain: normalise chroma saturation to the nominal
// 20 IRE burst. `median_burst_amplitude_ire` is the measured value (see
// BurstAmplitudeIre). Clamped to [0.5, 2.0], matching the reference.
float AccGain(float median_burst_amplitude_ire);
// Standard-aware overload: PAL nominal burst is 21.43 IRE, NTSC 20.
float AccGain(float median_burst_amplitude_ire, float nominal_ire);

// Convert one pixel of IRE-domain (Y, U, V) to 16-bit RGB. `black_ire` is 7.5
// for NTSC-M or 0.0 for NTSC-J. Values are clipped to [0, 1] then scaled.
std::array<uint16_t, 3> YuvToRgb16(float y, float u, float v, float black_ire);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_COLOUR_H_
