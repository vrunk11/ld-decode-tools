// SPDX-License-Identifier: GPL-3.0-or-later
//
// PAL engine tests.
//
// 1. Swinging-burst joint phase + V-switch-parity estimation: synthesize a
//    PAL field whose burst is the standard swinging burst
//    (U_b, V_b) = (-B/sqrt2, +B/sqrt2) through the V-switched modulation
//    path, with a known theta trajectory (270 deg/line model + slow wobble
//    standing in for the 25 Hz offset drift) and a known parity, and check
//    both are recovered — including with a block of bursts destroyed.
//
// 2. The PAL hologram identity: with the recovered (theta, s) build the
//    effective carrier and verify  S == Y + Re[chi * c]  reconstructs a
//    synthetic active picture exactly, for chi = V - iU held global across
//    switched and unswitched lines. This is THE structural claim of the PAL
//    port (THEORY-PAL.md section 1): if it holds, every downstream engine
//    equation is standard-agnostic.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "check.h"
#include "engine/lockin.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace {

using hvd::Complex;
using hvd::FieldGeometry;
using hvd::Plane;
using hvd::VideoStandard;

constexpr float kHalfPi = 1.57079632679489661923F;
constexpr float kQuarterPi = 0.78539816339744830962F;
// 270.576 deg/line, NOT 270. PAL's fsc is 283.7516 x fH (EBU Tech. 3280-E
// Table 1, the 25 Hz offset), so on the stored 1135-sample grid the carrier
// advances 2*pi*0.7516 per line. This constant used to be 3*kHalfPi, which
// baked the very approximation the decoder was fixed for into the test that
// was supposed to catch it: the synthetic field was generated at 270 and
// line_advance() was then asserted to equal 270, so the two agreed with each
// other and with nothing real.
constexpr float kLineAdv = 2.0F * 3.14159265358979323846F * 0.7516F;

float WrapPi(float a) { return std::atan2(std::sin(a), std::cos(a)); }

// Synthesize a PAL field: swinging burst in the burst window, and a flat
// colour patch (Y, U, V) over the active picture, both through
//   S = Y + U sin(phi) + s V cos(phi).
Plane MakePalField(const FieldGeometry& g, const std::vector<float>& theta,
                   const std::vector<int8_t>& sw, float Y, float U, float V,
                   float burst_ire) {
  Plane f(static_cast<int>(theta.size()), g.field_width);
  const float Bs = burst_ire / std::sqrt(2.0F);
  for (int y = 0; y < f.height(); ++y) {
    const float s = sw[y] > 0 ? 1.0F : -1.0F;
    for (int x = 0; x < g.field_width; ++x) {
      const float phi = theta[y] + kHalfPi * static_cast<float>(x);
      float v = 0.0F;
      if (x >= g.colour_burst_start && x < g.colour_burst_end)
        v = -Bs * std::sin(phi) + s * Bs * std::cos(phi);
      if (x >= g.active_video_start && x < g.active_video_end)
        v = Y + U * std::sin(phi) + s * V * std::cos(phi);
      f.at(y, x) = v;
    }
  }
  return f;
}

}  // namespace

