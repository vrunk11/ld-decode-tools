// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/holographic_init.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/gradients.h"

namespace hvd {

namespace {

// numpy.fft.fftfreq(n, d): [0, 1, ..., ceil(n/2)-1, -floor(n/2), ..., -1] / (n*d)
std::vector<float> FftFreq(int n, double d) {
  std::vector<float> f(n);
  const double inv = 1.0 / (static_cast<double>(n) * d);
  const int half = (n % 2 == 0) ? n / 2 : (n + 1) / 2;  // count of non-negative
  for (int i = 0; i < n; ++i) {
    const int k = (i < half) ? i : i - n;
    f[i] = static_cast<float>(k * inv);
  }
  return f;
}

// Separable 2-D Gaussian low-pass response (the "hologram crop"), built from
// per-axis cutoffs. cutoff_x in Hz; cutoff_y in cycles/line. `h_ref` is the
// height the cycles-per-picture-height cutoff is defined against (the REAL
// field height) — it differs from `h` when the transform runs on a
// vertically mirror-padded plane; normalising by the padded height would
// silently narrow the vertical band by (h_ref / h).
void GaussianAxes(int h, int w, const FieldGeometry& g, float lpf_h_mhz, float lpf_v_cph, int h_ref,
                  std::vector<float>* gx_out, std::vector<float>* gy_out) {
  const std::vector<float> fx = FftFreq(w, 1.0 / g.sample_rate);  // Hz
  const std::vector<float> fy = FftFreq(h, 1.0);                  // cyc/line
  const float cutoff_x = lpf_h_mhz * 1.0e6F;
  const float cutoff_y = lpf_v_cph / (2.0F * static_cast<float>(h_ref));

  std::vector<float> gx(w), gy(h);
  for (int x = 0; x < w; ++x) {
    const float r = fx[x] / cutoff_x;
    gx[x] = std::exp(-0.5F * r * r);
  }
  for (int y = 0; y < h; ++y) {
    const float r = fy[y] / cutoff_y;
    gy[y] = std::exp(-0.5F * r * r);
  }
  *gx_out = std::move(gx);
  *gy_out = std::move(gy);
}

Plane GaussianLpf(int h, int w, const FieldGeometry& g, float lpf_h_mhz, float lpf_v_cph, int h_ref) {
  std::vector<float> gx, gy;
  GaussianAxes(h, w, g, lpf_h_mhz, lpf_v_cph, h_ref, &gx, &gy);
  Plane g_out(h, w);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) g_out.at(y, x) = gy[y] * gx[x];
  return g_out;
}

// 1-D box filter of length (2r+1), zero-padded, 'same' output, normalised by the
// full length — matching numpy.convolve(..., 'same') on a 1/(2r+1) kernel
// (edges are attenuated by the zero padding).
// Each output is the sum of the (2r+1) neighbours added in the same order as before (the first row or column first), so the result is
// the same to the bit, but the loops run along the lines of the plane, in the order the data lies, and vectorise.
void BoxBlur1DRows(Plane* a, int r) {
  const int h = a->height();
  const int w = a->width();
  const float norm = 1.0F / static_cast<float>(2 * r + 1);
  const Plane src = *a;
  const float* sp = src.data();
  float* dp = a->data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    float* out = dp + static_cast<long>(y) * w;
    for (int x = 0; x < w; ++x) out[x] = 0.0F;
    for (int k = -r; k <= r; ++k) {
      const int yy = y + k;
      if (yy < 0 || yy >= h) continue;
      const float* in = sp + static_cast<long>(yy) * w;
      for (int x = 0; x < w; ++x) out[x] += in[x];
    }
    for (int x = 0; x < w; ++x) out[x] *= norm;
  }
}

void BoxBlur1DCols(Plane* a, int r) {
  const int h = a->height();
  const int w = a->width();
  const float norm = 1.0F / static_cast<float>(2 * r + 1);
  const Plane src = *a;
  const float* sp = src.data();
  float* dp = a->data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const float* in = sp + static_cast<long>(y) * w;
    float* out = dp + static_cast<long>(y) * w;
    // The ends, where the window leaves the line
    for (int x = 0; x < w; ++x) {
      if (x >= r && x + r < w) {
        x = w - r - 1;
        continue;
      }
      float acc = 0.0F;
      for (int k = -r; k <= r; ++k) {
        const int xx = x + k;
        if (xx >= 0 && xx < w) acc += in[xx];
      }
      out[x] = acc * norm;
    }
    // The middle, all the window in the line
    for (int x = r; x + r < w; ++x) {
      float acc = 0.0F;
      for (int k = -r; k <= r; ++k) acc += in[x + k];
      out[x] = acc * norm;
    }
  }
}

Plane BoxBlur(Plane a, int r) {
  BoxBlur1DRows(&a, r);
  BoxBlur1DCols(&a, r);
  return a;
}

