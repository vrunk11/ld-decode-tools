// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/lockin.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace hvd {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kHalfPi = 1.57079632679489661923F;

float MedianOf(std::vector<float> v) {
  if (v.empty()) return 0.0F;
  const size_t k = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

// Wrap an angle to (-pi, pi], matching numpy's angle(exp(i*x)).
float WrapPi(float a) {
  return std::atan2(std::sin(a), std::cos(a));
}

// Complex lock-in output per line over the burst window:
//   z[line] = mean_x[ (seg - mean(seg)) * exp(-i w0 x) ]
// with x the ABSOLUTE sample index (so the local oscillator matches the carrier
// referenced to line start, exactly as MakeCarrier does) and w0 the carrier's
// phase advance per sample (pi/2 on a 4fsc grid, g.phase_per_sample() for a
// non-standard subcarrier). Returns z and |z|.
void BurstLockin(const Plane& field_ire, const FieldGeometry& g,
                 std::vector<Complex>* z_out, std::vector<float>* amp_out) {
  const int h = field_ire.height();
  const int x0 = g.colour_burst_start;
  const int x1 = g.colour_burst_end;
  const int n = std::max(0, x1 - x0);
  const float w0 = static_cast<float>(g.phase_per_sample());

  z_out->assign(h, Complex{0.0F, 0.0F});
  amp_out->assign(h, 0.0F);
  if (n == 0) return;

  for (int y = 0; y < h; ++y) {
    // DC pedestal removal: subtract the burst-window mean.
    float mean = 0.0F;
    for (int x = x0; x < x1; ++x) mean += field_ire.at(y, x);
    mean /= static_cast<float>(n);

    Complex acc{0.0F, 0.0F};
    for (int x = x0; x < x1; ++x) {
      const Complex ref = std::polar(1.0F, -w0 * static_cast<float>(x));
      acc += (field_ire.at(y, x) - mean) * ref;
    }
    acc /= static_cast<float>(n);
    (*z_out)[y] = acc;
    (*amp_out)[y] = std::abs(acc);
  }
}
}  // namespace

std::vector<float> TridiagSmooth(const std::vector<float>& d,
                                 const std::vector<float>& a, float lam) {
  const int n = static_cast<int>(d.size());
  std::vector<float> x(n, 0.0F);
  if (n == 0) return x;
  if (n == 1) {
    x[0] = a[0] > 0.0F ? d[0] : 0.0F;
    return x;
  }

  // System matrix: diag(a) + lam * L, L the 1-D graph Laplacian (Neumann ends).
  std::vector<float> diag(n);
  for (int i = 0; i < n; ++i) diag[i] = a[i] + 2.0F * lam;
  diag[0] -= lam;
  diag[n - 1] -= lam;
  const float off = -lam;  // constant off-diagonal

  // rhs = a * d
  std::vector<float> rhs(n);
  for (int i = 0; i < n; ++i) rhs[i] = a[i] * d[i];

  // Thomas algorithm (forward elimination, back substitution).
  std::vector<float> c(n - 1);
  std::vector<float> dd(n);
  c[0] = off / diag[0];
  dd[0] = rhs[0] / diag[0];
  for (int i = 1; i < n; ++i) {
    const float m = diag[i] - off * c[i - 1];
    if (i < n - 1) c[i] = off / m;
    dd[i] = (rhs[i] - off * dd[i - 1]) / m;
  }
  x[n - 1] = dd[n - 1];
  for (int i = n - 2; i >= 0; --i) x[i] = dd[i] - c[i] * x[i + 1];
  return x;
}

