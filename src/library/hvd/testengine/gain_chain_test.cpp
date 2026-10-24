// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end GAIN audit: composite codes -> IRE -> chi -> u/v codes ->
// preview normalisation, for every standard the stage accepts.
//
// Phase has its own test (chroma_reference_test.cpp). This one exists
// because the gain path has three independent places to get a scale wrong,
// and each of them was wrong at some point in a way the other tests could
// not see:
//
//   1. The IRE domain. Referencing IRE to black instead of blanking made
//      one internal unit 5.18 codes instead of 5.60 on NTSC-M, so a
//      nominal 20 IRE burst measured 21.62 and the ACC applied a permanent
//      0.925. Invisible on PAL (black == blanking) and invisible in any
//      round trip, because the same wrong scale undoes itself.
//
//   2. The ACC nominal. DecodeFrameSequenceWindow -- the DEFAULT composite
//      path -- compared the measured burst against a hardcoded 20.0
//      instead of g.nominal_burst_ire(). On 625-line PAL (nominal 21.43)
//      that is a permanent 6.7 % desaturation, and it disagreed with the
//      frame path, which used the standard's own nominal all along.
//
//   3. The output scale. orc::ComponentFrame specifies chroma "with the
//      same scaling as in the original composite signal", and
//      orc::ColourFrameCarrier specifies U/V normalised over
//      (cvbs_white - cvbs_blanking) while Y is normalised over
//      (cvbs_white - cvbs_black). Using one denominator for both made
//      HVD's own preview and the host's disagree by 8.1 % on NTSC-M.
//
// The invariant, end to end: feed in a colour of known TRUE IRE amplitude
// on a nominal burst, and the u/v planes must come back at exactly that
// amplitude on the composite code scale, with ACC == 1.

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "check.h"
#include "engine/colour.h"
#include "engine/lockin.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace {

using hvd::Complex;
using hvd::ComplexPlane;
using hvd::FieldGeometry;
using hvd::Plane;
using hvd::VideoStandard;

constexpr float kPi = 3.14159265358979323846F;

struct Sys {
  const char* name;
  VideoStandard standard;
  float blanking;
  float black;
  float white;
  float nominal_burst;  // TRUE IRE
};

// The four systems the stage accepts, with their normative levels from
// orc/stage/cvbs_signal_constants.h.
const Sys kSystems[] = {
    {"NTSC-M", VideoStandard::kNtsc, 240.0F, 282.0F, 800.0F, 20.00F},
    {"NTSC-J", VideoStandard::kNtsc, 240.0F, 240.0F, 800.0F, 20.00F},
    {"PAL", VideoStandard::kPal, 256.0F, 256.0F, 844.0F, 21.43F},
    {"PAL-M", VideoStandard::kPalM, 240.0F, 282.0F, 800.0F, 20.00F},
};

}  // namespace

void RunDilutionTest();  // forward decl; defined after RunTests() below

