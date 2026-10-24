// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/variational.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/gradients.h"

namespace hvd {

namespace {

// ---- small elementwise helpers (kept local; they read like the NumPy) -----

// Y = S - Re[chi * carrier].
Plane Luma(const Plane& s, const ComplexPlane& chi,
           const ComplexPlane& carrier) {
  Plane y(s.height(), s.width());
  for (size_t i = 0; i < s.size(); ++i)
    y[i] = s[i] - (chi[i] * carrier[i]).real();
  return y;
}

// Elementwise real product a .* b (both real planes).
Plane Mul(const Plane& a, const Plane& b) {
  Plane out(a.height(), a.width());
  for (size_t i = 0; i < a.size(); ++i) out[i] = a[i] * b[i];
  return out;
}

// Elementwise scale of a complex plane by a real plane: a .* w.
ComplexPlane MulReal(const ComplexPlane& a, const Plane& w) {
  ComplexPlane out(a.height(), a.width());
  for (size_t i = 0; i < a.size(); ++i) out[i] = a[i] * w[i];
  return out;
}

// Charbonnier diffusivity  eps / sqrt(g^2 + eps^2), applied elementwise to a
// gradient plane. Returns a weight plane in (0, 1].
Plane CharbonnierWeight(const Plane& g, float eps) {
  Plane w(g.height(), g.width());
  const float e2 = eps * eps;
  for (size_t i = 0; i < g.size(); ++i)
    w[i] = eps / std::sqrt(g[i] * g[i] + e2);
  return w;
}

// Coupled chroma diffusivity  eps_c / sqrt(c^2 + k*gY^2 + eps_c^2), with c the
// magnitude of the chroma gradient and gY the co-located luma gradient.
Plane CoupledWeight(const Plane& c_abs, const Plane& g_luma, float eps_c,
                    float k) {
  Plane w(c_abs.height(), c_abs.width());
  const float e2 = eps_c * eps_c;
  for (size_t i = 0; i < c_abs.size(); ++i) {
    const float denom = c_abs[i] * c_abs[i] + k * g_luma[i] * g_luma[i] + e2;
    w[i] = eps_c / std::sqrt(denom);
  }
  return w;
}

// |Dx chi| etc.: magnitude of a complex plane, elementwise.
Plane Magnitude(const ComplexPlane& a) {
  Plane out(a.height(), a.width());
  for (size_t i = 0; i < a.size(); ++i) out[i] = std::abs(a[i]);
  return out;
}

// sum(|g|^2) over a complex plane (the real CG residual norm).
double SumAbs2(const ComplexPlane& g) {
  double acc = 0.0;
  const long n = static_cast<long>(g.size());
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
  for (long i = 0; i < n; ++i)
    acc += static_cast<double>(g[i].real()) * g[i].real() +
           static_cast<double>(g[i].imag()) * g[i].imag();
  return acc;
}

// Re(sum(conj(a) * b)) over complex planes.
double DotReal(const ComplexPlane& a, const ComplexPlane& b) {
  double acc = 0.0;
  const long n = static_cast<long>(a.size());
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
  for (long i = 0; i < n; ++i)
    acc += static_cast<double>(a[i].real()) * b[i].real() +
           static_cast<double>(a[i].imag()) * b[i].imag();
  return acc;
}

// sum(a^2) over a real plane.
double SumSq(const Plane& a) {
  double acc = 0.0;
  const long n = static_cast<long>(a.size());
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
  for (long i = 0; i < n; ++i) acc += static_cast<double>(a[i]) * a[i];
  return acc;
}

// sum(a .* b) over real planes.
double DotRealPlane(const Plane& a, const Plane& b) {
  double acc = 0.0;
  const long n = static_cast<long>(a.size());
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
  for (long i = 0; i < n; ++i) acc += static_cast<double>(a[i]) * b[i];
  return acc;
}

}  // namespace

// chroma_aniso == 0 => AUTO: resolve the horizontal/vertical chroma prior
// split from the INIT's dominant chroma-detail orientation. Measured on
// the two real-footage failure classes: thin HORIZONTAL chroma bands and
// fsc-adjacent texture want the vertical prior weak (aniso -> 1.0: lines
// ~15% better, curtain ~17%), while sharp HORIZONTAL chroma transitions
// (colour bars, graphics) want the reference's 0.5 (a fixed 1.0 costs
// the test chart 1.1 dB in 2D). High quantiles (p90) of |D chi0| carry
// the structure — medians only see the flat background. Any positive
// cfg value forces that fixed aniso (reference behaviour at 0.5).
static float ResolveChromaAniso(const ComplexPlane& chi0,
                                const HvdConfig& cfg) {
  if (cfg.chroma_aniso > 0.0F) return cfg.chroma_aniso;
  // Measure on LINE-GROUP AVERAGES, not raw chi0: the init's dominant
  // vertical energy is cross-colour LEAK, which alternates line to line
  // — on a colour-bar chart that alternation made |Dy chi0| read as
  // large as the bar transitions themselves (py ~ px measured) and auto
  // wrongly picked ~1.0. Averaging cancels the leak while genuine chroma
  // structure (slow vertically, and even a 1-frame-line feature at
  // reduced amplitude) survives.
  //
  // The GROUP SIZE is standard-dependent, and this is not cosmetic:
  //   NTSC: conj(c) alternates SIGN only (carrier flips 180 deg per
  //         field line) -> a 2-line average cancels the leak exactly.
  //   PAL : conj(c) alternates sign AND CONJUGATION (the V-switch is a
  //         conjugated reference wave — reference-pal/THEORY-PAL.md
  //         section 1), and the carrier walks 270 deg/line. A 2-line
  //         average does NOT cancel it; a 4-line average does (the
  //         conjugation pattern repeats over 2 lines, and 4 * 270 =
  //         1080 = 0 mod 360 closes the carrier walk).
  const int group = cfg.is_pal ? 4 : 2;
  const int Hh = chi0.height() / group;
  const int Wc = chi0.width();
  std::vector<float> gx;
  std::vector<float> gy;
  gx.reserve(static_cast<size_t>(Hh) * Wc / 9 + 1);
  gy.reserve(gx.capacity());
  auto pav = [&](int t, int x) {
    Complex acc{0.0F, 0.0F};
    for (int k = 0; k < group; ++k) acc += chi0.at(group * t + k, x);
    return acc * (1.0F / static_cast<float>(group));
  };
  for (int t = 1; t < Hh; t += 2) {
    for (int x = 1; x < Wc; x += 3) {
      gx.push_back(std::abs(pav(t, x) - pav(t, x - 1)));
      gy.push_back(std::abs(pav(t, x) - pav(t - 1, x)));
    }
  }
  // p98, not p90: structure can be SPARSE — on a colour-bar chart the
  // vertical transitions occupy ~4% of columns, invisible to a p90 (which
  // then reads noise/noise ~ 1 and wrongly picks aniso ~ 1, costing the
  // chart 1.1 dB); the top 2% still captures it, while line/stripe scenes
  // put 20-30% of samples on structure and don't care which quantile.
  auto p98 = [](std::vector<float>& v) {
    if (v.empty()) return 0.0F;
    const size_t k = v.size() - 1 - v.size() / 50;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
  };
  const float px = p98(gx);
  const float py = p98(gy);
  const float r = py / std::max(px, 1e-6F);
  if (std::getenv("HVD_DEBUG_ANISO")) {
    std::fprintf(stderr, "aniso auto: px=%.2f py=%.2f r=%.2f\n", px, py, r);
  }
  // Map calibrated on measured ratios: the colour-bar chart (wants 0.5)
  // reads r ~ 1.1 — its vertical p98 is init cross-colour leak, which
  // pair-averaging only partly cancels (the Dubois init's leak is not a
  // pure line-alternation) — while the thin-line/curtain scenes (want
  // 1.0) read r ~ 1.6-1.9. The 1.3 knee sits between the two families.
  // PAL carries U and V at the same ~1.3 MHz (no NTSC I/Q axis
  // asymmetry), so its floor sits nearer isotropy: the research maps
  // into [0.55, 1.0] with a gentler slope (reference-pal/THEORY-PAL.md
  // section 3). NTSC keeps the measured-on-real-footage 0.5 knee.
  if (cfg.is_pal) return std::clamp(0.7F + 0.9F * (r - 1.3F), 0.55F, 1.0F);
  return std::clamp(0.5F + 1.1F * (r - 1.3F), 0.5F, 1.0F);
}


// The solver for a field with no neighbours and no oriented prior (the 2D decode), with the maths of VariationalRefine in four passes over the
// field per IRLS round instead of some forty: the luma and the gradients and the weights and their products come out of one pass, the transposed
// differences and the gradient of the next, the curvature of the one after, and what CG does not read again is not computed (the last gradient of
// the last inner step, the luma and the direction that a pass can make as it goes). Each element is computed as it is there, so the result is the
// same to rounding. The loops are the same along every line, with the ends of the line and of the column handled by what the neighbour is (the
// same element for a forward difference, a zero for a transposed one: the value is the same), and the complex planes are read as floats, real
// and imaginary part by part (the compiler does not vectorise std::complex), so they vectorise.
static RefineResult RefineDecoupled(const Plane& s, const ComplexPlane& carrier,
                                    const ComplexPlane& chi0, const HvdConfig& cfg,
                                    float mu_h, float mu_v) {
  const float eps = cfg.charbonnier_eps;
  const float eps_c = cfg.chroma_eps;
  const float k = cfg.structure_coupling;
  const float e2 = eps * eps;
  const float ec2 = eps_c * eps_c;
  const int irls_outer = std::max(1, cfg.irls_outer);
  const int cg_total = cfg.fast ? cfg.cg_iterations * 2 / 3 : cfg.cg_iterations;
  const int n_inner = std::max(1, cg_total / irls_outer);
  const float cg_tol = cfg.cg_tol > 0.0F ? cfg.cg_tol : (cfg.fast ? 0.10F : 0.02F);

  const int H = s.height();
  const int W = s.width();
  const long N = static_cast<long>(s.size());

  ComplexPlane chi = chi0;
  Plane wx(H, W), wy(H, W), wcx(H, W), wcy(H, W), ux(H, W), uy(H, W);
  ComplexPlane vx(H, W), vy(H, W), g(H, W), g_new(H, W), d(H, W);
  const std::vector<float> zero_row(static_cast<size_t>(W), 0.0F);
  const std::vector<float> zero_row_c(static_cast<size_t>(2 * W), 0.0F);

  const float* sp = s.data();
  const float* car = reinterpret_cast<const float*>(carrier.data());  // real and imaginary part, one after the other

  // The gradient of the objective at c, and the sum of its squares. With `weights` the IRLS weights are made at c on the way (the first
  // gradient of a round); without, the ones of the round are kept (the later steps of CG). With `negated` the direction -out is written too.
  auto grad = [&](const ComplexPlane& c, bool weights, ComplexPlane& out, ComplexPlane* negated) -> double {
    const float* cp = reinterpret_cast<const float*>(c.data());
    float* wxp = wx.data();
    float* wyp = wy.data();
    float* wcxp = wcx.data();
    float* wcyp = wcy.data();
    float* uxp = ux.data();
    float* uyp = uy.data();
    float* vxp = reinterpret_cast<float*>(vx.data());
    float* vyp = reinterpret_cast<float*>(vy.data());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < H; ++y) {
      const long row = static_cast<long>(y) * W;
      // The luma of this line and of the next (the last line has itself: its difference is 0)
      std::vector<float> ycv(static_cast<size_t>(2 * W));
      float* yc0 = ycv.data();
      float* yc1 = yc0 + W;
      const long next = y + 1 < H ? row + W : row;
      HVD_SIMD
      for (int x = 0; x < W; ++x) {
        yc0[x] = sp[row + x] - (cp[2 * (row + x)] * car[2 * (row + x)] - cp[2 * (row + x) + 1] * car[2 * (row + x) + 1]);
        yc1[x] = sp[next + x] - (cp[2 * (next + x)] * car[2 * (next + x)] - cp[2 * (next + x) + 1] * car[2 * (next + x) + 1]);
      }
      const float* c0 = cp + 2 * row;
      const float* c1 = cp + 2 * next;
      float* wxr = wxp + row;
      float* wyr = wyp + row;
      float* wcxr = wcxp + row;
      float* wcyr = wcyp + row;
      float* uxr = uxp + row;
      float* uyr = uyp + row;
      float* vxr = vxp + 2 * row;
      float* vyr = vyp + 2 * row;
      if (weights) {
      HVD_SIMD
        for (int x = 0; x < W - 1; ++x) {
          const float gx = yc0[x + 1] - yc0[x];
          const float gy = yc1[x] - yc0[x];
          const float dxr = c0[2 * (x + 1)] - c0[2 * x];
          const float dxi = c0[2 * (x + 1) + 1] - c0[2 * x + 1];
          const float dyr = c1[2 * x] - c0[2 * x];
          const float dyi = c1[2 * x + 1] - c0[2 * x + 1];
          const float wxv = eps / std::sqrt(gx * gx + e2);
          const float wyv = eps / std::sqrt(gy * gy + e2);
          const float wcxv = eps_c / std::sqrt((dxr * dxr + dxi * dxi) + k * gx * gx + ec2);
          const float wcyv = eps_c / std::sqrt((dyr * dyr + dyi * dyi) + k * gy * gy + ec2);
          wxr[x] = wxv;
          wyr[x] = wyv;
          wcxr[x] = wcxv;
          wcyr[x] = wcyv;
          uxr[x] = wxv * gx;
          uyr[x] = wyv * gy;
          vxr[2 * x] = dxr * wcxv;
          vxr[2 * x + 1] = dxi * wcxv;
          vyr[2 * x] = dyr * wcyv;
          vyr[2 * x + 1] = dyi * wcyv;
        }
      } else {
      HVD_SIMD
        for (int x = 0; x < W - 1; ++x) {
          const float gx = yc0[x + 1] - yc0[x];
          const float gy = yc1[x] - yc0[x];
          uxr[x] = wxr[x] * gx;
          uyr[x] = wyr[x] * gy;
          vxr[2 * x] = (c0[2 * (x + 1)] - c0[2 * x]) * wcxr[x];
          vxr[2 * x + 1] = (c0[2 * (x + 1) + 1] - c0[2 * x + 1]) * wcxr[x];
          vyr[2 * x] = (c1[2 * x] - c0[2 * x]) * wcyr[x];
          vyr[2 * x + 1] = (c1[2 * x + 1] - c0[2 * x + 1]) * wcyr[x];
        }
      }
      {
        // The last column: no difference along the line
        const int x = W - 1;
        const float gy = yc1[x] - yc0[x];
        const float dyr = c1[2 * x] - c0[2 * x];
        const float dyi = c1[2 * x + 1] - c0[2 * x + 1];
        if (weights) {
          wxr[x] = eps / std::sqrt(e2);
          wyr[x] = eps / std::sqrt(gy * gy + e2);
          wcxr[x] = eps_c / std::sqrt(ec2);
          wcyr[x] = eps_c / std::sqrt((dyr * dyr + dyi * dyi) + k * gy * gy + ec2);
        }
        uxr[x] = 0.0F;
        uyr[x] = wyr[x] * gy;
        vxr[2 * x] = 0.0F;
        vxr[2 * x + 1] = 0.0F;
        vyr[2 * x] = dyr * wcyr[x];
        vyr[2 * x + 1] = dyi * wcyr[x];
      }
    }

    // out = -[2 (DxT(wx Dx yc) + DyT(wy Dy yc))] * conj(carrier) + 2 (mu_h DxT(wcx Dx c) + mu_v DyT(wcy Dy c))
    float* op = reinterpret_cast<float*>(out.data());
    float* np = negated ? reinterpret_cast<float*>(negated->data()) : nullptr;
    double gg = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : gg) schedule(static)
#endif
    for (int y = 0; y < H; ++y) {
      const long row = static_cast<long>(y) * W;
      // DyT along the column: the line before (0 for the first) minus this one (0 for the last)
      const float* uyPrev = y > 0 ? uyp + row - W : zero_row.data();
      const float* uyCur = y + 1 < H ? uyp + row : zero_row.data();
      const float* vyPrev = y > 0 ? vyp + 2 * (row - W) : zero_row_c.data();
      const float* vyCur = y + 1 < H ? vyp + 2 * row : zero_row_c.data();
      const float* uxr = uxp + row;
      const float* vxr = vxp + 2 * row;
      const float* carr = car + 2 * row;
      float* orow = op + 2 * row;
      float* nrow = np ? np + 2 * row : nullptr;
      // DxT along the line: the element before (0 for the first) minus this one (0 for the last)
      auto edge = [&](int x, float tux, float tvxr, float tvxi) {
        const float tuy = uyPrev[x] - uyCur[x];
        const float tvyr = vyPrev[2 * x] - vyCur[2 * x];
        const float tvyi = vyPrev[2 * x + 1] - vyCur[2 * x + 1];
        const float img = 2.0F * (tux + tuy);
        const float our = -img * carr[2 * x] + 2.0F * (mu_h * tvxr + mu_v * tvyr);
        const float oui = img * carr[2 * x + 1] + 2.0F * (mu_h * tvxi + mu_v * tvyi);
        orow[2 * x] = our;
        orow[2 * x + 1] = oui;
        if (nrow) {
          nrow[2 * x] = -our;
          nrow[2 * x + 1] = -oui;
        }
        return static_cast<double>(our) * our + static_cast<double>(oui) * oui;
      };
      double rowgg = edge(0, -uxr[0], -vxr[0], -vxr[1]);
      rowgg += edge(W - 1, uxr[W - 2], vxr[2 * (W - 2)], vxr[2 * (W - 2) + 1]);
      if (nrow) {
      HVD_SIMD_REDUCE_ADD(rowgg)
        for (int x = 1; x < W - 1; ++x) {
          const float tuy = uyPrev[x] - uyCur[x];
          const float tvyr = vyPrev[2 * x] - vyCur[2 * x];
          const float tvyi = vyPrev[2 * x + 1] - vyCur[2 * x + 1];
          const float img = 2.0F * ((uxr[x - 1] - uxr[x]) + tuy);
          const float our = -img * carr[2 * x] + 2.0F * (mu_h * (vxr[2 * (x - 1)] - vxr[2 * x]) + mu_v * tvyr);
          const float oui = img * carr[2 * x + 1] + 2.0F * (mu_h * (vxr[2 * (x - 1) + 1] - vxr[2 * x + 1]) + mu_v * tvyi);
          orow[2 * x] = our;
          orow[2 * x + 1] = oui;
          nrow[2 * x] = -our;
          nrow[2 * x + 1] = -oui;
          rowgg += static_cast<double>(our) * our + static_cast<double>(oui) * oui;
        }
      } else {
      HVD_SIMD_REDUCE_ADD(rowgg)
        for (int x = 1; x < W - 1; ++x) {
          const float tuy = uyPrev[x] - uyCur[x];
          const float tvyr = vyPrev[2 * x] - vyCur[2 * x];
          const float tvyi = vyPrev[2 * x + 1] - vyCur[2 * x + 1];
          const float img = 2.0F * ((uxr[x - 1] - uxr[x]) + tuy);
          const float our = -img * carr[2 * x] + 2.0F * (mu_h * (vxr[2 * (x - 1)] - vxr[2 * x]) + mu_v * tvyr);
          const float oui = img * carr[2 * x + 1] + 2.0F * (mu_h * (vxr[2 * (x - 1) + 1] - vxr[2 * x + 1]) + mu_v * tvyi);
          orow[2 * x] = our;
          orow[2 * x + 1] = oui;
          rowgg += static_cast<double>(our) * our + static_cast<double>(oui) * oui;
        }
      }
      gg += rowgg;
    }
    return gg;
  };

  // The curvature of the objective along dir (the weights are the ones of the round)
  auto curv = [&](const ComplexPlane& dir) -> double {
    const float* dp = reinterpret_cast<const float*>(dir.data());
    const float* wxp = wx.data();
    const float* wyp = wy.data();
    const float* wcxp = wcx.data();
    const float* wcyp = wcy.data();
    double h = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : h) schedule(static)
