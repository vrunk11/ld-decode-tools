// SPDX-License-Identifier: GPL-3.0-or-later
//
// gradients.h — forward-difference operators with Neumann boundaries and their
// EXACT adjoints. Ports `_dx`, `_dy`, `_dxT`, `_dyT` from the reference.
//
// Why Neumann (not periodic): a periodic roll couples opposite image borders
// through the priors — a subtle but real artefact source flagged by the
// reference's pre-port audit. The last column/row forward difference is defined
// as 0 instead of wrapping to column/row 0.
//
// Why the adjoints must be exact: the variational solver uses conjugate
// gradient, whose search direction is only a true gradient if the operator
// applied in the objective and the operator applied in its transpose form a
// genuine adjoint pair, i.e. <Dx a, b> == <a, DxT b> for all a, b. The
// `gradients_test.cpp` unit test checks exactly this identity. The templates
// work for both `float` (luma) and `Complex` (chroma) planes.

#ifndef LDDECODE_HVD_ENGINE_GRADIENTS_H_
#define LDDECODE_HVD_ENGINE_GRADIENTS_H_

#include "engine/plane.h"

namespace hvd {

// d[:, :-1] = a[:, 1:] - a[:, :-1];  d[:, -1] = 0
template <typename T>
BasicPlane<T> Dx(const BasicPlane<T>& a) {
  BasicPlane<T> d(a.height(), a.width());
  const int h = a.height();
  const int w = a.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x + 1 < w; ++x) {
      d.at(y, x) = a.at(y, x + 1) - a.at(y, x);
    }
    if (w > 0) d.at(y, w - 1) = T{};
  }
  return d;
}

// d[:-1] = a[1:] - a[:-1];  d[-1] = 0
template <typename T>
BasicPlane<T> Dy(const BasicPlane<T>& a) {
  BasicPlane<T> d(a.height(), a.width());
  const int h = a.height();
  const int w = a.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h - 1; ++y) {
    for (int x = 0; x < w; ++x) {
      d.at(y, x) = a.at(y + 1, x) - a.at(y, x);
    }
  }
  // Last row already zero-initialised.
  return d;
}

// Adjoint of Dx (negative divergence with matching boundary):
//   d[:, 0]    = -a[:, 0]
//   d[:, 1:-1] =  a[:, :-2] - a[:, 1:-1]
//   d[:, -1]   =  a[:, -2]
template <typename T>
BasicPlane<T> DxT(const BasicPlane<T>& a) {
  BasicPlane<T> d(a.height(), a.width());
  const int w = a.width();
  const int h = a.height();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    // For w < 2, Dx is the zero operator, so its adjoint is zero too
    // (d is already zero-initialised).
    if (w < 2) continue;
    d.at(y, 0) = -a.at(y, 0);
    for (int x = 1; x < w - 1; ++x) d.at(y, x) = a.at(y, x - 1) - a.at(y, x);
    d.at(y, w - 1) = a.at(y, w - 2);
  }
  return d;
}

// Adjoint of Dy.
template <typename T>
BasicPlane<T> DyT(const BasicPlane<T>& a) {
  BasicPlane<T> d(a.height(), a.width());
  const int h = a.height();
  const int w = a.width();
  // For h < 2, Dy is the zero operator, so its adjoint is zero too.
  if (h < 2) return d;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int x = 0; x < w; ++x) {
    d.at(0, x) = -a.at(0, x);
    for (int y = 1; y < h - 1; ++y) d.at(y, x) = a.at(y - 1, x) - a.at(y, x);
    d.at(h - 1, x) = a.at(h - 2, x);
  }
  return d;
}

// ---------------------------------------------------------------------------
// Out-parameter variants: same maths, but write into a caller-owned buffer so
// hot loops (the CG solver) can pre-allocate once instead of allocating a
// frame-sized plane per call. `d` must already have a's dimensions.
//
// DyInto/DyTInto also iterate row-major (unlike the column-major loops above),
// which matches the row-major storage and keeps the cache happy.
// ---------------------------------------------------------------------------