void RunTests() {
  const float kY = 50.0F, kU = -18.0F, kV = 25.0F;  // TRUE IRE

  for (const Sys& s : kSystems) {
    FieldGeometry g;
    g.standard = s.standard;
    g.field_width = 400;
    g.colour_burst_start = 40;
    g.colour_burst_end = 80;
    g.active_video_start = 100;
    g.active_video_end = 380;
    g.first_active_field_line = 0;
    g.last_active_field_line = 0;

    // The standard's own nominal must be what the ACC targets.
    CHECK_NEAR(g.nominal_burst_ire(), s.nominal_burst, 0.001F);

    const int h = 20;
    const float cpi = hvd::CodesPerIre(s.blanking, s.white);
    const bool pal = g.uses_vswitch();
    const float bs = pal ? s.nominal_burst / std::sqrt(2.0F) : s.nominal_burst;

    // --- synthesize the composite in 10-bit CODES, as a capture stores it
    Plane codes(h, g.field_width);
    for (int y = 0; y < h; ++y) {
      const float theta = 0.4F + static_cast<float>(g.line_advance()) * y;
      const float sw = (pal && (y % 2 != 0)) ? -1.0F : 1.0F;
      for (int x = 0; x < g.field_width; ++x) {
        const float phi = theta + 0.5F * kPi * static_cast<float>(x);
        float ire = 0.0F;  // blanking
        if (x >= g.colour_burst_start && x < g.colour_burst_end) {
          ire = -bs * std::sin(phi) + (pal ? sw * bs * std::cos(phi) : 0.0F);
        }
        if (x >= g.active_video_start && x < g.active_video_end) {
          ire = kY + kU * std::sin(phi) + sw * kV * std::cos(phi);
        }
        codes.at(y, x) = s.blanking + ire * cpi;
      }
    }

    // --- bridge: codes -> TRUE IRE (blanking-referenced)
    Plane ire(h, g.field_width);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < g.field_width; ++x) {
        ire.at(y, x) = hvd::SampleToIre(codes.at(y, x), s.blanking, s.white);
      }
    }

    // --- (1) the IRE domain itself
    CHECK_NEAR(hvd::SampleToIre(s.blanking, s.blanking, s.white), 0.0F, 0.001F);
    CHECK_NEAR(hvd::SampleToIre(s.white, s.blanking, s.white), 100.0F, 0.001F);

    // --- (2) ACC: a nominal burst must give unity, on every standard
    const float measured = hvd::BurstAmplitudeIre(ire, g);
    const float acc = hvd::AccGain(measured, g.nominal_burst_ire());
    CHECK_NEAR(measured, s.nominal_burst, 0.05F);
    CHECK_NEAR(acc, 1.0F, 0.005F);

    // --- demodulate one active line through the real carrier builder
    std::vector<float> theta;
    std::vector<int8_t> vswitch;
    ComplexPlane carrier;
    if (pal) {
      hvd::PalBurstLockin l = hvd::BurstLockinPhasePal(ire, g);
      theta = l.theta;
      vswitch = l.vswitch;
      carrier = hvd::MakeCarrierPal(theta, vswitch, g);
    } else {
      theta = hvd::BurstLockinPhase(ire, g);
      carrier = hvd::MakeCarrier(theta, g);
    }

    const int row = 8;
    const int w = g.active_width();
    Complex acc_sum{0.0F, 0.0F};
    float mean = 0.0F;
    for (int x = 0; x < w; ++x) {
      mean += ire.at(row, g.active_video_start + x);
    }
    mean /= static_cast<float>(w);
    for (int x = 0; x < w; ++x) {
      acc_sum += (ire.at(row, g.active_video_start + x) - mean) *
                 std::conj(carrier.at(row, x));
    }
    const Complex chi = acc_sum * (2.0F / static_cast<float>(w));

    // --- (3) writeback, exactly as frame_bridge.cpp computes it
    const float scale = hvd::CodesPerIre(s.blanking, s.white);
    const float gain = 1.0F * acc;  // chroma_gain * acc_gain
    const double u_code = gain * static_cast<double>(-chi.imag()) * scale;
    const double v_code = gain * static_cast<double>(chi.real()) * scale;

    // u/v must land on the ORC ComponentFrame composite scale.
    CHECK_NEAR(u_code, kU * cpi, 0.02 * std::fabs(kU * cpi));
    CHECK_NEAR(v_code, kV * cpi, 0.02 * std::fabs(kV * cpi));

    // --- preview normalisation, exactly as the stage computes it.
    //
    // Poynton's U/V are defined against Y' in [0, 1], so BOTH axes
    // normalise over the luma excursion (white - black). On NTSC-M and
    // PAL-M that is 92.5 IRE because of the setup pedestal; on NTSC-J and
    // 625-line PAL it is 100. Verified against three normative facts:
    // 100 % bars peak at 130.83 IRE, 75 % bars peak at EXACTLY 100.000,
    // and yellow and cyan share a peak. Dividing chroma by
    // (white - blanking) instead -- which orc::outputwriter.cpp does --
    // fails all three and under-saturates NTSC-M by 8.1 %.
    const double range = s.white - s.black;
    const double setup_ire = (s.black - s.blanking) / cpi;   // 7.5 or 0
    const double excursion_ire = 100.0 - setup_ire;          // 92.5 or 100
    CHECK_NEAR(u_code / range, kU / excursion_ire, 0.002);
    CHECK_NEAR(v_code / range, kV / excursion_ire, 0.002);
    // Picture black must normalise to 0 and white to 1 on the luma axis.
    CHECK_NEAR((s.black - s.black) / range, 0.0, 1e-9);
    CHECK_NEAR((s.white - s.black) / range, 1.0, 1e-9);

    std::printf("  %-7s codes/IRE %6.3f  burst %7.4f  ACC %.4f  "
                "u %8.2f (want %8.2f)\n",
                s.name, cpi, measured, acc, u_code, kU * cpi);
  }

  // The clamp is the only thing that may bend the gain, and only outside
  // a 2x window around nominal.
  CHECK_NEAR(hvd::AccGain(20.0F, 20.0F), 1.0F, 1e-6F);
  CHECK_NEAR(hvd::AccGain(21.43F, 21.43F), 1.0F, 1e-6F);
  CHECK_NEAR(hvd::AccGain(10.0F, 20.0F), 2.0F, 1e-6F);   // max clamp
  CHECK_NEAR(hvd::AccGain(60.0F, 20.0F), 0.5F, 1e-6F);   // min clamp
  // A PAL burst measured against the NTSC nominal is the bug this guards:
  // it would silently desaturate by 6.7 %.
  CHECK(std::fabs(hvd::AccGain(21.43F, 20.0F) - 1.0F) > 0.05F);

  RunDilutionTest();
}

