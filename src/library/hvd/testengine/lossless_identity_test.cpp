// SPDX-License-Identifier: GPL-3.0-or-later
//
// The purity contract: after decoding, luma + Re[chroma * carrier] must equal
// the composite S to numerical precision (a lossless split). This mirrors the
// reference's run_tests.test_lossless_identity. We build a synthetic composite
// from a known (Y, chi), run the holographic init and the variational solver,
// and check both stages preserve the identity.

#include <algorithm>
#include <cmath>
#include <random>

#include "check.h"
#include "engine/holographic_init.h"
#include "engine/ntsc_geometry.h"
#include "engine/variational.h"

namespace {

using hvd::Complex;
using hvd::ComplexPlane;
using hvd::FieldGeometry;
using hvd::Fft2d;
using hvd::HvdConfig;
using hvd::Plane;

// Residual: max |S - luma - Re[chroma * carrier]| over the plane.
float MaxResidual(const Plane& s, const Plane& luma, const ComplexPlane& chroma,
                  const ComplexPlane& carrier) {
  float m = 0.0F;
  for (size_t i = 0; i < s.size(); ++i) {
    const float recon = luma[i] + (chroma[i] * carrier[i]).real();
    m = std::max(m, std::fabs(s[i] - recon));
  }
  return m;
}

}  // namespace

void RunTests() {
  const int h = 40;
  const int w = 48;
  FieldGeometry g;
  g.active_video_start = 0;
  g.active_video_end = w;

  // Carrier over the active area (theta = 0 per line is fine for the identity).
  std::vector<float> theta(h, 0.0F);
  const ComplexPlane carrier = hvd::MakeCarrier(theta, g);

  // Known ground-truth Y (smooth ramp) and chi (a couple of colour blobs).
  std::mt19937 rng(7);
  Plane y_true(h, w);
  ComplexPlane chi_true(h, w);
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      y_true.at(r, c) = 20.0F + 40.0F * static_cast<float>(c) / w;
      const float u = (r > h / 3 && r < 2 * h / 3 && c > w / 4) ? 15.0F : 0.0F;
      const float v = (c > w / 2) ? -10.0F : 5.0F;
      chi_true.at(r, c) = Complex{v, -u};  // chi = V - iU
    }
  }

  // Synthesize the composite S = Y + Re[chi * carrier].
  Plane s(h, w);
  for (size_t i = 0; i < s.size(); ++i)
    s[i] = y_true[i] + (chi_true[i] * carrier[i]).real();

  Fft2d fft;
  HvdConfig cfg;
  cfg.cg_iterations = 40;

  const hvd::HoloInit init = hvd::HolographicInit(s, carrier, g, cfg, &fft);
  CHECK_NEAR(MaxResidual(s, init.luma, init.chroma, carrier), 0.0, 1e-3);

  const hvd::RefineResult ref =
      hvd::VariationalRefine(s, carrier, init.chroma, cfg);
  CHECK_NEAR(MaxResidual(s, ref.luma, ref.chroma, carrier), 0.0, 1e-3);

  // --- Same check, now with a temporal neighbour term active. The whole
  // point of VariationalRefine's neighbours parameter is that Y stays
  // EXACTLY eliminated (Y = S - Re[chi*carrier]) even in "3D mode" — see
  // variational.h's doc comment on the algebraic reformulation. A perfect
  // static neighbour (same composite, 180 deg-flipped carrier) is the
  // reference's own worked example for what this should do to a static
  // pixel; here we just confirm the lossless split survives regardless. --
  {
    hvd::NeighborTerm nbr;
    nbr.composite = s;  // a perfectly static "neighbour" frame
    nbr.carrier = ComplexPlane(h, w);
    for (size_t i = 0; i < nbr.carrier.size(); ++i) nbr.carrier[i] = -carrier[i];
    nbr.confidence = Plane(h, w, 1.0F);

    HvdConfig cfg_3d = cfg;
    cfg_3d.temporal_strength = 0.5F;
    cfg_3d.temporal_eps = 4.0F;

    const hvd::RefineResult ref3d = hvd::VariationalRefine(
        s, carrier, init.chroma, cfg_3d, {nbr});
    CHECK_NEAR(MaxResidual(s, ref3d.luma, ref3d.chroma, carrier), 0.0, 1e-3);
  }
}

TEST_MAIN()
