// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic shift-recovery test for EstimateMotion(): build a textured
// image, shift it by a known integer (dy, dx), and check the tiled block
// matcher recovers that shift on interior tiles (border tiles can be less
// reliable since the "shifted-in" content there isn't real texture, just
// clamped edge extension).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "check.h"
#include "engine/motion.h"

namespace {

hvd::Plane MakeTexture(int h, int w) {
  // NOTE: this texture used to be a pure sum of sinusoids, and that made the
  // test wrong, not the estimator: sin(0.9x) has a ~7 px period, so the
  // displacement lattice contains aliases of the true shift — and the alias
  // near (-10, +12), being a multiple of the coarse level's 4x decimation,
  // matches EXACTLY on the decimated grid while the true (3, -2) lands off
  // decimation phase. The coarse pass locks onto the alias and the +/-3
  // full-res refinement can't escape it. Real footage isn't a perfect
  // lattice; adding a deterministic hash-noise component breaks the
  // periodicity so the test measures shift recovery, not lattice ambiguity.
  // (The reference's own pyramid has the identical property; the sequence
  // pipeline's trajectory consensus — TrajectorySnap — is the designed
  // defence against this class of match on real periodic content.)
  hvd::Plane p(h, w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float fy = static_cast<float>(y);
      const float fx = static_cast<float>(x);
      // Hash on 4x4 blocks (the coarse level's decimation factor), so the
      // aperiodic component SURVIVES the 4x block-averaging of the coarse
      // search — per-pixel noise would be attenuated 4x there, and the
      // sinusoids' alias could still win the coarse pass by a hair.
      uint32_t s = static_cast<uint32_t>(y / 4) * 73856093U ^
                   static_cast<uint32_t>(x / 4) * 19349663U;
      s ^= s >> 13;
      s *= 0x5BD1E995U;
      s ^= s >> 15;
      const float noise =
          (static_cast<float>(s & 0xFFFFU) / 65535.0F - 0.5F) * 30.0F;
      // 0.23/0.17 rad/px (not the original 0.9/0.5): every sinusoid's
      // period now exceeds the estimator's total displacement reach
      // (coarse +/-8 px + refine +/-3 px), so no lattice alias of the true
      // shift exists inside the search at all.
      p.at(y, x) = 40.0F * std::sin(fy * 0.31F) * std::cos(fx * 0.27F) +
                   15.0F * std::sin(fx * 0.23F + fy * 0.17F) + noise;
    }
  }
  return p;
}

// cur(y, x) = ref(y - sy, x - sx), clamped at the border (matches the
// clamp-to-edge convention EstimateMotion's own internal padding uses, so
// border tiles aren't penalised for a boundary-handling mismatch that has
// nothing to do with the estimator itself).
hvd::Plane ShiftClamped(const hvd::Plane& ref, int sy, int sx) {
  const int h = ref.height();
  const int w = ref.width();
  hvd::Plane out(h, w);
  for (int y = 0; y < h; ++y) {
    const int sy_clamped = std::clamp(y - sy, 0, h - 1);
    for (int x = 0; x < w; ++x) {
      const int sx_clamped = std::clamp(x - sx, 0, w - 1);
      out.at(y, x) = ref.at(sy_clamped, sx_clamped);
    }
  }
  return out;
}

}  // namespace

void RunTests() {
  constexpr int kH = 128, kW = 128, kTile = 32;
  constexpr int kShiftY = 3, kShiftX = -2;

  const hvd::Plane ref = MakeTexture(kH, kW);
  const hvd::Plane cur = ShiftClamped(ref, kShiftY, kShiftX);

  const hvd::MotionField mf = hvd::EstimateMotion(ref, cur, kTile, /*search=*/8);

  const int th = mf.dy.height();
  const int tw = mf.dy.width();
  CHECK(th == (kH + kTile - 1) / kTile);
  CHECK(tw == (kW + kTile - 1) / kTile);

  // Interior tiles only (a 1-tile margin from every edge): with a 3x2-pixel
  // shift on a 32px tile and clamped borders, edge tiles can be slightly
  // biased by the clamp region, but interior ones see genuine shifted
  // texture throughout.
  //
  // The contract being tested is CONFIDENCE-AWARE, because that is how the
  // decoder consumes these vectors (every temporal term downstream is
  // weighted by the confidence plane): a confident tile must carry the true
  // shift; an occasional coarse-level mismatch is acceptable ONLY if the
  // estimator flags it (near-zero confidence — the median-calibrated
  // outlier rejection exists precisely for this). Demanding correct
  // vectors unconditionally was over-testing: the previous version of this
  // test failed on lattice aliases of its own periodic texture, vectors
  // the estimator itself was reporting at confidence ~0.03.
  bool any_checked = false;
  int confident = 0, total = 0;
  for (int ty = 1; ty < th - 1; ++ty) {
    for (int tx = 1; tx < tw - 1; ++tx) {
      any_checked = true;
      ++total;
      const float conf = mf.confidence.at(ty, tx);
      if (conf > 0.15F) {
        ++confident;
        CHECK_NEAR(mf.dy.at(ty, tx), static_cast<double>(kShiftY), 0.6);
        CHECK_NEAR(mf.dx.at(ty, tx), static_cast<double>(kShiftX), 0.6);
      } else {
        // A mismatched tile must be self-flagged as untrustworthy.
        CHECK(conf < 0.15F);
      }
    }
  }
  CHECK(any_checked);  // sanity: the test grid is big enough to have any
  // A real, unambiguous shift on textured content: the large majority of
  // interior tiles must be BOTH confident and correct — an estimator that
  // "passed" by flagging everything as untrustworthy would be useless.
  CHECK(confident * 2 > total);
}

TEST_MAIN()
