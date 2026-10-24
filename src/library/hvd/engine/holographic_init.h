// SPDX-License-Identifier: GPL-3.0-or-later
//
// holographic_init.h — digital-holography reconstruction of the chroma phasor.
//
// Ports `holographic_init` + `_gaussian_lpf_kernel_fft` + `_box_blur` from
// `reference/hvd/decoder.py`.
//
// Standard off-axis holographic reconstruction: demodulate the composite by
// exp(-i*phi) (which shifts the chroma sideband to DC and pushes luma to
// +/-fsc), then crop the sideband with a 2-D Gaussian window in Fourier space.
// Two complementary anisotropic crops (narrow-in-x / wide-in-y and the
// transpose) are blended per pixel by which one leaves the locally smoothest
// residual luma — the decoder's own arbitration criterion applied at t=0
// (Dubois-style adaptive spectral init from CFA demosaicing). This alone
// already matches or beats a 2-D comb; it is also a better linearisation point
// for the variational IRLS that follows.

#ifndef LDDECODE_HVD_ENGINE_HOLOGRAPHIC_INIT_H_
#define LDDECODE_HVD_ENGINE_HOLOGRAPHIC_INIT_H_

#include "engine/fft2d.h"
#include "engine/hvd_config.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace hvd {

// Result of the initial reconstruction: the residual luma Y0 and the chroma
// phasor chi0, both over the active picture area. By construction they satisfy
// Y0 + Re[chi0 * carrier] == S exactly (a lossless split), so the pair is a
// valid decode on its own when cg_iterations == 0.
struct HoloInit {
  Plane luma;           // Y0 (IRE)
  ComplexPlane chroma;  // chi0 = V - iU (IRE)
};

// `s` is the active composite (IRE). `carrier` is exp(i*phi) over the same
// area (from MakeCarrier). `fft` provides the 2-D transforms.
HoloInit HolographicInit(const Plane& s, const ComplexPlane& carrier,
                         const FieldGeometry& g, const HvdConfig& cfg,
                         Fft2d* fft);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_HOLOGRAPHIC_INIT_H_