template <typename T>
void DxInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int w = a.width();
  const int h = a.height();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x + 1 < w; ++x) d.at(y, x) = a.at(y, x + 1) - a.at(y, x);
    if (w > 0) d.at(y, w - 1) = T{};
  }
}

template <typename T>
void DyInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h - 1; ++y) {
    for (int x = 0; x < w; ++x) d.at(y, x) = a.at(y + 1, x) - a.at(y, x);
  }
  if (h > 0) {
    for (int x = 0; x < w; ++x) d.at(h - 1, x) = T{};
  }
}

template <typename T>
void DxTInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int w = a.width();
  const int h = a.height();
  const T* __restrict ap = a.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * w;
    if (w < 2) {
      for (int x = 0; x < w; ++x) dp[row + x] = T{};
      continue;
    }
    dp[row] = -ap[row];
    for (int x = 1; x < w - 1; ++x) dp[row + x] = ap[row + x - 1] - ap[row + x];
    dp[row + w - 1] = ap[row + w - 2];
  }
}

template <typename T>
void DyTInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
  const T* __restrict ap = a.data();
  T* __restrict dp = d.data();
  if (h < 2) {
    for (size_t i = 0; i < d.size(); ++i) dp[i] = T{};
    return;
  }
  for (int x = 0; x < w; ++x) dp[x] = -ap[x];
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 1; y < h - 1; ++y) {
    const int row = y * w;
    const int rowPrev = row - w;
    for (int x = 0; x < w; ++x) dp[row + x] = ap[rowPrev + x] - ap[row + x];
  }
  const int lastRow = (h - 1) * w;
  const int prevRow = lastRow - w;
  for (int x = 0; x < w; ++x) dp[lastRow + x] = ap[prevRow + x];
}

// ---------------------------------------------------------------------------
// Diagonal (+/-45 deg) forward differences with Neumann boundaries, for the
// oriented chroma priors (`--diag-prior`; THEORY 9e). Exact ports of the
// reference's `_d1/_d1T/_d2/_d2T`, adjoint-exact by construction (the
// reference verified <D1 a, b> == <a, D1T b> to 1e-14; gradients_test.cpp
// checks the same identity here). The adjoints are written in GATHER form —
// each output pixel reads its own contributors — so the OpenMP loops have no
// write conflicts, unlike a literal transcription of NumPy's scatter (-=/+=).
// ---------------------------------------------------------------------------

// +45 deg: d[:-1, :-1] = a[1:, 1:] - a[:-1, :-1]; last row & column zero.
template <typename T>
void D1Into(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    if (y + 1 < h) {
      for (int x = 0; x + 1 < w; ++x) d.at(y, x) = a.at(y + 1, x + 1) - a.at(y, x);
      if (w > 0) d.at(y, w - 1) = T{};
    } else {
      for (int x = 0; x < w; ++x) d.at(y, x) = T{};
    }
  }
}

// Adjoint of D1 (scatter form: d[:-1,:-1] -= a[:-1,:-1]; d[1:,1:] += a[:-1,:-1]):
//   d(y, x) = -a(y, x)       when y < h-1 and x < w-1
//           + a(y-1, x-1)    when y >= 1  and x >= 1
template <typename T>
void D1TInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
  const T* __restrict ap = a.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * w;
    const int rowPrev = row - w;
    for (int x = 0; x < w; ++x) {
      T v{};
      if (y + 1 < h && x + 1 < w) v = v - ap[row + x];
      if (y >= 1 && x >= 1) v = v + ap[rowPrev + x - 1];
      dp[row + x] = v;
    }
  }
}

// -45 deg: d[:-1, 1:] = a[1:, :-1] - a[:-1, 1:]; last row & first column zero.
template <typename T>
void D2Into(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    if (y + 1 < h) {
      if (w > 0) d.at(y, 0) = T{};
      for (int x = 1; x < w; ++x) d.at(y, x) = a.at(y + 1, x - 1) - a.at(y, x);
    } else {
      for (int x = 0; x < w; ++x) d.at(y, x) = T{};
    }
  }
}

