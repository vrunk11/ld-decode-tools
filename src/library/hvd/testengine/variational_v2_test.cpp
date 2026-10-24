// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the THEORY 9e/9f variational-solver additions:
//   * diag_prior — the oriented +/-45 deg chroma prior: the solve must stay
//     numerically sane, preserve the lossless split (Y is eliminated, so
//     the identity holds exactly by construction — checked anyway as the
//     purity contract), and actually change the chroma (a non-zero mu_d
//     that changed nothing would be the "declared but does nothing" trap).
//   * cg_tol / fast — the relative-tolerance early exit and the fast-mode
//     budget cap must not move the solution meaningfully: fast is "same
//     algorithm, cheaper logistics", so its result must stay close to the
//     slow solve on the same input.

#include <algorithm>
#include <cmath>
#include <vector>

#include "check.h"
#include "engine/ntsc_geometry.h"
#include "engine/variational.h"

namespace {

using hvd::Complex;
using hvd::ComplexPlane;
using hvd::FieldGeometry;
using hvd::HvdConfig;
using hvd::Plane;

float MaxResidual(const Plane& s, const Plane& luma, const ComplexPlane& chroma,
                  const ComplexPlane& carrier) {
  float m = 0.0F;
  for (size_t i = 0; i < s.size(); ++i) {
    const float recon = luma[i] + (chroma[i] * carrier[i]).real();
    m = std::max(m, std::fabs(s[i] - recon));
  }
  return m;
}

double RmsDiff(const ComplexPlane& a, const ComplexPlane& b) {
  double acc = 0.0;
  for (size_t i = 0; i < a.size(); ++i) acc += std::norm(a[i] - b[i]);
  return std::sqrt(acc / static_cast<double>(a.size()));
}

bool AllFinite(const Plane& p) {
  for (size_t i = 0; i < p.size(); ++i)
    if (!std::isfinite(p[i])) return false;
  return true;
}

struct Scene {
  Plane s;
  ComplexPlane carrier;
  ComplexPlane chi0;
};

// A composite with DIAGONAL chroma structure (the content class the
// oriented prior exists for) plus luma texture. chi0 is the ground truth
// perturbed by a deterministic corruption pattern — a stand-in for the
// holographic init kept FFTW-free on purpose, so this test exercises only
// the solver (and stays runnable in environments without fftw3f).
Scene MakeScene(const FieldGeometry& g, int h, int w) {
  std::vector<float> theta(h, 0.0F);
  Scene sc;
  sc.carrier = hvd::MakeCarrier(theta, g);

  Plane y_true(h, w);
  ComplexPlane chi_true(h, w);
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      y_true.at(r, c) =
          30.0F + 20.0F * std::sin(0.35F * static_cast<float>(r + c));
      const float diag = std::sin(0.4F * static_cast<float>(c - r));
      chi_true.at(r, c) = Complex{10.0F * diag, -6.0F * diag};
    }
  }
  sc.s = Plane(h, w);
  for (size_t i = 0; i < sc.s.size(); ++i)
    sc.s[i] = y_true[i] + (chi_true[i] * sc.carrier[i]).real();

  sc.chi0 = ComplexPlane(h, w);
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      // Cross-colour-like corruption for the solver to arbitrate away.
      const float e = 3.0F * std::sin(0.9F * static_cast<float>(c)) *
                      std::cos(0.7F * static_cast<float>(r));
      sc.chi0.at(r, c) = chi_true.at(r, c) + Complex{e, -e};
    }
  }
  return sc;
}

}  // namespace

void RunTests() {
  const int h = 48;
  const int w = 56;
  FieldGeometry g;
  g.active_video_start = 0;
  g.active_video_end = w;

  HvdConfig base;
  base.cg_iterations = 60;

  const Scene sc = MakeScene(g, h, w);

  // Baseline (no oriented prior, slow mode).
  const hvd::RefineResult slow =
      hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, base);
  CHECK(AllFinite(slow.luma));
  CHECK_NEAR(MaxResidual(sc.s, slow.luma, slow.chroma, sc.carrier), 0.0, 1e-3);

  // --- diag_prior: sane, lossless, and actually doing something -----------
  {
    HvdConfig cfg = base;
    cfg.diag_prior = 0.7F;
    const hvd::RefineResult diag =
        hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, cfg);
    CHECK(AllFinite(diag.luma));
    CHECK_NEAR(MaxResidual(sc.s, diag.luma, diag.chroma, sc.carrier), 0.0, 1e-3);
    const double moved = RmsDiff(diag.chroma, slow.chroma);
    CHECK(moved > 1e-4);  // the oriented terms are live, not declared-only
    CHECK(moved < 10.0);  // ...and renormalisation keeps them a redistribution

    // Same knob through the joint solver (anchor-pass path).
    Plane y0(h, w);
    for (size_t i = 0; i < y0.size(); ++i)
      y0[i] = sc.s[i] - (sc.chi0[i] * sc.carrier[i]).real();
    const hvd::RefineResult joint = hvd::VariationalRefineJoint(
        sc.s, sc.carrier, y0, sc.chi0, cfg, {}, nullptr);
    CHECK(AllFinite(joint.luma));
    CHECK(RmsDiff(joint.chroma, sc.chi0) < 50.0);  // bounded, finite move
  }

  // --- fast mode: same algorithm, close result ----------------------------
  {
    HvdConfig cfg = base;
    cfg.fast = true;
    const hvd::RefineResult fast =
        hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, cfg);
    CHECK(AllFinite(fast.luma));
    CHECK_NEAR(MaxResidual(sc.s, fast.luma, fast.chroma, sc.carrier), 0.0, 1e-3);
    // "<= 0.2 dB, no perceptible change": on this synthetic scene the two
    // solves should agree to well under an IRE in RMS.
    CHECK(RmsDiff(fast.chroma, slow.chroma) < 1.0);
  }

  // --- explicit cg_tol: a loose tolerance still converges sanely ----------
  {
    HvdConfig cfg = base;
    cfg.cg_tol = 0.2F;
    const hvd::RefineResult loose =
        hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, cfg);
    CHECK(AllFinite(loose.luma));
    CHECK(RmsDiff(loose.chroma, slow.chroma) < 2.0);
  }
}

TEST_MAIN()
