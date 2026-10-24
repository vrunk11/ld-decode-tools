// SPDX-License-Identifier: GPL-3.0-or-later
//
// sequence.cpp — port of decode_sequence's per-window body (reference/hvd/
// decoder.py). Reference line references: prepare_field (1433), the driver
// loop (1560-1730), synth_reference (741), psi_closed_form (1292).

#include "engine/sequence.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

#include "engine/engine.h"
#include "engine/holographic_init.h"
#include "engine/lockin.h"
#include "engine/motion.h"
#include "engine/temporal.h"
#include "engine/variational.h"

namespace hvd {

namespace {

constexpr float kPi = 3.14159265358979323846F;

float MedianOf(std::vector<float> v) {
  if (v.empty()) return 0.0F;
  const size_t k = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + k, v.end());
  float m = v[k];
  if (v.size() % 2 == 0) {
    std::nth_element(v.begin(), v.begin() + k - 1, v.begin() + k);
    m = 0.5F * (m + v[k - 1]);
  }
  return m;
}

// ---- synth_reference (reference line 741) --------------------------------
// Build a denoised reference for field j by motion-compensated temporal
// blending of the *decoded* fields, then re-encode it to composite for an
// honesty check. NTSC geometry does the heavy lifting: separation leakage
// anti-correlates at +/-1 frame (180 deg carrier flip), so the blend cancels
// it; at +/-2 frames the carrier is back in phase, so those neighbours
// average noise without touching signal. Geman-McClure per-pixel weights
// drop motion residuals out. conf_ref (the anchor weight) is measured in
// the COMPOSITE domain: 1 where the re-encoded synthesis explains the raw
// measurement, -> 0 where it does not — the anchor never trusts a reference
// the data contradicts.
AnchorTerm SynthReference(int j, const std::vector<Plane>& Ys,
                          const std::vector<ComplexPlane>& chis,
                          const std::vector<FieldObs>& fields,
                          const HvdConfig& cfg,
                          const std::vector<int>& parities,
                          std::map<std::pair<int, int>, MotionField>* mcache,
                          std::mutex* mcache_mu) {
  const Plane& S = fields[j].s;
  const ComplexPlane& carrier = fields[j].carrier;
  const float eps_b = cfg.nr_eps;
  const int hh = Ys[j].height();
  const int ww = Ys[j].width();
  const int tile = cfg.mc_tile;
  const long n = static_cast<long>(Ys[j].size());

  Plane accY = Ys[j];
  ComplexPlane accC = chis[j];
  Plane accW(hh, ww, 1.0F);

  for (int k = j - cfg.nr_radius; k <= j + cfg.nr_radius; ++k) {
    if (k == j || k < 0 || k >= static_cast<int>(Ys.size())) continue;
    // The shared fast-mode cache is read/written under `mcache_mu` when the
    // colored parallel sweep is active (map iterators/references are not
    // stable across concurrent inserts) — the estimate itself runs outside
    // the lock; a rare duplicate estimate on a race is benign (same inputs,
    // same result).
    MotionField local;
    const MotionField* mo = &local;
    bool have = false;
    if (mcache) {
      std::unique_lock<std::mutex> lk;
      if (mcache_mu) lk = std::unique_lock<std::mutex>(*mcache_mu);
      auto it = mcache->find({j, k});
      if (it != mcache->end()) {
        local = it->second;
        have = true;
      }
    }
    if (!have) {
      local = EstimateMotion(Ys[k], Ys[j], tile, cfg.mc_search);
      if (mcache) {
        std::unique_lock<std::mutex> lk;
        if (mcache_mu) lk = std::unique_lock<std::mutex>(*mcache_mu);
        (*mcache)[{j, k}] = local;
      }
    }
    const Plane conf = UpsampleConfidence(mo->confidence, tile, hh, ww);
    // Decoded fields are BASEBAND: bilinear sub-pixel warp is legal, and
    // the half-line parity offset between opposite fields is compensated
    // exactly on the sampling grid.
    const float row_off =
        (static_cast<float>(parities[j]) - static_cast<float>(parities[k])) /
        2.0F;
    const PerPixelMotion vpix = VectorsPerPixel(mo->dy, mo->dx, tile, hh, ww);
    const Plane Yw = WarpBilinearTiles(Ys[k], vpix, row_off, hh, ww);
    Plane re(hh, ww), im(hh, ww);
    for (long i = 0; i < n; ++i) {
      re[i] = chis[k][i].real();
      im[i] = chis[k][i].imag();
    }
    const Plane wre = WarpBilinearTiles(re, vpix, row_off, hh, ww);
    const Plane wim = WarpBilinearTiles(im, vpix, row_off, hh, ww);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n; ++i) {
      const Complex cw(wre[i], wim[i]);
      const float dy = Yw[i] - Ys[j][i];
      const float d2 =
          dy * dy + std::norm(cw - chis[j][i]);
      const float w = conf[i] * eps_b * eps_b / (d2 + eps_b * eps_b);
      accY[i] += w * Yw[i];
      accC[i] += w * cw;
      accW[i] += w;
    }
  }

  AnchorTerm out;
  out.luma = Plane(hh, ww);
  out.chroma = ComplexPlane(hh, ww);
  Plane resid(hh, ww);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long i = 0; i < n; ++i) {
    out.luma[i] = accY[i] / accW[i];
    out.chroma[i] = accC[i] / accW[i];
    const float s_hat = out.luma[i] + (out.chroma[i] * carrier[i]).real();
    resid[i] = std::fabs(S[i] - s_hat);
  }
  const Plane r = BoxBlur(std::move(resid), 1);
  out.weight = Plane(hh, ww);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long i = 0; i < n; ++i)
    out.weight[i] = eps_b * eps_b / (r[i] * r[i] + eps_b * eps_b);
  return out;
}

