// SPDX-License-Identifier: GPL-3.0-or-later
//
// Colour conversion sanity: greys map to equal R=G=B, levels land where
// expected, and the ACC gain clamps to [0.5, 2.0].

#include "check.h"
#include "engine/colour.h"

void RunTests() {
  // Black (Y = black_ire, no chroma) -> RGB 0.
  auto black = hvd::YuvToRgb16(hvd::kIreBlack, 0.0F, 0.0F, hvd::kIreBlack);
  CHECK(black[0] == 0 && black[1] == 0 && black[2] == 0);

  // White (Y = 100 IRE, no chroma) -> RGB max, equal channels.
  auto white = hvd::YuvToRgb16(hvd::kIreWhite, 0.0F, 0.0F, hvd::kIreBlack);
  CHECK(white[0] == white[1] && white[1] == white[2]);
  CHECK(white[0] >= 65500);

  // Mid grey stays neutral (R == G == B).
  auto grey = hvd::YuvToRgb16(50.0F, 0.0F, 0.0F, hvd::kIreBlack);
  CHECK(grey[0] == grey[1] && grey[1] == grey[2]);

  // ACC gain clamping.
  CHECK_NEAR(hvd::AccGain(20.0F), 1.0, 1e-6);   // nominal burst -> unity
  CHECK_NEAR(hvd::AccGain(40.0F), 0.5, 1e-6);   // strong burst -> min clamp
  CHECK_NEAR(hvd::AccGain(1.0F), 2.0, 1e-6);    // weak burst -> max clamp
}

TEST_MAIN()