// reflect101 row index into [0, h): ... 2 1 | 0 1 2 ... h-1 | h-2 h-3 ...
// The EDGE ROW IS NOT DUPLICATED, and that is load-bearing, not a style
// choice: within a field the carrier alternates ~180 deg per line, so the
// luma leak in the demodulated signal alternates SIGN line-to-line.
// reflect101 preserves that (-1)^y parity across the seam (row -1 mirrors
// row +1: same parity); a symmetric mirror (edge duplicated) puts two
// same-sign leak rows adjacent, manufacturing a low-vertical-frequency
// leak component at the border that the 30 c/ph crop passes straight into
// chi — measured on a synthetic field with a realistic alternating
// carrier: no padding 0.25 IRE border chi error, symmetric mirror 0.30
// (WORSE at the top), reflect101 0.04.
int MirrorRow(int y, int h) {
  if (h <= 1) return 0;
  const int period = 2 * h - 2;
  y = ((y % period) + period) % period;
  return y < h ? y : period - y;
}

// Vertical mirror padding: `pad` reflected rows above and below. The FFT the
// init runs is CIRCULAR — without padding, the vertical LPF (support ~ +/-8
// field lines at the 30 c/ph variant) blends the TOP rows of the picture
// with the BOTTOM rows and vice versa, which is exactly the band of residual
// carrier garbage visible on the first/last active lines of every field
// (and output_fidelity then writes any chi error straight back into Y as
// dots). Mirror padding makes the wrap-around see a smooth continuation
// instead of the opposite edge.
ComplexPlane PadRowsMirror(const ComplexPlane& a, int pad) {
  const int h = a.height();
  const int w = a.width();
  ComplexPlane out(h + 2 * pad, w);
  for (int y = 0; y < h + 2 * pad; ++y) {
    const int sy = MirrorRow(y - pad, h);
    for (int x = 0; x < w; ++x) out.at(y, x) = a.at(sy, x);
  }
  return out;
}

ComplexPlane CropRows(const ComplexPlane& a, int pad, int h) {
  ComplexPlane out(h, a.width());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < a.width(); ++x) out.at(y, x) = a.at(y + pad, x);
  return out;
}

// Re[chi * carrier] as a real plane.
Plane RealOfProduct(const ComplexPlane& chi, const ComplexPlane& carrier) {
  Plane out(chi.height(), chi.width());
  const long n = static_cast<long>(chi.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long i = 0; i < n; ++i) out[i] = (chi[i] * carrier[i]).real();
  return out;
}

// Residual luma Y = S - Re[chi * carrier].
Plane ResidualLuma(const Plane& s, const ComplexPlane& chi,
                   const ComplexPlane& carrier) {
  Plane y(s.height(), s.width());
  const long n = static_cast<long>(s.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long i = 0; i < n; ++i)
    y[i] = s[i] - (chi[i] * carrier[i]).real();
  return y;
}

// Edge-energy map E = box_blur(|Dx Y| + |Dy Y|, r=3), used as the inverse
// blend weight (smoother residual luma => higher weight).
Plane EdgeEnergy(const Plane& y) {
  const int h = y.height();
  const int w = y.width();
  Plane e(h, w);
  const float* yp = y.data();
  float* ep = e.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int r = 0; r < h; ++r) {
    const long row = static_cast<long>(r) * w;
    for (int x = 0; x < w; ++x) {
      const long i = row + x;
      const float gx = x + 1 < w ? yp[i + 1] - yp[i] : 0.0F;
      const float gy = r + 1 < h ? yp[i + w] - yp[i] : 0.0F;
      ep[i] = std::fabs(gx) + std::fabs(gy);
    }
  }
  return BoxBlur(std::move(e), 3);
}

}  // namespace

