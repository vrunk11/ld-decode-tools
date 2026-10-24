// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression tests for the chroma phase reference and the level domain.
//
// These lock down four things that were previously wrong in ways no existing
// test could see, because the tests shared the decoder's own conventions:
//
//  1. ABSOLUTE burst phase on NTSC. Synthesize the burst the way a real
//     encoder does -- on the -U axis, i.e. -A sin(phi) -- and require the
//     recovered theta to match. The old code returned arg(z) + pi/2 instead
//     of arg(z) - pi/2, i.e. every NTSC decode was 180 deg out; the old
//     lock-in test synthesized +A sin(phi) so the two errors cancelled.
//
//  2. NTSC and PAL agree. Decode the same nominal colour through both paths
//     and require the recovered chi to have the SAME sign. The PAL lock-in
//     was already correct, so the global chroma_phase_deg = 180 that made
//     NTSC look right was silently rotating every PAL decode by half a turn.
//
//  3. chroma_phase_deg is uniform across the V-switch. MakeCarrierPal
//     conjugates the carrier on switched lines, so adding a constant offset
//     to theta rotates the recovered phasor in OPPOSITE directions on
//     alternate lines (Hanover bars) at every angle except 0 and 180.
//     ApplyChromaPhase signs the offset by parity; verify the net rotation
//     of chi is the same on both line parities at a non-trivial angle.
//
//  4. Level domain. On real NTSC-M levels (blanking 240, black 282, white
//     800) a nominal 20 IRE burst must measure 20.0 IRE and produce an ACC
//     gain of exactly 1.0. Referencing IRE to black instead of blanking made
//     it measure 21.62 and apply a permanent 7.5 % desaturation.
//
//  5. PAL non-orthogonal line offsets, so a flat frame_width stride can
//     never come back.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "check.h"
#include "engine/colour.h"
#include "engine/lockin.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"
#include "frame_bridge.h"

namespace {

using hvd::Complex;
using hvd::ComplexPlane;
using hvd::FieldGeometry;
using hvd::FrameParams;
using hvd::Plane;
using hvd::VideoStandard;

constexpr float kPi = 3.14159265358979323846F;
constexpr float kHalfPi = 1.57079632679489661923F;

float WrapPi(float a) { return std::atan2(std::sin(a), std::cos(a)); }

// One field of composite, built from the physical definitions:
//   S = Y + U sin(phi) + s V cos(phi)
// with the burst carried on the -U axis (NTSC) or swinging +/-45 deg about
// it through the same V-switch (PAL).
Plane MakeField(const FieldGeometry& g, const std::vector<float>& theta,
                const std::vector<int8_t>& sw, float Y, float U, float V,
                float burst_ire) {
  const bool pal = g.uses_vswitch();
  Plane f(static_cast<int>(theta.size()), g.field_width);
  const float bs = pal ? burst_ire / std::sqrt(2.0F) : burst_ire;
  for (int y = 0; y < f.height(); ++y) {
    const float s = (pal && sw[y] < 0) ? -1.0F : 1.0F;
    for (int x = 0; x < g.field_width; ++x) {
      const float phi = theta[y] + kHalfPi * static_cast<float>(x);
      float v = 0.0F;
      if (x >= g.colour_burst_start && x < g.colour_burst_end) {
        v = -bs * std::sin(phi) + (pal ? s * bs * std::cos(phi) : 0.0F);
      }
      if (x >= g.active_video_start && x < g.active_video_end) {
        v = Y + U * std::sin(phi) + s * V * std::cos(phi);
      }
      f.at(y, x) = v;
    }
  }
  return f;
}

// Mean recovered phasor over one line of the active picture, given the
// carrier the engine would build. chi = V - iU.
Complex RecoverChi(const Plane& field, const ComplexPlane& carrier,
                   const FieldGeometry& g, int row) {
  const int w = g.active_width();
  Complex acc{0.0F, 0.0F};
  float mean = 0.0F;
  for (int x = 0; x < w; ++x) mean += field.at(row, g.active_video_start + x);
  mean /= static_cast<float>(w);
  for (int x = 0; x < w; ++x) {
    const float s = field.at(row, g.active_video_start + x) - mean;
    acc += s * std::conj(carrier.at(row, x));
  }
  return acc * (2.0F / static_cast<float>(w));
}

FieldGeometry MakeGeometry(VideoStandard std_) {
  FieldGeometry g;
  g.standard = std_;
  g.field_width = 300;
  g.colour_burst_start = 20;
  g.colour_burst_end = 60;
  g.active_video_start = 80;
  g.active_video_end = 280;
  g.first_active_field_line = 0;
  g.last_active_field_line = 0;
  return g;
}

}  // namespace

