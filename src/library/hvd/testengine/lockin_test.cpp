// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lock-in phase recovery: synthesize a field whose only content is a colour
// burst -A*sin(phi) (the real -U axis burst) with a known per-line phase
// offset theta, then check the recovered theta matches. Also exercise
// TridiagSmooth on a trivial system.

#include <cmath>
#include <vector>

#include "check.h"
#include "engine/lockin.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace {

using hvd::FieldGeometry;
using hvd::Plane;

constexpr float kHalfPi = 1.57079632679489661923F;
constexpr float kPi = 3.14159265358979323846F;

float WrapPi(float a) { return std::atan2(std::sin(a), std::cos(a)); }

}  // namespace

void RunTests() {
  FieldGeometry g;
  g.field_width = 200;
  g.colour_burst_start = 20;
  g.colour_burst_end = 60;

  const int h = 30;
  Plane field(h, g.field_width);

  // burst = -A sin(theta_line + (pi/2) x) -- the NTSC burst sits on the -U
  // axis, so chi_burst = +iA and Re[iA e^{i phi}] = -A sin(phi). The test
  // used +A sin here, which is the same sign error the decoder had, so the
  // two cancelled and the test passed on a 180 deg-wrong decoder.
  // Use the pi-per-line model so the
  // smoother does not fight the ground truth, plus a tiny slow wobble.
  std::vector<float> theta_true(h);
  for (int y = 0; y < h; ++y) {
    theta_true[y] = 0.3F + kPi * static_cast<float>(y) + 0.05F * std::sin(0.2F * y);
    for (int x = g.colour_burst_start; x < g.colour_burst_end; ++x) {
      const float phi = theta_true[y] + kHalfPi * static_cast<float>(x);
      field.at(y, x) = -20.0F * std::sin(phi);
    }
  }

  const std::vector<float> theta = hvd::BurstLockinPhase(field, g);
  CHECK(static_cast<int>(theta.size()) == h);
  // Recovered phase should match the truth modulo 2*pi on burst-bearing lines.
  for (int y = 2; y < h - 2; ++y) {
    CHECK_NEAR(std::fabs(WrapPi(theta[y] - theta_true[y])), 0.0, 0.05);
  }

  // Burst amplitude ~ 20 IRE (A = 20).
  CHECK_NEAR(hvd::BurstAmplitudeIre(field, g), 20.0, 1.0);

  // TridiagSmooth: with all weights 1 and lam -> 0 it returns the data; with
  // lam huge it returns the (weighted) mean. Check the low-lambda limit.
  const std::vector<float> d = {1.0F, 2.0F, 3.0F, 4.0F};
  const std::vector<float> a = {1.0F, 1.0F, 1.0F, 1.0F};
  const std::vector<float> x = hvd::TridiagSmooth(d, a, 1e-6F);
  for (size_t i = 0; i < d.size(); ++i) CHECK_NEAR(x[i], d[i], 1e-3);
}

TEST_MAIN()
