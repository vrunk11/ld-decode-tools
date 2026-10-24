// SPDX-License-Identifier: GPL-3.0-or-later
//
// DetectFieldParity: the signal-measured field-order vote. Synthesizes an
// interlaced frame from a continuous scene (luma with vertical detail +
// modulated chroma at the 4fsc carrier + noise), splits it into the two
// field blocks in BOTH storage orders, and checks the vote lands right —
// including that the horizontal low-pass keeps the chroma carrier from
// masquerading as vertical structure, and that flat content abstains.

#include <cmath>
#include <random>

#include "check.h"
#include "frame_bridge.h"

namespace {

using hvd::FieldInput;
using hvd::FrameParams;
using hvd::Plane;

constexpr float kPi = 3.14159265358979323846F;

FrameParams MakeParams(int fw, int field_lines) {
  FrameParams fp;
  fp.frame_width = fw;
  fp.frame_height = 2 * field_lines;
  fp.field1_lines = field_lines;
  fp.active_video_start = 8;
  fp.active_video_end = fw - 8;
  fp.first_active_frame_line = 12;
  fp.black_level = 128.0F;
  fp.white_level = 896.0F;
  fp.blanking_level = 128.0F;
  return fp;
}

// Continuous scene sampled at frame line fy: luma vertical detail plus a
// chroma-like component at the carrier frequency.
float SceneAt(float fy, int x, bool flat) {
  if (flat) return 40.0F;
  return 40.0F + 18.0F * std::sin(0.45F * fy) +
         6.0F * std::cos(0.07F * x + 0.2F * fy) +
         10.0F * std::cos((kPi / 2.0F) * x) * std::sin(0.3F * fy);
}

// Build the two field blocks; `f1_bottom` chooses which spatial half goes
// into block 1 (the storage order under test).
void MakeBlocks(const FrameParams& fp, bool f1_bottom, bool flat,
                unsigned seed, FieldInput* b1, FieldInput* b2) {
  const int L = fp.field1_lines;
  std::mt19937 rng(seed);
  std::normal_distribution<float> nz(0.0F, 0.8F);
  b1->samples = Plane(L, fp.frame_width);
  b2->samples = Plane(L, fp.frame_width);
  for (int r = 0; r < L; ++r) {
    for (int x = 0; x < fp.frame_width; ++x) {
      const float top_v = SceneAt(2.0F * r, x, flat) + nz(rng);
      const float bot_v = SceneAt(2.0F * r + 1.0F, x, flat) + nz(rng);
      b1->samples.at(r, x) = f1_bottom ? bot_v : top_v;
      b2->samples.at(r, x) = f1_bottom ? top_v : bot_v;
    }
  }
}

}  // namespace

void RunTests() {
  const FrameParams fp = MakeParams(256, 96);

  FieldInput b1, b2;
  MakeBlocks(fp, /*f1_bottom=*/false, /*flat=*/false, 3, &b1, &b2);
  CHECK(hvd::DetectFieldParity(b1, b2, fp) == +1);

  MakeBlocks(fp, /*f1_bottom=*/true, /*flat=*/false, 4, &b1, &b2);
  CHECK(hvd::DetectFieldParity(b1, b2, fp) == -1);

  // Flat content: no vertical detail to vote with -> abstain (0), and the
  // resolver then falls back to the format convention (field 1 = top).
  MakeBlocks(fp, /*f1_bottom=*/false, /*flat=*/true, 5, &b1, &b2);
  CHECK(hvd::DetectFieldParity(b1, b2, fp) == 0);
  {
    hvd::HvdConfig cfg;
    cfg.field_order = 0;
    CHECK(hvd::ResolveField1IsBottom(b1, b2, fp, cfg) == false);
    cfg.field_order = 2;
    CHECK(hvd::ResolveField1IsBottom(b1, b2, fp, cfg) == true);
  }
}

TEST_MAIN()