void RunTests() {
  FieldGeometry g;
  g.standard = VideoStandard::kPal;
  g.field_width = 300;
  g.colour_burst_start = 20;
  g.colour_burst_end = 60;
  g.active_video_start = 80;
  g.active_video_end = 280;
  g.first_active_field_line = 0;
  g.last_active_field_line = 0;

  const int h = 40;
  std::vector<float> theta_true(h);
  std::vector<int8_t> sw_true(h);
  for (int y = 0; y < h; ++y) {
    // 270 deg/line model + slow wobble (stands in for the 25 Hz drift).
    theta_true[y] = 0.4F + kLineAdv * static_cast<float>(y) +
                    0.05F * std::sin(0.15F * static_cast<float>(y));
    sw_true[y] = (y % 2 == 0) ? 1 : -1;
  }

  // --- 1a. clean joint estimation --------------------------------------
  Plane field = MakePalField(g, theta_true, sw_true, 50.0F, -12.0F, 20.0F,
                             21.43F);
  hvd::PalBurstLockin l = hvd::BurstLockinPhasePal(field, g);
  for (int y = 0; y < h; ++y) {
    CHECK(l.vswitch[y] == sw_true[y]);
    CHECK_NEAR(WrapPi(l.theta[y] - theta_true[y]), 0.0F, 0.03F);
  }

  // --- 1b. with a block of bursts destroyed -----------------------------
  Plane damaged = field;
  for (int y = 12; y < 20; ++y)
    for (int x = g.colour_burst_start; x < g.colour_burst_end; ++x)
      damaged.at(y, x) = 0.0F;
  hvd::PalBurstLockin ld = hvd::BurstLockinPhasePal(damaged, g);
  for (int y = 0; y < h; ++y) {
    CHECK(ld.vswitch[y] == sw_true[y]);  // parity is rigid alternation
    CHECK_NEAR(WrapPi(ld.theta[y] - theta_true[y]), 0.0F, 0.08F);
  }

  // --- 1c. a tape: the phase does not go from line to line as the standard says
  // The V-switch comes from the bursts of the neighbouring lines, not from a model of the phase over the whole field: with a drift of the phase
  // of 0.1 radian a line the field is still taken in its right parity, and so is one of the other parity (the votes of the lines are what say it)
  for (const int first_parity : {1, -1}) {
    std::vector<float> theta_tape(h);
    std::vector<int8_t> sw_tape(h);
    for (int y = 0; y < h; ++y) {
      theta_tape[y] = 0.4F + (kLineAdv + 0.1F) * static_cast<float>(y);
      sw_tape[y] = (y % 2 == 0) ? static_cast<int8_t>(first_parity) : static_cast<int8_t>(-first_parity);
    }
    const Plane tape = MakePalField(g, theta_tape, sw_tape, 50.0F, -12.0F, 20.0F, 21.43F);
    const hvd::PalBurstLockin lt = hvd::BurstLockinPhasePal(tape, g);
    int right = 0;
    for (int y = 3; y < h - 3; ++y) right += lt.vswitch[y] == sw_tape[y] ? 1 : 0;
    CHECK(right >= h - 6);
  }

  // --- 2. effective-carrier hologram identity ---------------------------
  // chi = V - iU, ONE phasor for the whole picture;
  // c = +exp(+i phi) on s=+1 lines, -exp(-i phi) on s=-1 lines.
  const float Y = 50.0F;
  const float U = -12.0F;
  const float V = 20.0F;
  const Complex chi(V, -U);
  // Ground-truth carrier: this is an IDENTITY test of MakeCarrierPal's
  // algebra, not a lock-in accuracy test (that's section 1; its 0.03 rad
  // tolerance would smear ~0.7 IRE through a 23 IRE chroma vector here).
  hvd::ComplexPlane c = hvd::MakeCarrierPal(theta_true, sw_true, g);
  float max_err = 0.0F;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < g.active_width(); ++x) {
      const float recon =
          Y + (chi * c.at(y, x)).real();
      const float s = field.at(y, g.active_video_start + x);
      max_err = std::max(max_err, std::abs(recon - s));
    }
  }
  CHECK(max_err < 1e-3F);

  // --- 3. line_advance / nominal burst dispatch -------------------------
  // Anchored on the DERIVATION, not on a constant copied from the header:
  // the fractional part of fsc / fH, times 2*pi. If someone reverts
  // line_advance() to a round 270 deg this fails, which is the whole point.
  {
    const double cycles_per_line = 4433618.75 / 15625.0;  // 283.7516
    const double frac = cycles_per_line - std::floor(cycles_per_line);
    const double want = 2.0 * 3.14159265358979323846 * frac;
    CHECK_NEAR(static_cast<float>(g.line_advance()),
               static_cast<float>(want), 1e-5F);
    // and it must NOT be the old 270 deg
    CHECK(std::abs(g.line_advance() - 1.5 * 3.14159265358979323846) > 1e-3);
  }
  CHECK_NEAR(static_cast<float>(g.line_advance()), kLineAdv, 1e-5F);
  CHECK_NEAR(g.nominal_burst_ire(), 21.43F, 1e-4F);
  FieldGeometry gn;  // default NTSC
  CHECK_NEAR(static_cast<float>(gn.line_advance()), 2.0F * kHalfPi, 1e-5F);
  CHECK_NEAR(gn.nominal_burst_ire(), 20.0F, 1e-4F);
}

TEST_MAIN()