// ---- psi_closed_form (reference line 1292) --------------------------------
// N-step phase-shifting-interferometry init: every static pixel is observed
// under several KNOWN carrier phases (its own field plus each MC-warped
// neighbour). Per observation k: S_k = Y + p*Re[c_k] - q*Im[c_k], chi=p+iq —
// exactly an N-step interferogram set; the weighted normal equations are a
// 3x3 per-pixel system solved in closed form (Cramer). Where confidence is
// high it lands on the temporally exact solution before any spatial prior;
// where total confidence is low it blends back to the fallback init.
ComplexPlane PsiClosedForm(const Plane& S, const ComplexPlane& carrier,
                           const std::vector<NeighborTerm>& neighbors,
                           const ComplexPlane& chi_fallback) {
  const int hh = S.height();
  const int ww = S.width();
  const long n = static_cast<long>(S.size());
  ComplexPlane out(hh, ww);
  constexpr float kEpsT = 6.0F;  // the refine's robust gate AT THE INIT
                                 // POINT — without it the closed form
                                 // commits to motion-contaminated chi

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long i = 0; i < n; ++i) {
    // Accumulate weighted normal equations A^T W A over the observations:
    // basis [1, Re c, -Im c].
    double a11 = 0, a12 = 0, a13 = 0, a22 = 0, a23 = 0, a33 = 0;
    double b1 = 0, b2 = 0, b3 = 0;
    double wsum_nbr = 0.0;
    auto add = [&](float Sk, Complex ck, float wk) {
      const double rc = ck.real();
      const double ic = -ck.imag();
      a11 += wk;
      a12 += wk * rc;
      a13 += wk * ic;
      a22 += wk * rc * rc;
      a23 += wk * rc * ic;
      a33 += wk * ic * ic;
      b1 += wk * Sk;
      b2 += wk * Sk * rc;
      b3 += wk * Sk * ic;
    };
    add(S[i], carrier[i], 1.0F);
    for (const NeighborTerm& nb : neighbors) {
      const Complex dc = carrier[i] - nb.carrier[i];
      const float rt =
          (nb.composite[i] - S[i]) + (chi_fallback[i] * dc).real();
      const float wk =
          nb.confidence[i] * kEpsT * kEpsT / (rt * rt + kEpsT * kEpsT);
      add(nb.composite[i], nb.carrier[i], wk);
      wsum_nbr += wk;
    }
    const double det = a11 * (a22 * a33 - a23 * a23) -
                       a12 * (a12 * a33 - a23 * a13) +
                       a13 * (a12 * a23 - a22 * a13);
    // Phase diversity: with < 3 well-separated phases the system is
    // near-singular (det -> 0); blend to fallback there.
    const double a11c = std::max(a11, 1e-9);
    const bool ok = det > 1e-3 * a11c * a11c * a11c * 0.01;
    const double d = ok ? det : 1.0;
    const double q1 = (a11 * (b2 * a33 - b3 * a23) -
                       b1 * (a12 * a33 - a23 * a13) +
                       a13 * (a12 * b3 - b2 * a13)) /
                      d;  // p = Re chi
    const double q2 = (a11 * (a22 * b3 - a23 * b2) -
                       a12 * (a12 * b3 - b2 * a13) +
                       b1 * (a12 * a23 - a22 * a13)) /
                      d;  // q = Im chi
    const float t =
        ok ? std::clamp(static_cast<float>(wsum_nbr / 2.0), 0.0F, 1.0F) : 0.0F;
    const Complex psi(static_cast<float>(q1), static_cast<float>(q2));
    out[i] = t * psi + (1.0F - t) * chi_fallback[i];
  }
  return out;
}

}  // namespace

FieldObs PrepareFieldObs(const FieldInput& field, const FieldGeometry& g,
                         const HvdConfig& cfg, int parity) {
  std::vector<float> theta;
  std::vector<int8_t> vswitch;
  if (g.uses_vswitch()) {
    PalBurstLockin l = BurstLockinPhasePal(field.samples, g);
    theta = std::move(l.theta);
    vswitch = std::move(l.vswitch);
  } else {
    theta = BurstLockinPhase(field.samples, g);
  }
  // Chroma phase correction on the burst-locked reference itself, same as
  // the frame path (WeaveAndBuildCarrier in engine.cpp) -- one shared
  // helper so the two paths cannot drift. The offset is signed by the
  // V-switch sense on the PAL family; see ApplyChromaPhase.
  ApplyChromaPhase(cfg.chroma_phase_deg, g, vswitch, &theta);
  const int fal = g.first_active_field_line;
  const int lal = g.last_active_line();
  const int a0 = g.active_video_start;
  const int a1 = g.active_video_end;
  const int lines = std::max(0, lal - fal);
  const int width = std::max(0, a1 - a0);

  FieldObs obs;
  obs.parity = parity;
  obs.s = Plane(lines, width);
  for (int y = 0; y < lines; ++y)
    for (int x = 0; x < width; ++x)
      obs.s.at(y, x) = field.samples.at(fal + y, a0 + x);
  std::vector<float> theta_active(lines);
  for (int y = 0; y < lines; ++y) theta_active[y] = theta[fal + y];
  if (g.uses_vswitch()) {
    std::vector<int8_t> sw_active(lines);
    for (int y = 0; y < lines; ++y) sw_active[y] = vswitch[fal + y];
    obs.carrier = MakeCarrierPal(theta_active, sw_active, g);
  } else {
    obs.carrier = MakeCarrier(theta_active, g);
  }
  return obs;
}

std::vector<DecodedField> DecodeFieldWindow(const std::vector<FieldObs>& fields,
                                            const FieldGeometry& g,
                                            const HvdConfig& cfg, Fft2d* fft,
                                            SequenceDiagnostics* diag) {
  std::vector<DecodedField> inits(fields.size());
  for (size_t j = 0; j < fields.size(); ++j) {
    HoloInit hi = HolographicInit(fields[j].s, fields[j].carrier, g, cfg, fft);
    inits[j].luma = std::move(hi.luma);
    inits[j].chroma = std::move(hi.chroma);
  }
  return DecodeFieldWindowWithInits(fields, inits, g, cfg, diag);
}