std::vector<float> BurstLockinPhase(const Plane& field_ire,
                                    const FieldGeometry& g) {
  const int h = field_ire.height();
  std::vector<Complex> z;
  std::vector<float> amp;
  BurstLockin(field_ire, g, &z, &amp);

  // SIGN OF THE BURST -- the source of the long-standing 180 deg error.
  //
  // The composite model is S = Y + Re[chi * e^{i phi}] with chi = V - iU,
  // i.e. S_chroma = U sin(phi) + V cos(phi). The NTSC burst sits on the
  // -U axis (SMPTE 170M-2004 §8.2: 180 deg), so U = -B, V = 0 and
  //
  //     chi_burst = V - iU = +iB = B e^{i pi/2}
  //     burst(x)  = Re[chi_burst e^{i phi}] = -B sin(phi)
  //
  // NOT +B sin(phi). The old derivation (and reference/hvd/encode.py:141,
  // fixed alongside this) evaluated Re[i A e^{i phi}] as +A sin(phi); it is
  // -A sin(phi). Feeding that through the lock-in:
  //
  //     z = mean(burst(x) e^{-i w0 x}) = (B/2) e^{i(theta + pi/2)}
  //     => theta = arg(z) - pi/2
  //
  // The old `+ kHalfPi` was therefore exactly pi out, which is why chroma
  // "has been persistently 180 deg off since the Python reference" and why
  // HvdConfig::chroma_phase_deg defaulted to 180 to cancel it. That default
  // is now 0 -- the compensation belongs here, not in a user-facing trim
  // control, and it must NOT be applied on PAL, whose lock-in below was
  // already correct (see BurstLockinPhasePal).
  std::vector<float> theta(h, 0.0F);
  for (int y = 0; y < h; ++y) theta[y] = std::arg(z[y]) - kHalfPi;

  // "good" lines: burst amplitude above 20 % of the field maximum.
  const float amp_max = amp.empty() ? 0.0F : *std::max_element(amp.begin(),
                                                               amp.end());
  const float good_thresh = amp_max > 0.0F ? amp_max * 0.2F : 1.0F;
  std::vector<char> good(h, 0);
  int first_good = -1;
  for (int y = 0; y < h; ++y) {
    good[y] = amp[y] > good_thresh ? 1 : 0;
    if (good[y] && first_good < 0) first_good = y;
  }
  if (first_good < 0) return theta;  // no burst anywhere: keep raw angles

  // Model phase: line_advance() per line, anchored at the first good line.
  // Derived, not hardcoded pi: 180 deg is the NTSC 4fsc value, but a
  // non-standard subcarrier advances by whatever fsc/fH says (see
  // FieldGeometry::line_advance). Getting this wrong leaves a per-line
  // phase RAMP in the deviations d below, which the tridiagonal smoother
  // then fights instead of smoothing.
  const float adv = static_cast<float>(g.line_advance());
  std::vector<float> model(h);
  for (int y = 0; y < h; ++y) {
    model[y] = theta[first_good] + adv * static_cast<float>(y - first_good);
  }

  // Deviation d = wrap(theta - model) about the model.
  std::vector<float> d(h);
  for (int y = 0; y < h; ++y) d[y] = WrapPi(theta[y] - model[y]);

  // Amplitude-derived weights, normalised by the median good amplitude and
  // clipped to [0, 2]; zero on lines with no usable burst.
  std::vector<float> good_amp;
  for (int y = 0; y < h; ++y)
    if (good[y]) good_amp.push_back(amp[y]);
  const float med_amp = MedianOf(good_amp) + 1e-9F;

  std::vector<float> a(h, 0.0F);
  for (int y = 0; y < h; ++y) {
    if (good[y]) a[y] = std::clamp(amp[y] / med_amp, 0.0F, 2.0F);
  }

  const float lam = 25.0F;
  std::vector<float> x = TridiagSmooth(d, a, lam);

  // One IRLS outlier rejection pass (Huber-like, scale ~0.15 rad).
  std::vector<float> a2(h, 0.0F);
  for (int y = 0; y < h; ++y) {
    const float r = std::fabs(d[y] - x[y]);
    a2[y] = a[y] * 0.15F / std::max(r, 0.15F);
  }
  x = TridiagSmooth(d, a2, lam);

  for (int y = 0; y < h; ++y) theta[y] = model[y] + x[y];
  return theta;
}