#endif
    for (int y = 0; y < H; ++y) {
      const long row = static_cast<long>(y) * W;
      const long next = y + 1 < H ? row + W : row;
      std::vector<float> dyv(static_cast<size_t>(2 * W));
      float* dy0 = dyv.data();
      float* dy1 = dy0 + W;
      HVD_SIMD
      for (int x = 0; x < W; ++x) {
        dy0[x] = -(dp[2 * (row + x)] * car[2 * (row + x)] - dp[2 * (row + x) + 1] * car[2 * (row + x) + 1]);
        dy1[x] = -(dp[2 * (next + x)] * car[2 * (next + x)] - dp[2 * (next + x) + 1] * car[2 * (next + x) + 1]);
      }
      const float* d0 = dp + 2 * row;
      const float* d1 = dp + 2 * next;
      const float* wxr = wxp + row;
      const float* wyr = wyp + row;
      const float* wcxr = wcxp + row;
      const float* wcyr = wcyp + row;
      double rowh = 0.0;
      HVD_SIMD_REDUCE_ADD(rowh)
      for (int x = 0; x < W - 1; ++x) {
        const float dxdy = dy0[x + 1] - dy0[x];
        const float dydy = dy1[x] - dy0[x];
        const float dxr = d0[2 * (x + 1)] - d0[2 * x];
        const float dxi = d0[2 * (x + 1) + 1] - d0[2 * x + 1];
        const float dyr = d1[2 * x] - d0[2 * x];
        const float dyi = d1[2 * x + 1] - d0[2 * x + 1];
        rowh += static_cast<double>(wxr[x]) * dxdy * dxdy;
        rowh += static_cast<double>(wyr[x]) * dydy * dydy;
        rowh += static_cast<double>(mu_h) * wcxr[x] * (dxr * dxr + dxi * dxi);
        rowh += static_cast<double>(mu_v) * wcyr[x] * (dyr * dyr + dyi * dyi);
      }
      {
        const int x = W - 1;
        const float dydy = dy1[x] - dy0[x];
        const float dyr = d1[2 * x] - d0[2 * x];
        const float dyi = d1[2 * x + 1] - d0[2 * x + 1];
        rowh += static_cast<double>(wyr[x]) * dydy * dydy;
        rowh += static_cast<double>(mu_v) * wcyr[x] * (dyr * dyr + dyi * dyi);
      }
      h += rowh;
    }
    return h;
  };

  const double n_pixels = static_cast<double>(N);
  Complex* chip = chi.data();
  Complex* dpw = d.data();
  for (int outer = 0; outer < irls_outer; ++outer) {
    double gg = grad(chi, true, g, &d);
    const double gg0 = gg;

    for (int it = 0; it < n_inner; ++it) {
      const double hh = curv(d);
      if (hh <= 1e-12) break;
      double gd = 0.0;
      if (it == 0) {
        // The direction is -g: their product is minus the sum of the squares of g
        gd = -gg;
      } else {
        const Complex* gp = g.data();
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : gd) schedule(static)
#endif
        for (long i = 0; i < N; ++i)
          gd += static_cast<double>(gp[i].real()) * dpw[i].real() + static_cast<double>(gp[i].imag()) * dpw[i].imag();
      }
      const float alpha = static_cast<float>(-0.5 * gd / hh);
      {
        float* cf = reinterpret_cast<float*>(chip);
        const float* df = reinterpret_cast<const float*>(dpw);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < 2 * N; ++i) cf[i] += alpha * df[i];
      }
      // Nothing reads the gradient after the last step of a round: the next round makes its own at the new chi
      if (it == n_inner - 1) break;

      const double gg_new = grad(chi, false, g_new, nullptr);
      const float beta = static_cast<float>(gg_new / std::max(gg, 1e-30));
      {
        const Complex* gn = g_new.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < N; ++i) dpw[i] = -gn[i] + beta * dpw[i];
      }
      std::swap(g, g_new);
      gg = gg_new;
      if (gg < static_cast<double>(cg_tol) * cg_tol * gg0) break;
      if (gg < 1e-10 * n_pixels) break;
    }
  }

  RefineResult out;
  out.luma = Luma(s, chi, carrier);
  out.chroma = std::move(chi);
  return out;
}