DrizzleResult DrizzleFrame(int j0, const std::vector<Plane>& Ys,
                           const std::vector<ComplexPlane>& chis,
                           const std::vector<int>& parities,
                           const HvdConfig& cfg, int scale) {
  const int L = Ys[j0].height();
  const int W = Ys[j0].width();
  const int HF = 2 * L * scale;  // fine frame height
  const int n = static_cast<int>(Ys.size());
  const float eps_b = cfg.nr_eps > 0.0F ? cfg.nr_eps : 3.0F;

  Plane accY(HF, W, 0.0F);
  Plane accCr(HF, W, 0.0F), accCi(HF, W, 0.0F);
  Plane accW(HF, W, 0.0F);

  // Scatter accumulation is inherently write-conflicting across sources
  // (nearby samples land on the same fine rows), so the deposit loops stay
  // sequential per contributing field; the arithmetic per deposit is tiny
  // next to the motion estimation feeding it.
  for (int jt = j0; jt <= j0 + 1; ++jt) {
    for (int k = std::max(0, j0 - 2 * cfg.nr_radius);
         k < std::min(n, j0 + 2 + 2 * cfg.nr_radius); ++k) {
      const int pk = parities[k];
      if ((k == j0 || k == j0 + 1) && k != jt)
        continue;  // the sibling field deposits on its own turn
      MotionField mo;
      if (k == jt) {
        mo.dy = Plane(1, 1, 0.0F);
        mo.dx = Plane(1, 1, 0.0F);
        mo.confidence = Plane(1, 1, 1.0F);
        mo.tile = cfg.mc_tile;
      } else {
        mo = EstimateMotion(Ys[k], Ys[jt], cfg.mc_tile, cfg.mc_search);
      }
      const PerPixelMotion v =
          VectorsPerPixel(mo.dy, mo.dx, cfg.mc_tile, L, W);
      const int th = mo.confidence.height();
      const int tw = mo.confidence.width();
      for (int y = 0; y < L; ++y) {
        const int cty = std::min(y / cfg.mc_tile, th - 1);
        for (int x = 0; x < W; ++x) {
          const int ctx = std::min(x / cfg.mc_tile, tw - 1);
          const float cpx = mo.confidence.at(cty, ctx);
          const float vy = v.dy.at(y, x);
          const float vx = v.dx.at(y, x);
          // Robust agreement with the target field's own decode, evaluated
          // at the integer-rounded warp (cheap).
          const int syi = std::clamp(
              static_cast<int>(std::lround(y - vy)), 0, L - 1);
          const int sxi = std::clamp(
              static_cast<int>(std::lround(x - vx)), 0, W - 1);
          const float dY = Ys[k].at(y, x) - Ys[jt].at(syi, sxi);
          const float d2 =
              dY * dY + std::norm(chis[k].at(y, x) - chis[jt].at(syi, sxi));
          const float w = cpx * eps_b * eps_b / (d2 + eps_b * eps_b);
          // Source sample (y, x) of field k lands, in target-frame fine
          // rows, at (2*(y + vy) + pk) * scale; linear split between the
          // two nearest fine rows ('pixfrac=1, linear kernel' drizzle).
          // DEPOSIT GEOMETRY — COUPLED TO THE MOTION CONVENTION, do not
          // change one without the other. This maps a source row to the
          // fine grid as 2*(y + motion) + parity, which is correct ONLY
          // because the vectors feeding it are INTEGER and margin-snapped:
          // the zero-motion margin rule discards sub-line displacement, so
          // `vy` never contains the half-line parity term h_k = (p_k-p_j)/2.
          // If sub-pixel refinement is ever added here (it would let
          // drizzle exploit half-line registration — see reference-pal/
          // THEORY-PAL.md 5d, where the PAL research added exactly that),
          // the vectors WILL contain h_k and this line must become
          // 2*(y + vy - h_k) + pk, or static cross-parity content lands a
          // full frame-line off. That derivation is why the research
          // flagged this as a latent issue rather than a live bug.
          const float yf = (2.0F * (y + vy) + static_cast<float>(pk)) *
                           static_cast<float>(scale);
          const int xs = std::clamp(
              static_cast<int>(std::lround(x + vx)), 0, W - 1);
          const int y0 = static_cast<int>(std::floor(yf));
          const float fy = yf - static_cast<float>(y0);
          const Complex ck = chis[k].at(y, x);
          const float sk = Ys[k].at(y, x);
          for (int off = 0; off < 2; ++off) {
            const int yt = std::clamp(y0 + off, 0, HF - 1);
            const float ww = w * (off == 0 ? (1.0F - fy) : fy);
            accY.at(yt, xs) += ww * sk;
            accCr.at(yt, xs) += ww * ck.real();
            accCi.at(yt, xs) += ww * ck.imag();
            accW.at(yt, xs) += ww;
          }
        }
      }
    }
  }

  // Fallback where coverage is thin: linear vertical interpolation of the
  // plain woven decode. (The reference computes an unused r0/fr/r1 block
  // here — dead code, not carried over.)
  const int p0 = parities[j0];
  const int p1 = parities[j0 + 1];
  Plane wovenY(2 * L, W);
  Plane wovenCr(2 * L, W), wovenCi(2 * L, W);
  for (int r = 0; r < L; ++r) {
    for (int x = 0; x < W; ++x) {
      wovenY.at(2 * r + p0, x) = Ys[j0].at(r, x);
      wovenY.at(2 * r + p1, x) = Ys[j0 + 1].at(r, x);
      wovenCr.at(2 * r + p0, x) = chis[j0].at(r, x).real();
      wovenCr.at(2 * r + p1, x) = chis[j0 + 1].at(r, x).real();
      wovenCi.at(2 * r + p0, x) = chis[j0].at(r, x).imag();
      wovenCi.at(2 * r + p1, x) = chis[j0 + 1].at(r, x).imag();
    }
  }

  DrizzleResult out;
  out.luma = Plane(HF, W);
  out.chroma = ComplexPlane(HF, W);
  constexpr float kLam = 0.35F;  // coverage confidence scale
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int r = 0; r < HF; ++r) {
    const float fine = static_cast<float>(r) / static_cast<float>(scale);
    const int f0 = std::clamp(static_cast<int>(std::floor(fine)), 0, 2 * L - 2);
    const float ff = fine - static_cast<float>(f0);
    for (int x = 0; x < W; ++x) {
      const float baseY =
          wovenY.at(f0, x) * (1.0F - ff) + wovenY.at(f0 + 1, x) * ff;
      const Complex baseC{
          wovenCr.at(f0, x) * (1.0F - ff) + wovenCr.at(f0 + 1, x) * ff,
          wovenCi.at(f0, x) * (1.0F - ff) + wovenCi.at(f0 + 1, x) * ff};
      const float aw = accW.at(r, x);
      const float mix = aw / (aw + kLam);
      const float dzY = aw > 0.0F ? accY.at(r, x) / std::max(aw, 1e-9F) : 0.0F;
      const Complex dzC =
          aw > 0.0F ? Complex{accCr.at(r, x) / std::max(aw, 1e-9F),
                              accCi.at(r, x) / std::max(aw, 1e-9F)}
                    : Complex{};
      out.luma.at(r, x) = mix * dzY + (1.0F - mix) * baseY;
      out.chroma.at(r, x) = mix * dzC + (1.0F - mix) * baseC;
    }
  }
  return out;
}

