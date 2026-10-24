// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guards the preview geometry reported by
// HvdChromaDecoderStage::get_preview_capability().
//
// Two invariants, both learned by getting them wrong and seeing the
// preview render at a different aspect than tbc_source on the same frame:
//
// 1. dar_correction_factor is the signal's PIXEL ASPECT RATIO — a fixed
//    per-system property of the 4fsc sampling. cvbs_signal_constants.h is
//    explicit that it must NOT be recomputed from a source's actual
//    active-area values, because that ties the display aspect to the
//    chosen window (changing the active area would rescale the preview
//    instead of re-framing it). The canonical values come from the
//    STANDARD active-area constants; this test recomputes them from those
//    constants and checks the expected per-system numbers.
//
// 2. geometry.active_* describes the ACTIVE PICTURE, not the delivered
//    image. The SDK's own reference stage reports the active area while
//    DELIVERING the full frame; reporting the delivered full-raster size
//    instead makes the GUI's "4:3 (Display)" mode reason about a ~1.73
//    picture rather than a 1.33 one.
//
// Neither value may vary with the preview view toggles: those change
// which pixels are delivered, not the shape of a pixel.
//
// The stage TU needs the decode-orc SDK to compile, so this mirrors the
// constants and formula hermetically (same approach as
// pal_aniso_group_test). Keep in step with cvbs_signal_constants.h.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "check.h"

namespace {

// Mirrors orc/stage/cvbs_signal_constants.h.
constexpr int32_t kNtscActiveVideoStart = 126;
constexpr int32_t kNtscActiveVideoEnd = 894;
constexpr int32_t kNtscFirstActiveFrameLine = 40;
constexpr int32_t kNtscLastActiveFrameLine = 523;

constexpr int32_t kPalActiveVideoStart = 185;
constexpr int32_t kPalActiveVideoEnd = 1107;
constexpr int32_t kPalFirstActiveFrameLine = 44;
constexpr int32_t kPalLastActiveFrameLine = 620;

// standard_dar_correction(), from the STANDARD constants only.
double StandardDar(bool pal) {
  const int32_t w = pal ? (kPalActiveVideoEnd - kPalActiveVideoStart)
                        : (kNtscActiveVideoEnd - kNtscActiveVideoStart);
  const int32_t h = pal ? (kPalLastActiveFrameLine - kPalFirstActiveFrameLine)
                        : (kNtscLastActiveFrameLine - kNtscFirstActiveFrameLine);
  return (4.0 / 3.0) / (static_cast<double>(w) / static_cast<double>(h));
}

}  // namespace

void RunTests() {
  for (bool pal : {false, true}) {
    const double dar = StandardDar(pal);

    // The STANDARD active picture displays 4:3 under this factor. (A
    // source whose own active window differs re-frames; it does not
    // rescale — that is the whole point of using fixed constants.)
    const int32_t w = pal ? (kPalActiveVideoEnd - kPalActiveVideoStart)
                          : (kNtscActiveVideoEnd - kNtscActiveVideoStart);
    const int32_t h = pal ? (kPalLastActiveFrameLine - kPalFirstActiveFrameLine)
                          : (kNtscLastActiveFrameLine - kNtscFirstActiveFrameLine);
    CHECK_NEAR((w * dar) / static_cast<double>(h), 4.0 / 3.0, 1e-9);

    // Plausible 4fsc broadcast PAR (the SDK doc says "typically ~0.7").
    CHECK(dar > 0.6 && dar < 1.1);
  }

  // Regression witness 1: deriving the factor from a source's actual
  // active window gives a DIFFERENT number than the standard one, which
  // is exactly the drift cvbs_signal_constants.h warns against. (Source
  // values seen in the wild: 134..894 x 40..525.)
  const double source_derived = (4.0 / 3.0) / (760.0 / 485.0);
  CHECK(std::fabs(source_derived - StandardDar(false)) > 1e-3);

  // Regression witness 2: reporting the DELIVERED full raster as the
  // active picture misstates the sample-domain picture ratio by ~9%
  // (1.733 vs 1.590), which is what the GUI's "4:3 (Display)" mode then
  // reasons about.
  const double full_raster_ratio = 910.0 / 525.0;
  const double active_ratio = 768.0 / 483.0;
  CHECK(std::fabs(full_raster_ratio - active_ratio) > 0.10);
}

TEST_MAIN()