RefineResult VariationalRefine(const Plane& s, const ComplexPlane& carrier,
                               const ComplexPlane& chi0, const HvdConfig& cfg,
                               const std::vector<NeighborTerm>& neighbors) {
  const float eps = cfg.charbonnier_eps;
  const float eps_c = cfg.chroma_eps;
  const float eps_t = cfg.temporal_eps;
  const float aniso = ResolveChromaAniso(chi0, cfg);
  float mu_h = cfg.lambda_c * aniso;  // chroma broader in x
  float mu_v = cfg.lambda_c;
  // Oriented (+/-45 deg) chroma prior weight, distance-normalised by 1/2
  // (a diagonal step is sqrt(2) px). Renormalise so the TOTAL chroma prior
  // mass is unchanged by the oriented terms: they redistribute smoothing
  // across directions (isotropy against diagonal cross-colour) without
  // increasing it. (THEORY 9e; exact port of the reference's block.)
  float mu_d = cfg.lambda_c * cfg.diag_prior * 0.5F;
  {
    const float tot0 = cfg.lambda_c * (1.0F + aniso);
    const float scale = mu_d > 0.0F ? tot0 / (tot0 + 2.0F * mu_d) : 1.0F;
    mu_h *= scale;
    mu_v *= scale;
    mu_d *= scale;
  }
  if (neighbors.empty() && !(mu_d > 0.0F) && chi0.height() >= 2 && chi0.width() >= 2) {
    return RefineDecoupled(s, carrier, chi0, cfg, mu_h, mu_v);
  }
  const float k = cfg.structure_coupling;
  const float nu = neighbors.empty() ? 0.0F : cfg.temporal_strength;
  const size_t n_nbr = neighbors.size();

  const int irls_outer = std::max(1, cfg.irls_outer);
  // Fast mode caps the total CG budget at 2/3 (the adaptive tol below is
  // what actually saves the iterations; the cap is the safety net).
  const int cg_total = cfg.fast ? cfg.cg_iterations * 2 / 3 : cfg.cg_iterations;
  const int n_inner = std::max(1, cg_total / irls_outer);
  // CG relative gradient-norm early-exit (0 = auto, reference values).
  const float cg_tol = cfg.cg_tol > 0.0F ? cfg.cg_tol : (cfg.fast ? 0.10F : 0.02F);

  const int H = s.height();
  const int W = s.width();
  const size_t N = s.size();
  const long n = static_cast<long>(N);

  ComplexPlane chi = chi0;  // working copy

  // ---- Workspace: every frame-sized buffer the hot loops need, allocated
  // ONCE here instead of once per grad()/curv() call. The previous version
  // allocated ~15 frame-sized planes per CG iteration (~900 allocations per
  // frame at cg_iterations=60), which dominated the runtime. The maths below
  // is unchanged; only the memory traffic is.
  Plane wx(H, W), wy(H, W), wcx(H, W), wcy(H, W);
  Plane wd1(H, W), wd2(H, W);  // oriented-prior diffusivities (mu_d > 0 only)
  Plane prior_scale(H, W, 1.0F);  // data/prior rebalancing factor (3-D only)
  Plane yc(H, W), tmpr1(H, W), tmpr2(H, W), img(H, W);
  Plane gxY(H, W), gyY(H, W), magx(H, W), magy(H, W);
  ComplexPlane tmpc1(H, W), tmpc2(H, W), tmpc3(H, W), cprior(H, W);
  ComplexPlane g(H, W), g_new(H, W), d(H, W);
  Plane dy(H, W), dxdy(H, W), dydy(H, W);
  ComplexPlane dxc(H, W), dyc(H, W);
  Plane rt(H, W);  // per-neighbour temporal residual scratch

  // Per-neighbour delta dc = carrier - c_w and IRLS weight wt, precomputed
  // once (dc is fixed for the whole solve) / recomputed per outer pass
  // (wt is lagged-diffusivity, like wx/wy/wcx/wcy).
  std::vector<ComplexPlane> dcs(n_nbr);
  std::vector<Plane> wts(n_nbr);
  for (size_t j = 0; j < n_nbr; ++j) {
    dcs[j] = ComplexPlane(H, W);
    wts[j] = Plane(H, W);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) dcs[j][i] = carrier[i] - neighbors[j].carrier[i];
  }

  // temporal_residual(chi, S_w, dc) = (S_w - S) + Re[chi * dc]  — the
  // reference's algebraic reformulation of S_w - Y - Re[chi*c_w] after
  // substituting Y = S - Re[chi*carrier] (see variational.h's doc comment
  // on this function for the derivation). Written into `rt` (reused
  // scratch), not returned, to avoid an allocation per neighbour per call.
  auto temporal_residual_into = [&](const ComplexPlane& c, const NeighborTerm& nbr,
                                    const ComplexPlane& dc) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i)
      rt[i] = (nbr.composite[i] - s[i]) + (c[i] * dc[i]).real();
  };

  // g = -[2 (DxT(wx Dx Yc) + DyT(wy Dy Yc))] * conj(carrier)
  //     + 2 (mu_h DxT(wcx Dx chi) + mu_v DyT(wcy Dy chi))
  //     + 2 * nu * sum_k wt_k * temporal_residual_k * conj(dc_k)
  auto grad = [&](const ComplexPlane& c, ComplexPlane& out) {
    // Yc = S - Re[c * carrier]
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) yc[i] = s[i] - (c[i] * carrier[i]).real();

    // img = 2 * (DxT(wx .* Dx(yc)) + DyT(wy .* Dy(yc)))
    DxWeightedInto(yc, wx, tmpr1);
    DxTInto(tmpr1, img);

    DyWeightedInto(yc, wy, tmpr2);
    DyTInto(tmpr2, tmpr1);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) img[i] = 2.0F * (img[i] + tmpr1[i]);

    // cprior = 2 * (mu_h * DxT(wcx .* Dx(c)) + mu_v * DyT(wcy .* Dy(c)))
    DxWeightedInto(c, wcx, tmpc1);
    DxTInto(tmpc1, cprior);

    DyWeightedInto(c, wcy, tmpc2);
    DyTInto(tmpc2, tmpc3);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i)
      cprior[i] = 2.0F * (mu_h * cprior[i] + mu_v * tmpc3[i]);

    // Oriented terms: + 2 * mu_d * (D1T(wd1 .* D1(c)) + D2T(wd2 .* D2(c))).
    // tmpc1..3 are free again by this point (their h/v contributions have
    // been folded into cprior above).
    if (mu_d > 0.0F) {
      D1WeightedInto(c, wd1, tmpc1);
      D1TInto(tmpc1, tmpc2);
      D2WeightedInto(c, wd2, tmpc1);
      D2TInto(tmpc1, tmpc3);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i)
        cprior[i] += 2.0F * mu_d * (tmpc2[i] + tmpc3[i]);
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i)
      out[i] = -img[i] * std::conj(carrier[i]) + cprior[i];

    for (size_t j = 0; j < n_nbr; ++j) {
      temporal_residual_into(c, neighbors[j], dcs[j]);
      const Plane& wt = wts[j];
      const ComplexPlane& dc = dcs[j];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i)
        out[i] += 2.0F * nu * wt[i] * rt[i] * std::conj(dc[i]);
    }
  };

  // H = sum wx (Dx dY)^2 + wy (Dy dY)^2
  //   + mu_h sum wcx |Dx dC|^2 + mu_v sum wcy |Dy dC|^2,   dY = -Re[dC c].
  //   + nu * sum_k sum wt_k * Re[dC * dc_k]^2
  auto curv = [&](const ComplexPlane& dc_dir) -> double {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) dy[i] = -(dc_dir[i] * carrier[i]).real();
    DxInto(dy, dxdy);
    DyInto(dy, dydy);
    DxInto(dc_dir, dxc);
    DyInto(dc_dir, dyc);
    double h = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : h) schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      h += static_cast<double>(wx[i]) * dxdy[i] * dxdy[i];
      h += static_cast<double>(wy[i]) * dydy[i] * dydy[i];
      h += static_cast<double>(mu_h) * wcx[i] * std::norm(dxc[i]);
      h += static_cast<double>(mu_v) * wcy[i] * std::norm(dyc[i]);
    }
    if (mu_d > 0.0F) {
      // dxc/dyc are consumed above; reuse them for the diagonal directions.
      D1Into(dc_dir, dxc);
      D2Into(dc_dir, dyc);
      double acc_d = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc_d) schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        acc_d += static_cast<double>(wd1[i]) * std::norm(dxc[i]);
        acc_d += static_cast<double>(wd2[i]) * std::norm(dyc[i]);
      }
      h += mu_d * acc_d;
    }
    for (size_t j = 0; j < n_nbr; ++j) {
      const Plane& wt = wts[j];
      const ComplexPlane& dc = dcs[j];
      double acc = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float re = (dc_dir[i] * dc[i]).real();
        acc += static_cast<double>(wt[i]) * re * re;
      }
      h += nu * acc;
    }
    return h;
  };

  const double n_pixels = static_cast<double>(N);
  const float e2 = eps * eps;
  const float ec2 = eps_c * eps_c;
  const float et2 = eps_t * eps_t;

  for (int outer = 0; outer < irls_outer; ++outer) {
    // Recompute IRLS weights at the current chi (lagged diffusivity).
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) yc[i] = s[i] - (chi[i] * carrier[i]).real();
    DxInto(yc, gxY);
    DyInto(yc, gyY);
    DxInto(chi, tmpc1);
    DyInto(chi, tmpc2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      wx[i] = eps / std::sqrt(gxY[i] * gxY[i] + e2);
      wy[i] = eps / std::sqrt(gyY[i] * gyY[i] + e2);
      magx[i] = std::abs(tmpc1[i]);
      magy[i] = std::abs(tmpc2[i]);
    }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      const float dx_den = magx[i] * magx[i] + k * gxY[i] * gxY[i] + ec2;
      const float dy_den = magy[i] * magy[i] + k * gyY[i] * gyY[i] + ec2;
      wcx[i] = eps_c / std::sqrt(dx_den);
      wcy[i] = eps_c / std::sqrt(dy_den);
    }
    if (mu_d > 0.0F) {
      // Diagonal diffusivities: 1 / sqrt(1 + (|D chi| / eps_c)^2), the same
      // Charbonnier shape as wcx/wcy written in the reference's normalised
      // form (equivalent to eps_c / sqrt(|D chi|^2 + eps_c^2)), without the
      // luma structure-coupling term (the reference's oriented prior has
      // none). tmpc1/tmpc2 are free again once magx/magy exist.
      D1Into(chi, tmpc1);
      D2Into(chi, tmpc2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float c1 = std::abs(tmpc1[i]) / eps_c;
        const float c2 = std::abs(tmpc2[i]) / eps_c;
        wd1[i] = 1.0F / std::sqrt(1.0F + c1 * c1);
        wd2[i] = 1.0F / std::sqrt(1.0F + c2 * c2);
      }
    }
    if (nu > 0.0F) {
      for (size_t j = 0; j < n_nbr; ++j) {
        temporal_residual_into(chi, neighbors[j], dcs[j]);
        Plane& wt = wts[j];
        const Plane& conf = neighbors[j].confidence;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < n; ++i)
          wt[i] = conf[i] * et2 / (rt[i] * rt[i] + et2);
      }

      // ---- PRIOR RENORMALISATION (data/prior balance) --------------------
      // Turning 3-D on adds equations to the DATA side of the objective
      // while the spatial priors keep fixed weights, so it silently
      // WEAKENS the priors relative to the data — the effective chroma
      // smoothness drops even where the neighbours contribute nothing
      // useful. That is not a second-order effect: with Y eliminated, a
      // pixel's 2-D curvature in chi is |dC|^2/2 (|carrier| = 1), and each
      // neighbour adds nu*wt_k*|dc_k|^2/2, with |dc_k|^2 = 4 on even
      // offsets (carrier flipped 180 deg) and 2 on odd ones. Six
      // neighbours at typical robust weights therefore multiply the data
      // mass by ~4 — matching the measurement that motivated this: on
      // moving fine texture (hair strands near f_sc, where the chroma
      // prior is doing all the work of suppressing cross-colour), 3-D had
      // to be run with lambda_c ~ 4 just to draw LEVEL with 2-D at
      // lambda_c = 1. The user was compensating this by hand.
      //
      // So scale the prior diffusivities by the same per-pixel factor the
      // data side just gained:
      //     k[i] = 1 + nu * sum_j wt_j[i] * |dc_j[i]|^2
      // Where the robust weights have gated the neighbours off (motion,
      // occlusion, mismatch) k -> 1 and this is exactly the 2-D solve, so
      // the graceful per-pixel fallback to 2-D that the robust weights are
      // supposed to give is now actually TRUE — previously that fallback
      // still ran with under-weighted priors. Where the neighbours are
      // trusted, the priors scale up with them and 3-D adds information
      // without trading away smoothness for it.
      //
      // NOTE: deliberate divergence from reference/hvd/decoder.py, which
      // has no such renormalisation.
      for (long i = 0; i < n; ++i) prior_scale[i] = 1.0F;
      for (size_t j = 0; j < n_nbr; ++j) {
        const Plane& wt = wts[j];
        const ComplexPlane& dc = dcs[j];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < n; ++i)
          prior_scale[i] += nu * wt[i] * std::norm(dc[i]);
      }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float k_i = prior_scale[i];
        wx[i] *= k_i;
        wy[i] *= k_i;
        wcx[i] *= k_i;
        wcy[i] *= k_i;
        if (mu_d > 0.0F) {
          wd1[i] *= k_i;
          wd2[i] *= k_i;
        }
      }
    }

    // Conjugate gradient on the fixed-weight quadratic.
    grad(chi, g);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) d[i] = -g[i];
    double gg = SumAbs2(g);
    const double gg0 = gg;  // for the relative-tolerance early exit

    for (int it = 0; it < n_inner; ++it) {
      const double hh = curv(d);
      if (hh <= 1e-12) break;
      const double gd = DotReal(g, d);
      const float alpha = static_cast<float>(-0.5 * gd / hh);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) chi[i] += alpha * d[i];

      grad(chi, g_new);
      const double gg_new = SumAbs2(g_new);
      const float beta = static_cast<float>(gg_new / std::max(gg, 1e-30));
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) d[i] = -g_new[i] + beta * d[i];
      g = g_new;
      gg = gg_new;
      if (gg < static_cast<double>(cg_tol) * cg_tol * gg0) break;
      if (gg < 1e-10 * n_pixels) break;
    }
  }

  RefineResult out;
  out.luma = Luma(s, chi, carrier);
  out.chroma = std::move(chi);
  return out;
}

