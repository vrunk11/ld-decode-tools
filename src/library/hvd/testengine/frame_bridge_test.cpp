// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end bridge check on the real field-sequential CVBS layout: build a
// synthetic frame (field 1 lines, then field 2 lines) from a known composite,
// run DecodeFrameBuffer, and verify the Y/C split reconstructs the composite
// via luma + (chroma - chroma_dc) within a couple of codes (10-bit
// requantisation of two channels). Also checks output shape.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "check.h"
#include "engine/ntsc_geometry.h"
#include "frame_bridge.h"

namespace {

using hvd::FrameParams;
using hvd::HvdConfig;

constexpr float kHalfPi = 1.57079632679489661923F;
constexpr float kPi = 3.14159265358979323846F;

}  // namespace

void RunTests() {
  FrameParams fp;
  fp.frame_width = 260;
  fp.frame_height = 48;   // small frame
  fp.field1_lines = 24;   // field 1 = flat lines [0,24); field 2 = [24,48)
  fp.active_video_start = 20;
  fp.active_video_end = 240;
  fp.colour_burst_start = 4;
  fp.colour_burst_end = 16;
  fp.first_active_frame_line = 8;   // -> field line 4
  fp.last_active_frame_line = 44;   // -> field line 22
  fp.black_level = 240.0F;
  fp.white_level = 800.0F;
  fp.blanking_level = 240.0F;
  fp.chroma_dc = 240.0F;
  fp.sample_rate = hvd::kFs4Fsc;

  const int fw = fp.frame_width;
  const int fh = fp.frame_height;
  const float scale = (fp.white_level - fp.blanking_level) / 100.0F;

  // Synthesize a field-sequential composite: burst in every line, plus a luma
  // ramp and modulated chroma in the active picture of each field.
  std::vector<int16_t> frame(static_cast<size_t>(fw) * fh, 0);
  const int fal_field = fp.first_active_frame_line / 2;
  const int lal_field = fp.last_active_frame_line / 2;
  for (int line = 0; line < fh; ++line) {
    const bool is_f1 = line < fp.field1_lines;
    const int field_line = is_f1 ? line : (line - fp.field1_lines);
    for (int x = 0; x < fw; ++x) {
      float ire = 0.0F;
      if (x >= fp.colour_burst_start && x < fp.colour_burst_end) {
        // -U axis burst: -A sin(phi). See lockin.cpp.
        ire = -20.0F * std::sin(kHalfPi * x + kPi * field_line);
      }
      if (x >= fp.active_video_start && x < fp.active_video_end &&
          field_line >= fal_field && field_line < lal_field) {
        const float luma =
            30.0F + 30.0F * static_cast<float>(x - fp.active_video_start) /
                        (fp.active_video_end - fp.active_video_start);
        const float chroma =
            (x > fw / 2) ? 12.0F * std::cos(kHalfPi * x + kPi * field_line)
                         : 0.0F;
        ire = luma + chroma;
      }
      const float sample = fp.blanking_level + ire * scale;
      frame[static_cast<size_t>(line) * fw + x] =
          static_cast<int16_t>(std::lround(std::clamp(sample, 0.0F, 1023.0F)));
    }
  }

  HvdConfig cfg;
  cfg.cg_iterations = 20;  // keep the test fast
  const hvd::YcFrameS16 yc = hvd::DecodeFrameBuffer(frame.data(), fp, cfg);

  CHECK(yc.width == fw);
  CHECK(yc.height == fh);
  CHECK(yc.luma.size() == frame.size());
  CHECK(yc.chroma.size() == frame.size());

  // Chroma is zero-centred (signed), matching what the host's NTSC decoder
  // demodulates: composite == luma + chroma, with no DC term to remove.
  int max_err = 0;
  for (size_t i = 0; i < frame.size(); ++i) {
    const int recon =
        static_cast<int>(yc.luma[i]) + static_cast<int>(yc.chroma[i]);
    max_err = std::max(max_err, std::abs(recon - static_cast<int>(frame[i])));
  }
  CHECK(max_err <= 2);
}

TEST_MAIN()
