// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/colour.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hvd {

namespace {
// Standard BT.601-ish YUV -> RGB matrix used by the reference (YUV_TO_RGB).
constexpr float kYuvToRgb[3][3] = {
    {1.0F, 0.0F, 1.13983F},
    {1.0F, -0.39465F, -0.58060F},
    {1.0F, 2.03211F, 0.0F},
};
}  // namespace

float AccGain(float median_burst_amplitude_ire) {
  return AccGain(median_burst_amplitude_ire, 20.0F);
}

float AccGain(float median_burst_amplitude_ire, float nominal_ire) {
  const float denom = std::max(median_burst_amplitude_ire, 1.0F);
  return std::clamp(nominal_ire / denom, 0.5F, 2.0F);
}

std::array<uint16_t, 3> YuvToRgb16(float y, float u, float v, float black_ire) {
  const float yn = (y - black_ire) / (kIreWhite - black_ire);
  const float un = u / (kIreWhite - kIreBlack);
  const float vn = v / (kIreWhite - kIreBlack);

  std::array<uint16_t, 3> rgb{};
  for (int c = 0; c < 3; ++c) {
    const float lin =
        kYuvToRgb[c][0] * yn + kYuvToRgb[c][1] * un + kYuvToRgb[c][2] * vn;
    const float clipped = std::clamp(lin, 0.0F, 1.0F);
    rgb[c] = static_cast<uint16_t>(clipped * 65535.0F + 0.5F);
  }
  return rgb;
}

}  // namespace hvd