RefineResult VariationalRefineJoint(const Plane& s, const ComplexPlane& carrier,
                                    const Plane& y0, const ComplexPlane& chi0,
                                    const HvdConfig& cfg,
                                    const std::vector<NeighborTerm>& neighbors,
                                    const AnchorTerm* anchor) {
  const float eps = cfg.charbonnier_eps;
  const float eps_c = cfg.chroma_eps;
  const float eps_t = cfg.temporal_eps;
  const float aniso = ResolveChromaAniso(chi0, cfg);
  float mu_h = cfg.lambda_c * aniso;
  float mu_v = cfg.lambda_c;
  // Oriented prior weight + total-mass renormalisation, identical to the
  // pass-1 solver (and to the reference, which repeats the block verbatim
  // in both refiners).
  float mu_d = cfg.lambda_c * cfg.diag_prior * 0.5F;
  {
    const float tot0 = cfg.lambda_c * (1.0F + aniso);
    const float scale = mu_d > 0.0F ? tot0 / (tot0 + 2.0F * mu_d) : 1.0F;
    mu_h *= scale;
    mu_v *= scale;
    mu_d *= scale;
  }
  const float k = cfg.structure_coupling;
  const float nu = neighbors.empty() ? 0.0F : cfg.temporal_strength;
  const float nu_a = anchor ? cfg.nr_anchor : 0.0F;

  const int irls_outer = std::max(1, cfg.irls_outer);
  const int cg_total = cfg.fast ? cfg.cg_iterations * 2 / 3 : cfg.cg_iterations;
  const int n_inner = std::max(1, cg_total / irls_outer);
  const float cg_tol = cfg.cg_tol > 0.0F ? cfg.cg_tol : (cfg.fast ? 0.10F : 0.02F);

  const int H = s.height();
  const int W = s.width();
  const size_t N = s.size();
  const size_t n_nbr = neighbors.size();

  Plane Y = y0;
  ComplexPlane chi = chi0;

  // ---- Workspace, allocated once (same rationale as VariationalRefine) ----
  Plane wx(H, W), wy(H, W), wcx(H, W), wcy(H, W);
  Plane wd1(H, W), wd2(H, W);  // oriented-prior diffusivities (mu_d > 0 only)
  Plane prior_scale(H, W, 1.0F);  // data/prior rebalancing factor (3-D only)
  Plane gxY(H, W), gyY(H, W), magx(H, W), magy(H, W);
  Plane tmpr1(H, W), tmpr2(H, W);
  ComplexPlane tmpc1(H, W), tmpc2(H, W), tmpc3(H, W), cprior(H, W);
  Plane r0(H, W), rk(H, W);
  Plane gY(H, W), gY_new(H, W), dY(H, W);
  ComplexPlane gC(H, W), gC_new(H, W), dC(H, W);
  Plane dy_r(H, W), dxdy(H, W), dydy(H, W);
  ComplexPlane dxc(H, W), dyc(H, W);
  std::vector<Plane> wts(n_nbr);
  for (auto& w : wts) w = Plane(H, W);

  const long n = static_cast<long>(N);
  const double n_pixels = static_cast<double>(N);
  const float e2 = eps * eps;
  const float ec2 = eps_c * eps_c;
  const float et2 = eps_t * eps_t;

  // grad(Y, chi) -> (gY_out, gC_out): the joint objective's gradient —
  // exact data term + raw-neighbour terms + anchor term + spatial priors.
  auto grad = [&](const Plane& yy, const ComplexPlane& cc, Plane& gy_out,
                  ComplexPlane& gc_out) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      r0[i] = s[i] - yy[i] - (cc[i] * carrier[i]).real();
      gy_out[i] = -2.0F * r0[i];
      gc_out[i] = -2.0F * r0[i] * std::conj(carrier[i]);
    }
    for (size_t j = 0; j < n_nbr; ++j) {
      const NeighborTerm& nbr = neighbors[j];
      const Plane& wt = wts[j];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float rkv = nbr.composite[i] - yy[i] - (cc[i] * nbr.carrier[i]).real();
        gy_out[i] += -2.0F * nu * wt[i] * rkv;
        gc_out[i] += -2.0F * nu * wt[i] * rkv * std::conj(nbr.carrier[i]);
      }
    }
    if (nu_a > 0.0F) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        gy_out[i] += 2.0F * nu_a * anchor->weight[i] * (yy[i] - anchor->luma[i]);
        gc_out[i] += 2.0F * nu_a * anchor->weight[i] * (cc[i] - anchor->chroma[i]);
      }
    }

    // Spatial Charbonnier priors on grad Y, grad chi (same shape as the
    // pass-1 solver, just added directly onto (gy_out, gc_out) here).
    DxWeightedInto(yy, wx, tmpr1);
    DxTInto(tmpr1, tmpr2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) gy_out[i] += 2.0F * tmpr2[i];

    DyWeightedInto(yy, wy, tmpr1);
    DyTInto(tmpr1, tmpr2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) gy_out[i] += 2.0F * tmpr2[i];

    DxWeightedInto(cc, wcx, tmpc1);
    DxTInto(tmpc1, cprior);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) gc_out[i] += 2.0F * mu_h * cprior[i];

    DyWeightedInto(cc, wcy, tmpc2);
    DyTInto(tmpc2, cprior);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) gc_out[i] += 2.0F * mu_v * cprior[i];

    // Oriented terms: + 2 * mu_d * (D1T(wd1 .* D1(cc)) + D2T(wd2 .* D2(cc))).
    if (mu_d > 0.0F) {
      D1WeightedInto(cc, wd1, tmpc1);
      D1TInto(tmpc1, cprior);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) gc_out[i] += 2.0F * mu_d * cprior[i];

      D2WeightedInto(cc, wd2, tmpc1);
      D2TInto(tmpc1, cprior);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) gc_out[i] += 2.0F * mu_d * cprior[i];
    }
  };

  // curv(dY, dC): the quadratic form's curvature along direction (dY, dC).
  auto curv = [&](const Plane& d_y, const ComplexPlane& d_c) -> double {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) dy_r[i] = d_y[i] + (d_c[i] * carrier[i]).real();
    double h = SumSq(dy_r);
    for (size_t j = 0; j < n_nbr; ++j) {
      const NeighborTerm& nbr = neighbors[j];
      const Plane& wt = wts[j];
      double acc = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float dk = d_y[i] + (d_c[i] * nbr.carrier[i]).real();
        acc += static_cast<double>(wt[i]) * dk * dk;
      }
      h += nu * acc;
    }
    if (nu_a > 0.0F) {
      double acc = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc) schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        acc += static_cast<double>(anchor->weight[i]) *
               (static_cast<double>(d_y[i]) * d_y[i] + std::norm(d_c[i]));
      }
      h += nu_a * acc;
    }
    DxInto(d_y, dxdy);
    DyInto(d_y, dydy);
    DxInto(d_c, dxc);
    DyInto(d_c, dyc);
    double spatial = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : spatial) schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      spatial += static_cast<double>(wx[i]) * dxdy[i] * dxdy[i];
      spatial += static_cast<double>(wy[i]) * dydy[i] * dydy[i];
      spatial += static_cast<double>(mu_h) * wcx[i] * std::norm(dxc[i]);
      spatial += static_cast<double>(mu_v) * wcy[i] * std::norm(dyc[i]);
    }
    if (mu_d > 0.0F) {
      D1Into(d_c, dxc);  // dxc/dyc consumed above; reuse for the diagonals
      D2Into(d_c, dyc);
      double acc_d = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : acc_d) schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        acc_d += static_cast<double>(wd1[i]) * std::norm(dxc[i]);
        acc_d += static_cast<double>(wd2[i]) * std::norm(dyc[i]);
      }
      spatial += mu_d * acc_d;
    }
    return h + spatial;
  };

  for (int outer = 0; outer < irls_outer; ++outer) {
    // IRLS weights, lagged on the current (Y, chi) — same Charbonnier shape
    // as the pass-1 solver, just reading Y directly instead of deriving it
    // from S - Re[chi*carrier] (Y is now a free unknown).
    DxInto(Y, gxY);
    DyInto(Y, gyY);
    DxInto(chi, tmpc1);
    DyInto(chi, tmpc2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      wx[i] = eps / std::sqrt(gxY[i] * gxY[i] + e2);
      wy[i] = eps / std::sqrt(gyY[i] * gyY[i] + e2);
      magx[i] = std::abs(tmpc1[i]);
      magy[i] = std::abs(tmpc2[i]);
    }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      const float dx_den = magx[i] * magx[i] + k * gxY[i] * gxY[i] + ec2;
      const float dy_den = magy[i] * magy[i] + k * gyY[i] * gyY[i] + ec2;
      wcx[i] = eps_c / std::sqrt(dx_den);
      wcy[i] = eps_c / std::sqrt(dy_den);
    }
    if (mu_d > 0.0F) {
      D1Into(chi, tmpc1);
      D2Into(chi, tmpc2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float c1 = std::abs(tmpc1[i]) / eps_c;
        const float c2 = std::abs(tmpc2[i]) / eps_c;
        wd1[i] = 1.0F / std::sqrt(1.0F + c1 * c1);
        wd2[i] = 1.0F / std::sqrt(1.0F + c2 * c2);
      }
    }
    if (nu > 0.0F) {
      for (size_t j = 0; j < n_nbr; ++j) {
        const NeighborTerm& nbr = neighbors[j];
        Plane& wt = wts[j];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < n; ++i) {
          const float rkv = nbr.composite[i] - Y[i] - (chi[i] * nbr.carrier[i]).real();
          wt[i] = nbr.confidence[i] * et2 / (rkv * rkv + et2);
        }
      }

      // ---- PRIOR RENORMALISATION (see VariationalRefine for the full
      // rationale and the measurement). Same imbalance, different base:
      // here Y is a FREE unknown, so the base data term r0 = S - Y -
      // Re[chi*carrier] carries unit weight explicitly and each neighbour
      // adds nu*wt_k of the same shape (its carrier is a unit phasor, so
      // the leverage on (Y, chi) is the same as the base term's — no
      // |dc|^2 factor here, unlike the Y-eliminated solver). The anchor is
      // NOT counted: it is itself a prior, not data.
      for (long i = 0; i < n; ++i) prior_scale[i] = 1.0F;
      for (size_t j = 0; j < n_nbr; ++j) {
        const Plane& wt = wts[j];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (long i = 0; i < n; ++i) prior_scale[i] += nu * wt[i];
      }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        const float k_i = prior_scale[i];
        wx[i] *= k_i;
        wy[i] *= k_i;
        wcx[i] *= k_i;
        wcy[i] *= k_i;
        if (mu_d > 0.0F) {
          wd1[i] *= k_i;
          wd2[i] *= k_i;
        }
      }
    }

    grad(Y, chi, gY, gC);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      dY[i] = -gY[i];
      dC[i] = -gC[i];
    }
    double gg = SumSq(gY) + SumAbs2(gC);
    const double gg0 = gg;  // for the relative-tolerance early exit

    for (int it = 0; it < n_inner; ++it) {
      const double hh = curv(dY, dC);
      if (hh <= 1e-12) break;
      const double gd = DotRealPlane(gY, dY) + DotReal(gC, dC);
      const float alpha = static_cast<float>(-0.5 * gd / hh);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        Y[i] += alpha * dY[i];
        chi[i] += alpha * dC[i];
      }

      grad(Y, chi, gY_new, gC_new);
      const double gg_new = SumSq(gY_new) + SumAbs2(gC_new);
      const float beta = static_cast<float>(gg_new / std::max(gg, 1e-30));
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < n; ++i) {
        dY[i] = -gY_new[i] + beta * dY[i];
        dC[i] = -gC_new[i] + beta * dC[i];
      }
      gY = gY_new;
      gC = gC_new;
      gg = gg_new;
      if (gg < static_cast<double>(cg_tol) * cg_tol * gg0) break;
      if (gg < 1e-10 * n_pixels) break;
    }
  }

  RefineResult out;
  out.luma = std::move(Y);
  out.chroma = std::move(chi);
  return out;
}

}  // namespace hvd