// Adjoint of D2 (scatter form: d[:-1,1:] -= a[:-1,1:]; d[1:,:-1] += a[:-1,1:]):
//   d(y, x) = -a(y, x)       when y < h-1 and x >= 1
//           + a(y-1, x+1)    when y >= 1  and x < w-1
template <typename T>
void D2TInto(const BasicPlane<T>& a, BasicPlane<T>& d) {
  const int h = a.height();
  const int w = a.width();
  const T* __restrict ap = a.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * w;
    const int rowPrev = row - w;
    for (int x = 0; x < w; ++x) {
      T v{};
      if (y + 1 < h && x >= 1) v = v - ap[row + x];
      if (y >= 1 && x + 1 < w) v = v + ap[rowPrev + x + 1];
      dp[row + x] = v;
    }
  }
}

// ---------------------------------------------------------------------------
// Fused forward-difference + elementwise-weight variants: d = w .* D*(a) in
// a single pass, instead of D*Into(a, tmp) followed by a separate `tmp[i] *=
// w[i]` loop. Same maths, one fewer full-frame read+write of the
// intermediate buffer — this is the memory-traffic-bound hot path (CG
// gradient), so cutting a pass here is a real, not cosmetic, win.
//
// PRECONDITION: raw `__restrict` pointers via .data() — caller must guarantee
// `a`, `w`, and `d` don't alias each other (true at every call site below:
// `d` is always a scratch buffer distinct from `a`/`w`). No bounds checks;
// dimensions must already match (same contract as the *Into functions).
template <typename T>
void DxWeightedInto(const BasicPlane<T>& a, const BasicPlane<float>& w,
                     BasicPlane<T>& d) {
  const int h = a.height();
  const int wid = a.width();
  const T* __restrict ap = a.data();
  const float* __restrict wp = w.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * wid;
    for (int x = 0; x + 1 < wid; ++x)
      dp[row + x] = wp[row + x] * (ap[row + x + 1] - ap[row + x]);
    if (wid > 0) dp[row + wid - 1] = T{};
  }
}

template <typename T>
void DyWeightedInto(const BasicPlane<T>& a, const BasicPlane<float>& w,
                     BasicPlane<T>& d) {
  const int h = a.height();
  const int wid = a.width();
  const T* __restrict ap = a.data();
  const float* __restrict wp = w.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h - 1; ++y) {
    const int row = y * wid;
    const int rowNext = row + wid;
    for (int x = 0; x < wid; ++x)
      dp[row + x] = wp[row + x] * (ap[rowNext + x] - ap[row + x]);
  }
  if (h > 0) {
    const int lastRow = (h - 1) * wid;
    for (int x = 0; x < wid; ++x) dp[lastRow + x] = T{};
  }
}

template <typename T>
void D1WeightedInto(const BasicPlane<T>& a, const BasicPlane<float>& w,
                     BasicPlane<T>& d) {
  const int h = a.height();
  const int wid = a.width();
  const T* __restrict ap = a.data();
  const float* __restrict wp = w.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * wid;
    if (y + 1 < h) {
      const int rowNext = row + wid;
      for (int x = 0; x + 1 < wid; ++x)
        dp[row + x] = wp[row + x] * (ap[rowNext + x + 1] - ap[row + x]);
      if (wid > 0) dp[row + wid - 1] = T{};
    } else {
      for (int x = 0; x < wid; ++x) dp[row + x] = T{};
    }
  }
}

template <typename T>
void D2WeightedInto(const BasicPlane<T>& a, const BasicPlane<float>& w,
                     BasicPlane<T>& d) {
  const int h = a.height();
  const int wid = a.width();
  const T* __restrict ap = a.data();
  const float* __restrict wp = w.data();
  T* __restrict dp = d.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < h; ++y) {
    const int row = y * wid;
    if (y + 1 < h) {
      const int rowNext = row + wid;
      if (wid > 0) dp[row] = T{};
      for (int x = 1; x < wid; ++x)
        dp[row + x] = wp[row + x] * (ap[rowNext + x - 1] - ap[row + x]);
    } else {
      for (int x = 0; x < wid; ++x) dp[row + x] = T{};
    }
  }
}

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_GRADIENTS_H_