float BurstAmplitudeIre(const Plane& field_ire, const FieldGeometry& g) {
  // AMPLITUDE, unlike PHASE, must not be a coherent average over the whole
  // burst window.
  //
  // The window comes from orc::colour_burst_range() and is a fixed 36
  // samples (9 cycles) for NTSC / 40 (10 cycles) for PAL -- the NOMINAL
  // burst length. But SMPTE 170M-2004 Table 2 permits 9 +/- 1 cycles, and
  // a TBC's sample-0 reference is not guaranteed to place the burst
  // identically on every capture. Whenever the burst fills only m of the
  // window's n samples, a coherent mean over all n reports
  //
  //     A * m / n        instead of        A
  //
  // -- the amplitude is DILUTED by the empty part of the window, linearly.
  // A perfectly legal 8-cycle burst reads 17.78 IRE instead of 20.00, so
  // the ACC applies 1.125; a 4-sample window misalignment does the same;
  // the two together land near 1.25, i.e. 25 % over-saturation that the
  // operator then has to cancel by hand with Chroma Gain ~0.8.
  //
  // Note the host's decoders are immune to this by construction, which is
  // why the window was never sized for it: palcolour.cpp and comb.cpp
  // normalise the burst vector to UNIT MAGNITUDE and use it as a phase
  // reference only, so any dilution cancels out. This decoder is the only
  // one that consumes the burst's amplitude, so it needs an estimator that
  // does not care how much of the window is empty.
  //
  // Estimator: slide a ONE-CYCLE (4-sample) coherent demodulation across
  // the window to get the local complex envelope, then take the MEDIAN of
  // its magnitude. One full cycle at 4fsc rejects DC exactly (the four
  // unit phasors sum to zero), so no pedestal estimate is needed either --
  // and the pedestal estimate was itself contaminated by the empty part of
  // the window. The median is unbiased as long as more than half the
  // window carries burst, and it does not dilute at all below that; it
  // simply degrades to the amplitude of whatever burst is present.
  const int h = field_ire.height();
  const int x0 = g.colour_burst_start;
  const int x1 = g.colour_burst_end;
  const float w0 = static_cast<float>(g.phase_per_sample());
  if (x1 - x0 < 4) return g.nominal_burst_ire();

  std::vector<float> per_line;
  per_line.reserve(static_cast<size_t>(h));
  std::vector<float> env;
  for (int y = 0; y < h; ++y) {
    env.clear();
    for (int s = x0; s + 4 <= x1; ++s) {
      Complex acc{0.0F, 0.0F};
      for (int k = 0; k < 4; ++k) {
        const int x = s + k;
        acc += field_ire.at(y, x) *
               std::polar(1.0F, -w0 * static_cast<float>(x));
      }
      // |z| = A/2 for a sinusoid of peak amplitude A.
      env.push_back(2.0F * std::abs(acc) * 0.25F);
    }
    if (!env.empty()) per_line.push_back(MedianOf(env));
  }
  if (per_line.empty()) return g.nominal_burst_ire();

  // Across lines: drop the ones with no usable burst (dropouts, VBI), then
  // take the median of the rest -- unchanged in spirit from before.
  const float med_all = MedianOf(per_line);
  std::vector<float> good;
  for (float v : per_line) {
    if (v > 0.25F * med_all) good.push_back(v);
  }
  return good.empty() ? g.nominal_burst_ire() : MedianOf(good);
}