std::vector<DecodedField> DecodeFieldWindowWithInits(
    const std::vector<FieldObs>& fields,
    const std::vector<DecodedField>& inits, const FieldGeometry& g,
    const HvdConfig& cfg_in, SequenceDiagnostics* diag) {
  (void)g;
  const int nf = static_cast<int>(fields.size());
  std::vector<DecodedField> out(nf);
  if (nf == 0) return out;

  // --- self-calibration: measure the source's noise, scale the robust
  // gates accordingly (a fixed IRE gate is 5 sigma on a clean disc but
  // 1.4 sigma on a noisy one => over-gating exactly where 3D helps most)
  HvdConfig ccfg = cfg_in;
  // Standard flag from the geometry (see HvdConfig::is_pal): selects the
  // 4-line (PAL) vs 2-line (NTSC) leak cancellation in AUTO chroma_aniso.
  ccfg.is_pal = g.uses_vswitch();
  // The noise sets the gates of the neighbour equations: a decode without them (temporal strength below 0) does not measure it
  if (cfg_in.temporal_strength >= 0.0F || diag) {
    std::vector<float> sigmas;
    sigmas.reserve(fields.size());
    for (const FieldObs& f : fields) sigmas.push_back(EstimateNoiseIre(f.s));
    const float sigma = MedianOf(std::move(sigmas));
    if (diag) diag->sigma_ire = sigma;
    if (ccfg.temporal_eps <= 0.0F)
      ccfg.temporal_eps = std::clamp(7.0F * sigma, 4.0F, 20.0F);
    if (ccfg.nr_eps <= 0.0F)
      ccfg.nr_eps = std::clamp(3.0F * sigma, 3.0F, 12.0F);
  }

  std::vector<int> offs;
  const bool pal = g.uses_vswitch();
  if (ccfg.extended_temporal) {
    // PAL's strongest equation (180 deg carrier relation, |dc| = 2) sits
    // at f+/-4 — TWO frames — not NTSC's f+/-2; the 135 deg/field ladder
    // makes every offset in +/-4 non-degenerate (THEORY-PAL 5, 5c). The
    // odd offsets' oscillating |dc| and half-line bias are handled by the
    // machinery below exactly as NTSC's odd offsets are (actual carrier
    // arrays + the envelope gate), no special-casing.
    offs = pal ? (ccfg.bidirectional
                      ? std::vector<int>{-4, -3, -2, -1, 1, 2, 3, 4}
                      : std::vector<int>{-4, -3, -2, -1})
               : (ccfg.bidirectional ? std::vector<int>{-3, -2, -1, 1, 2, 3}
                                     : std::vector<int>{-3, -2, -1});
  } else {
    offs = ccfg.bidirectional ? std::vector<int>{-2, -1, 1, 2}
                              : std::vector<int>{-2, -1};
  }
  // PAL-swept defaults (reference-pal THEORY 5b): full-weight coherence
  // (1.0) with nu=0.5 measured strictly dominating; nr_radius 4 because
  // the blend's leakage anti-correlation lives at f+/-4 on PAL. Applied
  // only when the caller left the NTSC-tuned defaults in place.
  if (pal) {
    if (ccfg.coherence_gate == 0.6F) ccfg.coherence_gate = 1.0F;
    if (ccfg.nr_radius == 2) ccfg.nr_radius = 4;
  }

  std::vector<Plane> Ys(nf);
  std::vector<ComplexPlane> chis(nf);
  std::vector<int> parities(nf);
  for (int j = 0; j < nf; ++j) {
    chis[j] = inits[j].chroma;
    if (!inits[j].luma.empty()) {
      Ys[j] = inits[j].luma;
    } else {
      Ys[j] = Plane(fields[j].s.height(), fields[j].s.width());
      for (size_t i = 0; i < Ys[j].size(); ++i)
        Ys[j][i] =
            fields[j].s[i] - (chis[j][i] * fields[j].carrier[i]).real();
    }
    parities[j] = fields[j].parity;
  }

  // Per-window motion cache. Fast mode shares one estimate per field pair
  // across passes AND with the anchor blend (~4x fewer block-matching
  // runs); slow mode keys per pass (bit-exact preservation of the
  // reference's per-pass estimates). Key: (j, k) with pass folded in for
  // slow mode via a distinct map per pass.
  std::map<std::pair<int, int>, MotionField> mcache_fast;

  // ADAPTIVE temporal strength. Convention: strength < 0 => 3D OFF (the
  // stage passes -1 when the switch is off); == 0 => AUTO (measure the
  // content); > 0 => fixed (reference behaviour). The right strength is a
  // property of the CONTENT: on Y/C-ambiguous material (luma energy at
  // the subcarrier) the neighbour equations resolve what 2D cannot and
  // deserve to be strong; on unambiguous material they can only lift
  // chroma noise and should stay weak. The measurement uses the phase
  // physics directly: d_j = 5-tap demod of S by the field's own carrier;
  // for a same-parity pair (j, j+2) the carrier has flipped 180 degrees,
  // so true (static) chroma appears IDENTICALLY in both while luma
  // leaking through the chroma band flips sign — (d_j - d_{j+2}) / 2
  // isolates exactly the ambiguous energy. Noise is subtracted in
  // quadrature via the same sigma the robust gates calibrated from;
  // motion only biases the estimate upward, i.e. toward "more 3D", where
  // the per-pixel gates already protect the result.
  // Same-parity 180-deg pair stride: 2 fields on NTSC, 4 on PAL (the
  // 135 deg/field ladder reaches the flip at two frames — same
  // discriminator, longer baseline; fields j and j+4 share V-switch
  // parity so the chroma-coherent / leak-sign-flip logic transfers).
  const int amb_stride = g.uses_vswitch() ? 4 : 2;
  // == 0 means AUTO (measure). A NEGATIVE strength means the caller
  // explicitly switched 3D off and must skip this scan entirely -- it is
  // not free (a decimated demod over every same-parity field pair in the
  // window) and, more importantly, it would resolve a positive strength
  // and silently switch the whole 3D path back on.
  if (ccfg.temporal_strength == 0.0F && ccfg.cg_iterations > 0 &&
      nf > amb_stride) {
    std::vector<float> ambs;
    for (int j = 0; j + amb_stride < nf; ++j) {
      const Plane& Sj = fields[j].s;
      const Plane& Sk = fields[j + amb_stride].s;
      const int hh = Sj.height();
      const int ww = Sj.width();
      // Crude demod, decimated: this is a scalar calibration, not a decode.
      // Accumulated PER HORIZONTAL BAND (8 decimated rows ~ 24 field
      // lines), aggregated as the 95th-percentile band instead of the
      // full-field RMS. Rationale, measured: the
      // artifact class this gate exists for — thin bright horizontal
      // bars, small text, blinds — occupies a few LINES of an otherwise
      // ordinary frame; a full-field RMS dilutes 6 ambiguous lines out
      // of 242 to near the noise floor and resolves strength ~0, which
      // does not merely weaken 3D: strength 0 forces n_passes=1, so the
      // ANCHORED pass (the thing that actually fixes thin lines, §17)
      // never runs and "adaptive 3D" silently becomes plain 2D on
      // exactly the content that needed it. p95-of-bands responds to
      // localized ambiguity while measuring the SAME value as the RMS
      // on uniform content (repro: 0.93 -> 1.64 IRE localized; flat
      // noise 0.60 -> 0.62; clean chart unchanged at saturation).
      std::vector<double> band_acc;
      std::vector<long> band_n;
      int yi = 0;
      double acc = 0.0;
      long n = 0;
      for (int y = 2; y < hh - 2; y += 3, ++yi) {
        const int band = yi / 8;
        if (band >= static_cast<int>(band_acc.size())) {
          band_acc.push_back(0.0);
          band_n.push_back(0);
        }
        for (int x = 4; x + 7 < ww; x += 4) {
          // STATIC MASK (motion robustness). The (d_j - d_{j+2})/2
          // discriminator below is only valid where the scene is STATIC
          // between the pair: under motion, true chroma decorrelates
          // between the fields and the whole moving region reads as
          // "ambiguity" — measured on real moving content (music video,
          // frames 5902-5906 of the report that motivated this): the
          // unmasked measure saturated the strength at 1.5 while a
          // strength sweep showed every step DOWN from there brought the
          // moving fine-detail zones back toward the (better) 2D result.
          // The doc's old claim that "motion only biases the estimate
          // upward, where the per-pixel gates already protect the result"
          // did not hold there: at saturated strength the robust gates
          // demonstrably did not protect (3D measurably worse than 2D
          // across the frame). Gate the measurement to pixels whose
          // carrier-free (box-4) means agree within ~3 sigma; the
          // threshold reuses the just-calibrated nr_eps (= clip(3*sigma))
          // so a forced nr_eps stays consistent with the gates it feeds.
          {
            float mj = 0.0F, mk = 0.0F;
            for (int k4 = 0; k4 < 4; ++k4) {
              mj += Sj.at(y, x + k4);
              mk += Sk.at(y, x + k4);
            }
            if (std::fabs(mj - mk) * 0.25F > ccfg.nr_eps) continue;
          }
          // Triangle-7 (= box4 convolved with box4): a DOUBLE null at the
          // carrier frequency (pi/2 rad/px at 4fsc). The demod shifts
          // ordinary luma to ~pi/2; a single 4-tap null is exact only ON
          // the carrier and leaks ~9% a tenth of a radian away — enough
          // for a 20 IRE luma ramp to dwarf real ambiguity. The double
          // null rejects the whole neighbourhood (<1% at +/-0.12 rad), so
          // what survives is energy that genuinely sat NEAR fsc before
          // the demod: the actual Y/C ambiguity.
          auto demod = [&](const Plane& S, const ComplexPlane& car) {
            static constexpr float kW[7] = {1.0F, 2.0F, 3.0F, 4.0F,
                                            3.0F, 2.0F, 1.0F};
            Complex d{0.0F, 0.0F};
            for (int k = 0; k < 7; ++k)
              d += kW[k] * S.at(y, x + k) * std::conj(car.at(y, x + k));
            return d * (1.0F / 16.0F);
          };
          const Complex dj = demod(Sj, fields[j].carrier);
          const Complex dk = demod(Sk, fields[j + amb_stride].carrier);
          const double e = 0.25 * std::norm(dj - dk);
          acc += e;
          ++n;
          band_acc[band] += e;
          ++band_n[band];
        }
      }
      if (n > 0) {
        std::vector<float> brms;
        brms.reserve(band_acc.size());
        for (size_t b = 0; b < band_acc.size(); ++b)
          if (band_n[b] >= 8)  // a mostly-masked (moving) band's few
                               // surviving taps are not a measurement
            brms.push_back(static_cast<float>(band_acc[b] / band_n[b]));
        if (!brms.empty()) {
          const size_t k95 =
              std::min(brms.size() - 1,
                       static_cast<size_t>(0.95 * brms.size()));
          std::nth_element(brms.begin(), brms.begin() + k95, brms.end());
          ambs.push_back(std::sqrt(brms[k95]));
        } else {
          ambs.push_back(std::sqrt(static_cast<float>(acc / n)));
        }
      }
    }
    float amb = MedianOf(std::move(ambs));
    // Remove the noise contribution: the triangle-7 demod carries
    // sum(w^2)/16^2 = 44/256 of sigma^2 per field; the half-difference of
    // two fields halves that -> rms ~= 0.29*sigma. Use the same sigma the
    // robust gates calibrated from.
    const float sigma_d = ccfg.nr_eps > 0.0F ? ccfg.nr_eps / 3.0F : 1.0F;
    const float noise_floor = sigma_d * 0.29F;
    amb = std::sqrt(std::max(0.0F, amb * amb - noise_floor * noise_floor));
    // Map ambiguity (IRE of leaked luma in the chroma band) onto strength
    // around the reference's --3d value (~1 IRE of leak deserves 0.5;
    // heavy cross-colour saturates at 1.5). NO floor: measured on the
    // clean regression scene, even a small forced strength (plus the
    // anchor it drags in) costs ~3 dB where there is no ambiguity to
    // resolve — amb ~ 0 must mean genuinely OFF.
    ccfg.temporal_strength =
        std::clamp(0.5F * amb - 0.05F, 0.0F, 1.5F);
    if (diag) diag->ambiguity_ire = amb;
  }
  // Collapse the three-valued input to the two-valued thing the rest of
  // this function wants. This runs AFTER the adaptive scan above on
  // purpose: a negative (explicit OFF) strength has to survive long
  // enough to skip that scan, and only then becomes 0, which
  // has_temporal's `> 0.0F` test below reads as "no temporal terms".
  if (ccfg.temporal_strength < 0.0F) ccfg.temporal_strength = 0.0F;

  if (diag) {
    diag->resolved_strength = std::max(0.0F, ccfg.temporal_strength);
    diag->temporal_eps = ccfg.temporal_eps;
    diag->nr_eps = ccfg.nr_eps;
  }

  const bool has_temporal =
      ccfg.temporal_strength > 0.0F && ccfg.cg_iterations > 0;

  // decode_field semantics when the temporal terms are off: ONE pass, no
  // anchor. (The reference only runs passes/anchor inside decode_sequence,
  // i.e. WITH temporal terms; letting the anchor blend fields in "2D" both
  // diverged from the reference and made the supposedly-decoupled fields
  // couple through SynthReference — a data race under the stride-1
  // parallel sweep, and a semantics bug before it was a threading one.)
  const int n_passes = has_temporal ? std::max(1, ccfg.passes) : 1;

  std::mutex mcache_mu;  // guards the shared fast-mode cache during the
                         // colored parallel sweep (see below)

  for (int pass = 0; pass < n_passes; ++pass) {
    std::map<std::pair<int, int>, MotionField> mcache_pass;  // slow mode
    auto& mcache = ccfg.fast ? mcache_fast : mcache_pass;

    // Cache access helpers: lookups/inserts under the mutex, estimation
    // outside it (a duplicated estimate on a race is benign — identical
    // inputs give identical results; corrupting the map would not be).
    auto cache_get = [&](int a, int b, MotionField* out_mf) {
      std::lock_guard<std::mutex> lk(mcache_mu);
      auto it = mcache.find({a, b});
      if (it == mcache.end()) return false;
      *out_mf = it->second;
      return true;
    };
    auto cache_put = [&](int a, int b, const MotionField& mf) {
      std::lock_guard<std::mutex> lk(mcache_mu);
      mcache[{a, b}] = mf;
    };

    auto solve_field = [&](int j) {
      // 0 iterations is the holographic reconstruction alone (the fast preview): the init is the result
      if (ccfg.cg_iterations <= 0) return;
      const Plane& S = fields[j].s;
      const ComplexPlane& carrier = fields[j].carrier;
      const int pj = parities[j];

      // ---- pairwise motion for this field's offsets ----------------------
      std::map<int, MotionField> pair_motion;
      if (has_temporal) {
        // Fast mode: full pyramid search only for {-1, +1, +2}; the rest
        // get trajectory-PREDICTED vectors audited by VerifyMotion.
        const std::vector<int> full_os =
            ccfg.fast ? std::vector<int>{-1, 1, 2} : offs;
        for (int o : offs) {
          const int k = j + o;
          if (k < 0 || k >= nf) continue;
          MotionField mf;
          if (cache_get(j, k, &mf)) {
            pair_motion[o] = std::move(mf);
          } else if (!ccfg.trajectory_fit ||
                     std::find(full_os.begin(), full_os.end(), o) !=
                         full_os.end()) {
            mf = EstimateMotion(Ys[k], Ys[j], ccfg.mc_tile, ccfg.mc_search);
            cache_put(j, k, mf);
            pair_motion[o] = std::move(mf);
          }
        }
        if (ccfg.fast && ccfg.trajectory_fit && !pair_motion.empty()) {
          // Per-tile velocity from the offsets we DID search (plain median,
          // parity term removed), used to PREDICT the remaining offsets.
          const int th = pair_motion.begin()->second.dy.height();
          const int tw = pair_motion.begin()->second.dy.width();
          Plane vym0(th, tw), vxm0(th, tw);
          {
            std::vector<float> sy, sx;
            for (int i = 0; i < th * tw; ++i) {
              sy.clear();
              sx.clear();
              for (const auto& [o, mf] : pair_motion) {
                const float ho =
                    (static_cast<float>(parities[j + o]) - pj) / 2.0F;
                sy.push_back((mf.dy[i] - ho) / static_cast<float>(o));
                sx.push_back(mf.dx[i] / static_cast<float>(o));
              }
              vym0[i] = MedianOf(sy);
              vxm0[i] = MedianOf(sx);
            }
          }
          for (int o : offs) {
            const int k = j + o;
            if (pair_motion.count(o) || k < 0 || k >= nf) continue;
            MotionField mf;
            if (cache_get(j, k, &mf)) {
              pair_motion[o] = std::move(mf);
              continue;
            }
            const float ho = (static_cast<float>(parities[k]) - pj) / 2.0F;
            Plane pdy(vym0.height(), vym0.width());
            Plane pdx(vxm0.height(), vxm0.width());
            for (size_t i = 0; i < pdy.size(); ++i) {
              pdy[i] = static_cast<float>(o) * vym0[i] + ho;
              pdx[i] = static_cast<float>(o) * vxm0[i];
            }
            mf = VerifyMotion(Ys[k], Ys[j], pdy, pdx, ccfg.mc_tile);
            cache_put(j, k, mf);
            pair_motion[o] = std::move(mf);
          }
        }
        if (ccfg.trajectory_fit && pair_motion.size() >= 2) {
          // Trajectory-coherent consensus snap (THEORY 9e), with the known
          // half-line parity term h_k = (p_k - p_j)/2 removed before the
          // fit and reinstated after.
          std::vector<OffsetMotion> pm;
          pm.reserve(pair_motion.size());
          for (const auto& [o, mf] : pair_motion) {
            OffsetMotion m;
            m.offset = o;
            m.parity_shift =
                (static_cast<float>(parities[j + o]) - pj) / 2.0F;
            m.field = mf;
            pm.push_back(std::move(m));
          }
          TrajectorySnap(&pm);
          for (OffsetMotion& m : pm) pair_motion[m.offset] = std::move(m.field);
        }
      }

      // ---- neighbour equations -------------------------------------------
      std::vector<NeighborTerm> neighbors;
      // Half-line validity envelope for ODD offsets (opposite parity).
      // An opposite-parity neighbour samples the scene BETWEEN this
      // field's lines: after the integer warp its equation claims the
      // content at y +/- 0.5 lines is the content at y. Wherever the
      // current field has vertical structure that claim is wrong by
      // ~0.5*|dS/dline| — and, crucially, on detail thinner than the
      // frame-line pitch (a 1-frame-line coloured ledge, thin blinds) the
      // opposite parity genuinely does not SEE the feature, so its
      // equation votes "background" with a SMALL residual that the
      // robust weight never gates: measured, 3D made 1-frame-line chroma
      // detail WORSE than 2D (5.2 vs 3.5 IRE chi error) through exactly
      // this hole. The bias is known geometry, so gate it
      // deterministically: wt *= eps^2 / (eps^2 + (0.5*dS/dline)^2),
      // horizontally smoothed. Even offsets are aligned and untouched.
      Plane vgrad;
      Plane lapsm;  // per-pixel multiplier on the odd-offset gate floor
      if (has_temporal) {
        bool any_odd = false;
        for (int o : offs) {
          const int k = j + o;
          if (k >= 0 && k < nf && ((o & 1) != 0)) any_odd = true;
        }
        if (any_odd) {
          // BASEBAND vertical envelope — NOT the composite's: within a
          // field, consecutive lines are consecutive in SCAN TIME, so the
          // carrier flips 180 deg per FIELD line and the composite of a
          // flat saturated colour alternates sign line-to-line (|dS| ~
          // 2|chi|) — a composite-based gate reads "vertical detail"
          // everywhere chroma is strong and strangles the odd equations
          // globally, chroma-correlated (measured: -8.6 dB on the
          // reference's saturated SMPTE scene). The decoded baseband
          // state (Ys, chis) measures the actual scene structure the
          // opposite parity cannot see. One-sided max diff (a central
          // diff is exactly zero ON a one-row feature), local RMS via
          // blurred squares.
          const int hh = S.height();
          const int ww = S.width();
          Plane gmag(hh, ww);
          Plane lapmag(hh, ww);  // vertical SECOND difference (see below)
          for (int y = 0; y < hh; ++y) {
            const int ym = std::max(0, y - 1);
            const int yp = std::min(hh - 1, y + 1);
            for (int x = 0; x < ww; ++x) {
              const float dyu = Ys[j].at(y, x) - Ys[j].at(ym, x);
              const float dyd = Ys[j].at(yp, x) - Ys[j].at(y, x);
              const float dcu = std::abs(chis[j].at(y, x) - chis[j].at(ym, x));
              const float dcd = std::abs(chis[j].at(yp, x) - chis[j].at(y, x));
              const float mu = dyu * dyu + dcu * dcu;
              const float md = dyd * dyd + dcd * dcd;
              gmag.at(y, x) = std::max(mu, md);
              // SECOND difference: separates the two things a large vertical
              // gradient can mean, which the magnitude-only envelope above
              // conflates (it takes max(mu, md) and discards the SIGNS).
              //   monotonic ramp/edge -> both fields sample the same
              //     transition, half a line apart: the odd equation is
              //     BIASED but informative, and the reference measured 4.4 dB
              //     of 3-D gain lost by hard-gating it. Laplacian ~ 0.
              //   single-field-line EXTREMUM -> a feature one frame line
              //     wide that THIS parity happens to sample and the opposite
              //     parity samples straight past: the odd equation is not
              //     biased, it is answering about content it never saw, and
              //     it does so with a small (hence ungated) residual.
              //     Laplacian ~ the gradient itself.
              const float ly = Ys[j].at(y, x) -
                               0.5F * (Ys[j].at(ym, x) + Ys[j].at(yp, x));
              const float lc = std::abs(chis[j].at(y, x) -
                                        0.5F * (chis[j].at(ym, x) +
                                                chis[j].at(yp, x)));
              lapmag.at(y, x) = ly * ly + lc * lc;
            }
          }
          // HORIZONTAL-only smoothing: a 2D blur dilutes a one-row
          // feature's vertical footprint by ~2.5x and weakens the gate on
          // exactly the detail it protects; vertical coverage already
          // comes free from the one-sided max (the feature row and both
          // its neighbours each carry a full-amplitude diff).
          // Shared sliding-window row smoother, applied to BOTH the
          // gradient envelope and the second-difference map so their ratio
          // below is taken on consistently filtered quantities.
          auto smooth_rows = [&](const Plane& src) {
            Plane out(hh, ww);
            for (int y = 0; y < hh; ++y) {
              // Sliding sum over [x-2, x+2] clipped to the row. The prologue
              // must prime `acc` with EXACTLY the x = 0 window, i.e. taps
              // [0, 2] = 3 of them; the loop then adds src[x + 2] once per
              // step. Priming with 5 taps (as this did) put taps 3 and 4 in
              // the sum before their steps, and the x = 1 / x = 2 steps then
              // added them AGAIN — leaving a constant excess of
              // (src[y][3] + src[y][4]) on the window sum for the WHOLE row,
              // never subtracted out, so the gate on every column of a line
              // was inflated by whatever sat at the far LEFT of the active
              // picture. Measured against the reference's cumsum
              // formulation: +17% on random content, unbounded when the left
              // edge carries a strong horizontal transition.
              float acc = 0.0F;
              for (int x = 0; x < std::min(3, ww); ++x) acc += src.at(y, x);
              for (int x = 0; x < ww; ++x) {
                const int lo = x - 2, hi = x + 2;
                if (x > 0) {
                  if (hi < ww) acc += src.at(y, hi);
                  if (lo - 1 >= 0) acc -= src.at(y, lo - 1);
                }
                const int n_taps =
                    std::min(hi, ww - 1) - std::max(lo, 0) + 1;
                out.at(y, x) = std::sqrt(acc / static_cast<float>(n_taps));
              }
            }
            return out;
          };
          vgrad = smooth_rows(gmag);
          // Noise-floor the envelope: at pass 0 the state is the
          // holographic INIT, whose vertical cross-colour/noise gives a
          // non-zero envelope everywhere — ungated that read as "detail"
          // and cost the reference chart 4 dB of 3D gain. The floor is
          // self-calibrated as the MEDIAN envelope (background dominates
          // by area): background -> gate ~ 1, true structure >> median
          // -> gate bites.
          {
            std::vector<float> sample;
            sample.reserve(vgrad.size() / 7 + 1);
            for (size_t i = 0; i < vgrad.size(); i += 7)
              sample.push_back(vgrad[i]);
            const float med = MedianOf(std::move(sample));
            const float fl2 = 2.25F * med * med;  // (1.5 * median)^2
            for (size_t i = 0; i < vgrad.size(); ++i)
              vgrad[i] = std::sqrt(
                  std::max(0.0F, vgrad[i] * vgrad[i] - fl2));
          }
          // Per-pixel EXTREMUM factor, replacing the fixed floor below.
          // t = |second difference| / |gradient| is scale-free and separates
          // the two regimes exactly: a monotonic ramp gives t -> 0 (the
          // second difference of a straight line vanishes), a one-field-line
          // spike gives t -> 1 (there the second difference IS the
          // gradient), and a step edge lands in between at ~0.5. Squaring
          // keeps step edges close to the reference's behaviour (0.5^2 =
          // 0.25, so they keep ~75% of the floor and the 4.4 dB the
          // reference measured there is largely preserved) while driving the
          // floor to 0 only on true spikes — the case where the opposite
          // parity never sampled the feature at all and its equation is not
          // biased but simply uninformed.
          lapsm = Plane(hh, ww);
          {
            Plane t_raw = smooth_rows(lapmag);
            for (size_t i = 0; i < lapsm.size(); ++i) {
              const float g = vgrad[i];
              const float t = g > 1e-6F ? std::clamp(t_raw[i] / g, 0.0F, 1.0F)
                                        : 0.0F;
              lapsm[i] = 1.0F - t * t;  // multiplies the floor
            }
          }
        }
      }
      if (has_temporal) {
        for (int o : offs) {
          const int k = j + o;
          if (k < 0 || k >= nf) continue;
          // Raw integer warp for the equations: EXACT for aligned static
          // content; residual half-line bias on odd offsets is per-pixel
          // gated. (An envelope-resampled variant was tried in the
          // reference and rejected: its 1-line comb leaks vertical luma
          // gradients into chroma.)
          NeighborRawState state;
          state.luma = Ys[k];
          state.composite = fields[k].s;
          state.carrier = fields[k].carrier;
          const MotionField& mo = pair_motion.at(o);
          const bool gate_on =
              ccfg.coherence_gate > 0.0F && !(ccfg.fast && pass == 0);
          // Share ONE per-pixel vector interpolation between the raw warps
          // and the gate's chi warp (it used to be computed twice per
          // neighbour when the gate was active).
          PerPixelMotion vpx_shared;
          if (gate_on) {
            vpx_shared = VectorsPerPixel(mo.dy, mo.dx, ccfg.mc_tile,
                                         Ys[j].height(), Ys[j].width());
          }
          MotionCompensatedResult mc = MotionCompensatePrev(
              state, Ys[j], ccfg.mc_tile, ccfg.mc_search, &mo, ccfg.fast,
              gate_on ? &vpx_shared : nullptr);
          if (gate_on) {
            // Fast: coherence only from pass 1 — at pass 0 the chi fields
            // are still inits and the measurement is barely informative.
            // InSAR coherence between the current chroma and the warped
            // neighbour chroma, floored so grey content (|chi| ~ 0, gamma
            // = pure noise) keeps the equation's LUMA benefit.
            //
            // REFERENCE BUG, fixed here (the numeric demonstration is in the tests):
            // the bilinear-warp convention assumes the motion carries NO half-line parity component
            // (row_offset supplies it), which held when the estimates were
            // margin-zeroed on static content — but the trajectory snap
            // (THEORY 9e) deliberately RE-INTRODUCES h_k = (p_k - p_j)/2
            // into the pairwise vectors, so adding row_offset on top
            // double-compensated: the gate compared chi fields shifted by
            // a FULL line on exactly the static consensus tiles it exists
            // to protect. With the snapped/subpixel motion already
            // carrying the total inter-grid displacement, the correct warp
            // is simply sy = y - dy: row_offset must be 0 here. (Worst
            // case moves from 1.0 line, on the dominant snapped case, to
            // 0.5 line on margin-zeroed non-consensus tiles only.)
            const float row_off = 0.0F;
            const int hh = Ys[j].height();
            const int ww = Ys[j].width();
            const PerPixelMotion& vpx = vpx_shared;
            Plane re(hh, ww), im(hh, ww);
            for (size_t i = 0; i < chis[k].size(); ++i) {
              re[i] = chis[k][i].real();
              im[i] = chis[k][i].imag();
            }
            const Plane wre = WarpBilinearTiles(re, vpx, row_off, hh, ww);
            const Plane wim = WarpBilinearTiles(im, vpx, row_off, hh, ww);
            ComplexPlane cw(hh, ww);
            for (size_t i = 0; i < cw.size(); ++i)
              cw[i] = Complex{wre[i], wim[i]};
            const Plane gamma = ComplexCoherence(chis[j], cw, 6);
            const float a = ccfg.coherence_gate;
            for (size_t i = 0; i < mc.confidence.size(); ++i)
              mc.confidence[i] *= (1.0F - a) + a * gamma[i];
          }
          if ((o & 1) != 0 && !vgrad.empty()) {
            // ENVELOPE gate, floored. The residual cannot do this job:
            // at a chroma feature the odd equation's residual OSCILLATES
            // with x (|dchi * cos|), so its robust weight gates the
            // equation at some columns and lets confident wrong votes
            // through at the cosine zeros — only the phase-independent
            // ENVELOPE catches all of them (this is why the residual-
            // dependent variant measured worse). The 0.25 floor keeps a
            // quarter of the weight alive everywhere: on step-edge-heavy
            // content (the reference's saturated chart) the odd
            // equations are biased but informative and hard-gating them
            // cost 4.4 dB of 3D gain; a floored gate keeps ~75% of the
            // thin-detail protection while leaving the solve enough
            // equation mass to keep absorbing edge bias.
            const float e2 = ccfg.temporal_eps * ccfg.temporal_eps;
            const float floor_max = std::clamp(ccfg.odd_gate_floor, 0.0F, 1.0F);
            for (size_t i = 0; i < mc.confidence.size(); ++i) {
              const float b = vgrad[i];
              // Floor is now PER PIXEL: the ceiling from config, scaled by
              // how much this pixel looks like a monotonic edge (keep the
              // floor) rather than a one-field-line extremum (drop it).
              const float floor_i =
                  floor_max * (lapsm.empty() ? 1.0F : lapsm[i]);
              mc.confidence[i] *=
                  std::max(floor_i, e2 / (e2 + b * b));
            }
          }
          neighbors.push_back(
              NeighborTerm{std::move(mc.composite), std::move(mc.carrier),
                           std::move(mc.confidence)});
        }
      }

      if (pass == 0 && ccfg.psi_init && !neighbors.empty()) {
        chis[j] = PsiClosedForm(S, carrier, neighbors, chis[j]);
      }
      if (pass >= 1 && ccfg.nr_anchor > 0.0F && nf > 1) {
        const AnchorTerm anchor = SynthReference(
            j, Ys, chis, fields, ccfg, parities,
            ccfg.fast ? &mcache_fast : nullptr,
            ccfg.fast ? &mcache_mu : nullptr);
        RefineResult r = VariationalRefineJoint(S, carrier, Ys[j], chis[j],
                                                ccfg, neighbors, &anchor);
        Ys[j] = std::move(r.luma);
        chis[j] = std::move(r.chroma);
      } else {
        RefineResult r = VariationalRefine(S, carrier, chis[j], ccfg, neighbors);
        Ys[j] = std::move(r.luma);
        chis[j] = std::move(r.chroma);
      }
    };

    if (ccfg.fast || !has_temporal) {
      // COLORED Gauss-Seidel sweep (the fast-mode math change that unlocks
      // field-level parallelism): field j only READS fields within
      // `reach` = max(largest temporal offset, nr_radius) of itself (the
      // neighbour equations, the coherence gate, the anchor blend), so
      // fields spaced `reach + 1` apart share no read/write dependency and
      // can be solved CONCURRENTLY. Sweeping color-by-color keeps the
      // Gauss-Seidel character (each color reads the freshest available
      // state of the others) — same fixed point as the sequential sweep,
      // a different but convergent iteration trajectory; measured within
      // the fast-mode quality envelope on the regression scene. The
      // sequential sweep stays the slow-mode behaviour, bit-faithful to
      // the reference. Inside the parallel region the solvers' own OpenMP
      // loops deactivate (no nested parallelism); the intent was "one
      // field per core beats one memory-bound loop across all cores",
      // but that's a plausible-sounding claim that was never actually
      // profiled on real hardware -- see HvdConfig::parallel_across_fields
      // for an A/B toggle between this and the opposite strategy (decode
      // fields one at a time, letting each one's internal OpenMP loops use
      // every core, same shape as the GUI preview's single-frame decode).
      int max_off = 0;
      for (int o : offs) max_off = std::max(max_off, std::abs(o));
      const int reach =
          std::max(max_off, ccfg.nr_anchor > 0.0F ? ccfg.nr_radius : 0);
      // With no temporal terms the fields are fully decoupled: one color,
      // every field concurrent (this is the per-field 2D decode path).
      const int stride = has_temporal ? reach + 1 : 1;
      if (ccfg.parallel_across_fields) {
        for (int color = 0; color < stride; ++color) {
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
          for (int j = color; j < nf; j += stride) solve_field(j);
        }
      } else {
        // A/B comparison path (parallel_across_fields = false): no outer
        // parallel region at all, so each solve_field(j) call's own
        // internal OpenMP loops are free to use every core for THAT one
        // field -- exactly the shape of the GUI preview's single-frame
        // decode. Correctness is identical either way when stride == 1
        // (the decoupled/fast case this branch is reached for): fields
        // don't read each other's state, so the order they're solved in
        // doesn't change the result, only the wall-clock time.
        for (int j = 0; j < nf; ++j) solve_field(j);
      }
    } else {
      for (int j = 0; j < nf; ++j) solve_field(j);
    }
  }

  for (int j = 0; j < nf; ++j) {
    out[j].chroma = std::move(chis[j]);
    if (ccfg.output_fidelity) {
      // Purist output: chroma keeps the fully guided separation, luma
      // reverts to exact data fidelity (no NR in the deliverable).
      out[j].luma = Plane(fields[j].s.height(), fields[j].s.width());
      for (size_t i = 0; i < out[j].luma.size(); ++i)
        out[j].luma[i] = fields[j].s[i] -
                         (out[j].chroma[i] * fields[j].carrier[i]).real();
    } else {
      out[j].luma = std::move(Ys[j]);
    }
  }
  return out;
}

}  // namespace hvd