void RunTests() {
  // ---------------------------------------------------------------------
  // 1. NTSC absolute burst phase
  // ---------------------------------------------------------------------
  {
    const FieldGeometry g = MakeGeometry(VideoStandard::kNtsc);
    const int h = 24;
    std::vector<float> theta_true(h);
    for (int y = 0; y < h; ++y) {
      theta_true[y] = 0.35F + kPi * static_cast<float>(y) +
                      0.04F * std::sin(0.2F * static_cast<float>(y));
    }
    const Plane f = MakeField(g, theta_true, {}, 50.0F, 0.0F, 0.0F, 20.0F);
    const std::vector<float> theta = hvd::BurstLockinPhase(f, g);
    for (int y = 0; y < h; ++y) {
      CHECK_NEAR(WrapPi(theta[y] - theta_true[y]), 0.0F, 0.03F);
    }
  }

  // ---------------------------------------------------------------------
  // 2. NTSC and PAL recover the same chi from the same nominal colour
  // ---------------------------------------------------------------------
  const float kU = -18.0F, kV = 25.0F;
  Complex chi_ntsc{}, chi_pal{};
  {
    const FieldGeometry g = MakeGeometry(VideoStandard::kNtsc);
    const int h = 16;
    std::vector<float> tt(h);
    for (int y = 0; y < h; ++y) tt[y] = 0.6F + kPi * static_cast<float>(y);
    const Plane f = MakeField(g, tt, {}, 50.0F, kU, kV, 20.0F);
    std::vector<float> theta = hvd::BurstLockinPhase(f, g);
    const ComplexPlane c = hvd::MakeCarrier(theta, g);
    chi_ntsc = RecoverChi(f, c, g, 8);
  }
  {
    const FieldGeometry g = MakeGeometry(VideoStandard::kPal);
    const int h = 16;
    std::vector<float> tt(h);
    std::vector<int8_t> sw(h);
    for (int y = 0; y < h; ++y) {
      tt[y] = 0.6F + 1.5F * kPi * static_cast<float>(y);
      sw[y] = (y % 2 == 0) ? 1 : -1;
    }
    const Plane f = MakeField(g, tt, sw, 50.0F, kU, kV, 21.43F);
    hvd::PalBurstLockin l = hvd::BurstLockinPhasePal(f, g);
    const ComplexPlane c = hvd::MakeCarrierPal(l.theta, l.vswitch, g);
    chi_pal = RecoverChi(f, c, g, 8);
  }
  // chi = V - iU, so V is the real part and U is minus the imaginary part.
  CHECK_NEAR(chi_ntsc.real(), kV, 0.6F);
  CHECK_NEAR(-chi_ntsc.imag(), kU, 0.6F);
  CHECK_NEAR(chi_pal.real(), kV, 0.6F);
  CHECK_NEAR(-chi_pal.imag(), kU, 0.6F);

  // ---------------------------------------------------------------------
  // 3. chroma_phase_deg rotates both PAL line parities the same way
  // ---------------------------------------------------------------------
  {
    const FieldGeometry g = MakeGeometry(VideoStandard::kPal);
    const int h = 16;
    std::vector<float> tt(h);
    std::vector<int8_t> sw(h);
    for (int y = 0; y < h; ++y) {
      tt[y] = 0.2F + 1.5F * kPi * static_cast<float>(y);
      sw[y] = (y % 2 == 0) ? 1 : -1;
    }
    const Plane f = MakeField(g, tt, sw, 50.0F, kU, kV, 21.43F);
    hvd::PalBurstLockin l = hvd::BurstLockinPhasePal(f, g);

    const ComplexPlane c0 = hvd::MakeCarrierPal(l.theta, l.vswitch, g);
    const Complex ref_even = RecoverChi(f, c0, g, 8);   // unswitched
    const Complex ref_odd = RecoverChi(f, c0, g, 9);    // switched

    std::vector<float> theta_rot = l.theta;
    const float kTrimDeg = 30.0F;
    hvd::ApplyChromaPhase(kTrimDeg, g, l.vswitch, &theta_rot);
    const ComplexPlane c1 = hvd::MakeCarrierPal(theta_rot, l.vswitch, g);
    const Complex rot_even = RecoverChi(f, c1, g, 8);
    const Complex rot_odd = RecoverChi(f, c1, g, 9);

    // Both parities must pick up the SAME rotation, and a positive
    // chroma_phase_deg must rotate the recovered hue positively (the offset
    // is negated on the reference precisely so that it does).
    const float d_even = WrapPi(std::arg(rot_even) - std::arg(ref_even));
    const float d_odd = WrapPi(std::arg(rot_odd) - std::arg(ref_odd));
    const float want = kTrimDeg * kPi / 180.0F;
    CHECK_NEAR(d_even, want, 0.02F);
    CHECK_NEAR(d_odd, want, 0.02F);
    CHECK_NEAR(WrapPi(d_even - d_odd), 0.0F, 0.02F);
  }

  // ---------------------------------------------------------------------
  // 4. Level domain: nominal burst on real NTSC-M levels -> ACC gain 1.0
  // ---------------------------------------------------------------------
  {
    const float blanking = 240.0F, black = 282.0F, white = 800.0F;
    FieldGeometry g = MakeGeometry(VideoStandard::kNtsc);
    const int h = 12;
    std::vector<float> tt(h);
    for (int y = 0; y < h; ++y) tt[y] = 0.1F + kPi * static_cast<float>(y);

    // Build the field in CODES, then convert exactly as the bridge does.
    Plane codes = MakeField(g, tt, {}, 50.0F, 0.0F, 0.0F, 20.0F);
    Plane ire(codes.height(), codes.width());
    const float cpi = hvd::CodesPerIre(blanking, white);
    for (int y = 0; y < codes.height(); ++y) {
      for (int x = 0; x < codes.width(); ++x) {
        ire.at(y, x) = hvd::SampleToIre(blanking + codes.at(y, x) * cpi,
                                        blanking, white);
      }
    }
    const float measured = hvd::BurstAmplitudeIre(ire, g);
    CHECK_NEAR(measured, 20.0F, 0.05F);
    CHECK_NEAR(hvd::AccGain(measured, g.nominal_burst_ire()), 1.0F, 0.005F);

    // And the domain itself: black sits at +7.5 IRE, white at 100 IRE.
    CHECK_NEAR(hvd::SampleToIre(black, blanking, white), 7.5F, 0.02F);
    CHECK_NEAR(hvd::SampleToIre(white, blanking, white), 100.0F, 0.02F);
    CHECK_NEAR(hvd::CodesPerIre(blanking, white), 5.6F, 0.001F);
  }

  // ---------------------------------------------------------------------
  // 5. PAL non-orthogonal line offsets
  // ---------------------------------------------------------------------
  {
    FrameParams fp;
    fp.standard = VideoStandard::kPal;
    fp.frame_width = 1135;
    fp.frame_height = 625;
    fp.field1_lines = 313;
    // EBU Tech. 3280-E: 2 extra samples each on frame-flat lines 312 and 624.
    const int32_t extras[4] = {312, 312, 624, 624};
    fp.extra_sample_line_count = 4;
    for (int i = 0; i < 4; ++i) fp.extra_sample_lines[i] = extras[i];

    CHECK(hvd::FrameLineOffset(fp, 0) == 0);
    CHECK(hvd::FrameLineOffset(fp, 312) == 312LL * 1135);
    // The whole of field 2 sits 2 samples later than a flat stride says.
    CHECK(hvd::FrameLineOffset(fp, 313) == 313LL * 1135 + 2);
    CHECK(hvd::FrameLineOffset(fp, 624) == 624LL * 1135 + 2);
    // 623 lines of 1135 plus two of 1137 == 709 379 == orc::kPalFrameSamples.
    CHECK(hvd::FrameLineOffset(fp, 625) == 709379LL);

    // NTSC / PAL-M / synthetic geometries stay on a flat stride.
    FrameParams n;
    n.standard = VideoStandard::kNtsc;
    n.frame_width = 910;
    CHECK(hvd::FrameLineOffset(n, 263) == 263LL * 910);
  }
}

TEST_MAIN()