HoloInit HolographicInit(const Plane& s, const ComplexPlane& carrier,
                         const FieldGeometry& g, const HvdConfig& cfg,
                         Fft2d* fft) {
  const int h = s.height();
  const int w = s.width();
  const long n_total = static_cast<long>(s.size());

  // The complex planes are read and written as floats, real and imaginary part one after the other: the loops below do the same arithmetic as
  // the std::complex expressions they replace, element by element, and the compiler can vectorise them (it does not vectorise std::complex).
  const float* sp = s.data();
  const float* car = reinterpret_cast<const float*>(carrier.data());

  // demod = S * conj(carrier); shift the chroma sideband to DC.
  ComplexPlane demod(h, w);
  {
    float* dm = reinterpret_cast<float*>(demod.data());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n_total; ++i) {
      dm[2 * i] = sp[i] * car[2 * i];
      dm[2 * i + 1] = sp[i] * -car[2 * i + 1];
    }
  }

  // Vertical mirror padding before the (circular) FFT — see PadRowsMirror.
  // 16 lines comfortably covers the widest vertical kernel support (~+/-8
  // lines at the 30 c/ph crop). Horizontal wrap is left alone: its kernel
  // support is ~+/-9 samples into near-blanking content, visually inert,
  // and keeping `w` unchanged keeps the cached FFT plan width shared with
  // every other call site.
  const int pad = std::min(16, h > 1 ? h - 1 : 0);
  const int hp = h + 2 * pad;
  const ComplexPlane spectrum = fft->Forward(PadRowsMirror(demod, pad));
  const float* spf = reinterpret_cast<const float*>(spectrum.data());

  // Accumulate the per-pixel weighted blend of the crop variants.
  ComplexPlane chi_num(h, w);
  Plane w_sum(h, w);
  float* cnum = reinterpret_cast<float*>(chi_num.data());
  float* wsum = w_sum.data();

  // The variant of the blend: a chroma plane (chi, as floats) of the field, its residual luma, the weight from the edge energy of that luma,
  // and the sums of the blend
  auto accumulate = [&](const float* chi_f) {
    Plane y_v(h, w);
    float* yv = y_v.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n_total; ++i) yv[i] = sp[i] - (chi_f[2 * i] * car[2 * i] - chi_f[2 * i + 1] * car[2 * i + 1]);
    const Plane e = EdgeEnergy(y_v);
    const float* ep = e.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n_total; ++i) {
      const float weight = 1.0F / (ep[i] + 0.5F);
      cnum[2 * i] += chi_f[2 * i] * weight;
      cnum[2 * i + 1] += chi_f[2 * i + 1] * weight;
      wsum[i] += weight;
    }
  };

  // Two complementary anisotropic crops: (narrow-x, wide-y) and (wide-x,
  // narrow-y). Values copied verbatim from the reference.
  // NOTE: this outer loop (2 iterations) stays sequential on purpose — the
  // FFT plan cache (Fft2d::PlanFor) isn't safe to look up/insert into
  // concurrently from multiple threads sharing one Fft2d, and chi_num/
  // w_sum are shared accumulators across variants. The per-pixel loops
  // WITHIN each variant below are what's parallelised instead — that's
  // most of the actual elementwise work anyway.
  const float variants[2][2] = {{0.8F, 120.0F}, {1.8F, 30.0F}};
  const long n_pad = static_cast<long>(hp) * w;
  for (const auto& v : variants) {
    // Kernel at the PADDED size; the vertical cutoff stays defined against
    // the real field height `h` (cycles/picture-height semantics).
    // The kernel is the product of a curve along the lines and a curve along the columns: it is not made as a plane, it is applied as it is
    std::vector<float> kx, ky;
    GaussianAxes(hp, w, g, v[0], v[1], h, &kx, &ky);
    ComplexPlane cropped(hp, w);
    float* cr = reinterpret_cast<float*>(cropped.data());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < hp; ++y) {
      const long row = static_cast<long>(y) * w;
      const float kyy = ky[y];
      HVD_SIMD
      for (int x = 0; x < w; ++x) {
        const float kernel = kyy * kx[x];
        cr[2 * (row + x)] = spf[2 * (row + x)] * kernel;
        cr[2 * (row + x) + 1] = spf[2 * (row + x) + 1] * kernel;
      }
    }
    const ComplexPlane inverse = fft->Inverse(cropped);
    // The rows of the field out of the padded ones, doubled (the sideband that was kept is half of the chroma)
    ComplexPlane chi_v(h, w);
    {
      const float* inf = reinterpret_cast<const float*>(inverse.data()) + 2 * static_cast<long>(pad) * w;
      float* cvf = reinterpret_cast<float*>(chi_v.data());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for (long i = 0; i < 2 * n_total; ++i) cvf[i] = inf[i] * 2.0F;
    }
    accumulate(reinterpret_cast<const float*>(chi_v.data()));
  }
  (void)n_pad;

  // Optional third variant: spectral-symmetry certified chroma ("Transform
  // NTSC, repaired"). Off by default; see the reference for the rationale.
  if (cfg.symmetry_variant) {
    // sym[k] = min(|D[k]|, |D[-k reflected]|) / (|D[k]| + eps), a lower bound
    // of chroma that luma almost never fakes. The point-reflection is
    // roll(roll(D[::-1,::-1], 1, 0), 1, 1) so bin k pairs with bin -k.
    Plane mag(hp, w);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n_pad; ++i) mag[i] = std::abs(spectrum[i]);
    ComplexPlane certified(hp, w);
    const Plane kernel = GaussianLpf(hp, w, g, 1.3F, 60.0F, h);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < hp; ++y) {
      for (int x = 0; x < w; ++x) {
        const int ry = (hp - y) % hp;  // reflect+shift by 1 => index (hp-y) mod hp
        const int rx = (w - x) % w;
        const float m = mag.at(y, x);
        const float mr = mag.at(ry, rx);
        const float sym = std::min(m, mr) / (m + 1e-6F);
        certified.at(y, x) = spectrum.at(y, x) * sym * kernel.at(y, x);
      }
    }
    ComplexPlane chi_s = CropRows(fft->Inverse(certified), pad, h);
    for (long i = 0; i < n_total; ++i) chi_s[i] *= 2.0F;
    accumulate(reinterpret_cast<const float*>(chi_s.data()));
  }

  HoloInit out;
  out.chroma = ComplexPlane(h, w);
  {
    float* of = reinterpret_cast<float*>(out.chroma.data());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < n_total; ++i) {
      of[2 * i] = cnum[2 * i] / wsum[i];
      of[2 * i + 1] = cnum[2 * i + 1] / wsum[i];
    }
  }
  out.luma = ResidualLuma(s, out.chroma, carrier);
  return out;
}

}  // namespace hvd
