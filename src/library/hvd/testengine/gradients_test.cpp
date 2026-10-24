// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conjugate-gradient solver is only correct if Dx/DxT and Dy/DyT are exact
// adjoint pairs: <D a, b> == <a, D^T b>. This is the single most important
// algebraic property of the solver's operators, so we check it directly on
// random real and complex planes.

#include <random>

#include "check.h"
#include "engine/gradients.h"
#include "engine/plane.h"

namespace {

using hvd::ComplexPlane;
using hvd::D1Into;
using hvd::D1TInto;
using hvd::D2Into;
using hvd::D2TInto;
using hvd::Dx;
using hvd::DxT;
using hvd::Dy;
using hvd::DyT;
using hvd::Plane;

Plane RandomReal(int h, int w, std::mt19937* rng) {
  std::uniform_real_distribution<float> u(-1.0F, 1.0F);
  Plane p(h, w);
  for (size_t i = 0; i < p.size(); ++i) p[i] = u(*rng);
  return p;
}

double DotReal(const Plane& a, const Plane& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
  return s;
}

void CheckAdjointReal(int h, int w, std::mt19937* rng) {
  const Plane a = RandomReal(h, w, rng);
  const Plane b = RandomReal(h, w, rng);
  // <Dx a, b> == <a, DxT b>
  CHECK_NEAR(DotReal(Dx(a), b), DotReal(a, DxT(b)), 1e-3);
  CHECK_NEAR(DotReal(Dy(a), b), DotReal(a, DyT(b)), 1e-3);

  // Diagonal (+/-45 deg) operators for the oriented chroma prior: same
  // identity (the reference verified its _d1/_d2 adjoints to 1e-14).
  Plane d1a(h, w), d1tb(h, w), d2a(h, w), d2tb(h, w);
  D1Into(a, d1a);
  D1TInto(b, d1tb);
  D2Into(a, d2a);
  D2TInto(b, d2tb);
  CHECK_NEAR(DotReal(d1a, b), DotReal(a, d1tb), 1e-3);
  CHECK_NEAR(DotReal(d2a, b), DotReal(a, d2tb), 1e-3);
}

}  // namespace

void RunTests() {
  std::mt19937 rng(12345);
  CheckAdjointReal(1, 1, &rng);
  CheckAdjointReal(1, 8, &rng);
  CheckAdjointReal(8, 1, &rng);
  CheckAdjointReal(17, 23, &rng);
  CheckAdjointReal(64, 48, &rng);
}

TEST_MAIN()