PalBurstLockin BurstLockinPhasePal(const Plane& field_ire,
                                   const FieldGeometry& g) {
  const int h = field_ire.height();
  std::vector<Complex> z;
  std::vector<float> amp;
  BurstLockin(field_ire, g, &z, &amp);

  PalBurstLockin out;
  out.theta.assign(h, 0.0F);
  out.vswitch.assign(h, 1);

  const float adv = static_cast<float>(g.line_advance());  // 3*pi/2 for standard PAL
  constexpr float kQuarterPi = 0.78539816339744830962F;

  const float amp_max =
      amp.empty() ? 0.0F : *std::max_element(amp.begin(), amp.end());
  const float good_thresh = amp_max > 0.0F ? amp_max * 0.2F : 1.0F;
  std::vector<char> good(h, 0);
  int first_good = -1;
  for (int y = 0; y < h; ++y) {
    good[y] = amp[y] > good_thresh ? 1 : 0;
    if (good[y] && first_good < 0) first_good = y;
  }
  if (first_good < 0) {
    // No burst anywhere: model phase, alternating parity by convention.
    for (int y = 0; y < h; ++y) {
      out.theta[y] = adv * static_cast<float>(y);
      out.vswitch[y] = (y % 2 == 0) ? 1 : -1;
    }
    return out;
  }
  // The V-switch of each line, line by line, as PALcolour decides it (palcolour.cpp, detectBurst): the burst of the line, taken against the lines two
  // above and below (the same V-switch, the opposite carrier), and the burst of the lines one above and below (the other V-switch), say which of the
  // two the line is by how far the two bursts lie from each other. It needs no model of how the phase goes from line to line, which a tape does not
  // keep, and it is what the two parities below are scored against. +1 or -1, 0 where the lines it needs are not there or the burst is weak.
  std::vector<int8_t> vote(h, 0);
  {
    auto bp = [&](int y) { return -z[y].imag(); };
    auto bq = [&](int y) { return z[y].real(); };
    for (int y = 2; y + 2 < h; ++y) {
      if (!(good[y] && good[y - 1] && good[y + 1] && good[y - 2] && good[y + 2])) continue;
      const float p = (bp(y) - 0.5F * (bp(y - 2) + bp(y + 2))) * 0.5F;
      const float q = (bq(y) - 0.5F * (bq(y - 2) + bq(y + 2))) * 0.5F;
      const float po = (bp(y + 1) - bp(y - 1)) * 0.5F;
      const float qo = (bq(y + 1) - bq(y - 1)) * 0.5F;
      const float dist = (p - po) * (p - po) + (q - qo) * (q - qo);
      vote[y] = dist < (p * p + q * q) * 2.0F ? 1 : -1;
    }
  }

  std::vector<float> a_meas(h);
  for (int y = 0; y < h; ++y) a_meas[y] = std::arg(z[y]);

  // (625-line PAL: PALcolour reads PAL-M with its sine and cosine swapped, and the sense of its votes has not been checked against a PAL-M capture)
  const bool use_votes = g.standard == VideoStandard::kPal;
  float vote_mass = 0.0F;
  for (int y = 0; y < h; ++y) vote_mass += static_cast<float>(std::abs(vote[y])) * amp[y];

  // Two parity hypotheses for the reference line, scored by how well the
  // alternating +/-45 deg swing explains every good line.
  float best_score = -1e30F;
  std::vector<float> model(h);
  std::vector<int8_t> s_best(h, 1);
  for (int hyp = 0; hyp < 2; ++hyp) {
    const float s_ref = hyp == 0 ? 1.0F : -1.0F;
    const float theta_ref =
        a_meas[first_good] - kHalfPi + s_ref * kQuarterPi;
    std::vector<float> m(h);
    std::vector<int8_t> s(h);
    float score = 0.0F;
    for (int y = 0; y < h; ++y) {
      m[y] = theta_ref + adv * static_cast<float>(y - first_good);
      const bool same = ((y - first_good) % 2) == 0;
      const float sy = same ? s_ref : -s_ref;
      s[y] = sy > 0 ? 1 : -1;
      if (good[y]) {
        const float dev = WrapPi(a_meas[y] - (m[y] + kHalfPi - sy * kQuarterPi));
        score += std::cos(dev) * amp[y];
      }
    }
    // The votes of the lines decide when they are clear; the model of the phase from line to line, which a tape does not keep, only when they are not
    float vscore = 0.0F;
    for (int y = 0; y < h; ++y) vscore += static_cast<float>(vote[y] * s[y]) * amp[y];
    const bool decided = use_votes && vote_mass > 0.0F && std::fabs(vscore) > 0.3F * vote_mass;
    const float key = decided ? 1.0e6F + vscore : score;
    if (key > best_score) {
      best_score = key;
      model = m;
      s_best = s;
    }
  }
  out.vswitch = s_best;

  // Per-line phase with the parity swing removed, then the same robust
  // trajectory smoothing as the NTSC path (tridiag + one Huber IRLS).
  std::vector<float> d(h);
  for (int y = 0; y < h; ++y) {
    const float sy = s_best[y] > 0 ? 1.0F : -1.0F;
    const float theta_meas = a_meas[y] - kHalfPi + sy * kQuarterPi;
    d[y] = WrapPi(theta_meas - model[y]);
  }

  std::vector<float> good_amp;
  for (int y = 0; y < h; ++y)
    if (good[y]) good_amp.push_back(amp[y]);
  const float med_amp = MedianOf(good_amp) + 1e-9F;
  std::vector<float> a(h, 0.0F);
  for (int y = 0; y < h; ++y)
    if (good[y]) a[y] = std::clamp(amp[y] / med_amp, 0.0F, 2.0F);

  const float lam = 25.0F;
  std::vector<float> xs = TridiagSmooth(d, a, lam);
  std::vector<float> a2(h, 0.0F);
  for (int y = 0; y < h; ++y) {
    const float r = std::abs(d[y] - xs[y]);
    a2[y] = a[y] * 0.15F / std::max(r, 0.15F);
  }
  xs = TridiagSmooth(d, a2, lam);
  for (int y = 0; y < h; ++y) out.theta[y] = model[y] + xs[y];
  return out;
}

}  // namespace hvd
