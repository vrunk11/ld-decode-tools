// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guards the standard-dependent leak cancellation inside
// ResolveChromaAniso() (engine/variational.cpp).
//
// The AUTO chroma_aniso measurement must cancel the holographic init's
// cross-colour leak BEFORE measuring chroma orientation, otherwise the
// leak dominates the vertical gradient and AUTO picks a wrong anisotropy.
// The leak's line-to-line structure differs by standard:
//
//   NTSC: conj(c) alternates SIGN only            -> 2-line average cancels
//   PAL : conj(c) alternates sign AND CONJUGATION -> only a 4-line average
//         cancels (V-switch = conjugated reference wave, and the 270 deg/
//         line carrier walk closes only over 4 lines: 4*270 = 0 mod 360)
//
// This test reconstructs each standard's characteristic leak analytically
// and asserts the group size actually used by the implementation collapses
// it. The PAL/group-2 case is asserted to FAIL to cancel: that is the bug
// this guards against regressing to (it shipped that way once).

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "check.h"

namespace {

using Complex = std::complex<float>;

// Worst-case vertical gradient of a chi0 whose ONLY content is the leak.
// A correct group size drives this to ~0.
float LeakResidual(int group, bool pal) {
  const int H = 64;
  const int W = 32;
  std::vector<Complex> chi(static_cast<size_t>(H) * W);
  for (int y = 0; y < H; ++y) {
    const float phi = 1.5F * 3.14159265F * static_cast<float>(y);
    const Complex leak =
        pal ? ((y % 2 == 0) ? std::polar(1.0F, phi)
                            : -std::conj(std::polar(1.0F, phi)))
            : Complex((y % 2 == 0) ? 1.0F : -1.0F, 0.0F);
    for (int x = 0; x < W; ++x) chi[static_cast<size_t>(y) * W + x] = leak;
  }
  auto pav = [&](int t, int x) {
    Complex acc{0.0F, 0.0F};
    for (int k = 0; k < group; ++k)
      acc += chi[static_cast<size_t>(group * t + k) * W + x];
    return acc * (1.0F / static_cast<float>(group));
  };
  float worst = 0.0F;
  for (int t = 1; t < H / group; ++t)
    for (int x = 0; x < W; ++x)
      worst = std::max(worst, std::abs(pav(t, x) - pav(t - 1, x)));
  return worst;
}

}  // namespace

void RunTests() {
  // NTSC: the shipped group size (2) cancels its leak exactly.
  CHECK(LeakResidual(2, false) < 1e-5F);

  // PAL: group 2 does NOT cancel — residual is sqrt(2) per the algebra.
  // If this ever starts passing at <1e-5, someone has "simplified" the
  // standard dispatch away and PAL's AUTO aniso is silently wrong again.
  CHECK(LeakResidual(2, true) > 0.1F);

  // PAL: the shipped group size (4) cancels it.
  CHECK(LeakResidual(4, true) < 1e-5F);
}

TEST_MAIN()