// ---------------------------------------------------------------------
// BurstAmplitudeIre must not DILUTE when the burst doesn't fill the whole
// measurement window. SMPTE 170M-2004 Table 2 permits 9 +/- 1 cycles on a
// window sized for the nominal 9; a coherent mean over the full window
// reports A * (cycles present) / (window cycles) instead of A, which is
// exactly the "gain feels too strong on NTSC, I have to dial ~0.8" bug
// report this guards against.
void RunDilutionTest() {
  FieldGeometry g;
  g.standard = VideoStandard::kNtsc;
  g.field_width = 910;
  g.colour_burst_start = 72;
  g.colour_burst_end = 108;  // 36 samples = 9 cycles, the real ORC window
  g.active_video_start = 126;
  g.active_video_end = 894;
  g.first_active_field_line = 0;
  g.last_active_field_line = 0;

  struct Case { int cycles; int offset; };
  // Every one of these is a LEGAL burst (SMPTE 170M: 9 +/- 1 cycles) or a
  // plausible sample-0 misalignment; every one must still measure 20 IRE.
  const Case cases[] = {{9, 0}, {8, 0}, {8, 2}, {7, 0},
                        {9, 4}, {9, -4}, {10, 0}, {9, 6}};
  const int h = 24;
  for (const Case& c : cases) {
    Plane f(h, g.field_width);
    const int b0 = g.colour_burst_start + c.offset;
    const int b1 = b0 + 4 * c.cycles;
    for (int y = 0; y < h; ++y) {
      const float theta = 0.4F + kPi * static_cast<float>(y);
      for (int x = 0; x < g.field_width; ++x) {
        float v = 0.0F;  // blanking
        if (x >= b0 && x < b1) {
          v = -20.0F * std::sin(theta + 0.5F * kPi * static_cast<float>(x));
        }
        f.at(y, x) = v;
      }
    }
    const float measured = hvd::BurstAmplitudeIre(f, g);
    CHECK_NEAR(measured, 20.0F, 0.05F);
  }
}

TEST_MAIN()
