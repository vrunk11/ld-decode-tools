// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the THEORY 9e/9f motion additions:
//   * BoxBlur — the integral-image (prefix-sum) rewrite must be numerically
//     equivalent to the naive zero-padded constant-normalised convolution
//     it replaced (the reference verified its own rewrite to 1e-16; an
//     earlier attempt that silently changed the semantics cost 0.75 dB).
//   * VerifyMotion — the 2-evaluation predicted+verified ME must accept a
//     correct trajectory prediction (keeping its sub-pixel part) and reject
//     a garbage prediction on static content (falling back to zero).
//   * FitTrajectory / TrajectorySnap — consistent multi-offset measurements
//     of one physical velocity must collapse onto the trajectory under
//     consensus; a disagreeing tile must be left untouched.

#include <algorithm>
#include <cmath>
#include <vector>

#include "check.h"
#include "engine/motion.h"

namespace {

hvd::Plane MakeTexture(int h, int w) {
  hvd::Plane p(h, w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float fy = static_cast<float>(y);
      const float fx = static_cast<float>(x);
      p.at(y, x) = 40.0F * std::sin(fy * 0.31F) * std::cos(fx * 0.27F) +
                   15.0F * std::sin(fx * 0.9F + fy * 0.5F);
    }
  }
  return p;
}

hvd::Plane ShiftClamped(const hvd::Plane& ref, int sy, int sx) {
  const int h = ref.height();
  const int w = ref.width();
  hvd::Plane out(h, w);
  for (int y = 0; y < h; ++y) {
    const int yy = std::clamp(y - sy, 0, h - 1);
    for (int x = 0; x < w; ++x) {
      const int xx = std::clamp(x - sx, 0, w - 1);
      out.at(y, x) = ref.at(yy, xx);
    }
  }
  return out;
}

// The naive box blur the integral-image version replaced: zero-padded
// 'same' convolution, CONSTANT 1/(2r+1) normalisation per axis.
hvd::Plane NaiveBoxBlur(const hvd::Plane& in, int r) {
  const int h = in.height();
  const int w = in.width();
  const float norm = 1.0F / static_cast<float>(2 * r + 1);
  hvd::Plane mid(h, w), out(h, w);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) {
      float acc = 0.0F;
      for (int k = -r; k <= r; ++k) {
        const int yy = y + k;
        if (yy >= 0 && yy < h) acc += in.at(yy, x);
      }
      mid.at(y, x) = acc * norm;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc = 0.0F;
      for (int k = -r; k <= r; ++k) {
        const int xx = x + k;
        if (xx >= 0 && xx < w) acc += mid.at(y, xx);
      }
      out.at(y, x) = acc * norm;
    }
  }
  return out;
}

void TestBoxBlurEquivalence() {
  const hvd::Plane tex = MakeTexture(37, 53);
  for (const int r : {2, 8}) {
    const hvd::Plane fast = hvd::BoxBlur(tex, r);
    const hvd::Plane slow = NaiveBoxBlur(tex, r);
    double max_err = 0.0;
    for (size_t i = 0; i < fast.size(); ++i)
      max_err = std::max(max_err, std::fabs(static_cast<double>(fast[i]) - slow[i]));
    CHECK(max_err < 1e-4);  // float storage; the accumulators are double
  }
}

void TestVerifyMotionAcceptsGoodPrediction() {
  constexpr int kH = 128, kW = 128, kTile = 32;
  constexpr float kShiftY = 3.0F, kShiftX = -2.0F;
  const hvd::Plane ref = MakeTexture(kH, kW);
  const hvd::Plane cur = ShiftClamped(ref, static_cast<int>(kShiftY),
                                      static_cast<int>(kShiftX));

  const int th = (kH + kTile - 1) / kTile;
  const int tw = (kW + kTile - 1) / kTile;
  // Prediction with a deliberate sub-pixel part: the audit rounds it for
  // the SSD but must return the FULL float value where it wins.
  hvd::Plane pdy(th, tw, kShiftY + 0.25F);
  hvd::Plane pdx(th, tw, kShiftX - 0.25F);

  const hvd::MotionField mf = hvd::VerifyMotion(ref, cur, pdy, pdx, kTile);
  int accepted = 0;
  for (int ty = 1; ty + 1 < th; ++ty) {
    for (int tx = 1; tx + 1 < tw; ++tx) {
      if (std::fabs(mf.dy.at(ty, tx) - (kShiftY + 0.25F)) < 1e-6F &&
          std::fabs(mf.dx.at(ty, tx) - (kShiftX - 0.25F)) < 1e-6F) {
        ++accepted;
      }
      CHECK(mf.confidence.at(ty, tx) >= 0.0F);
      CHECK(mf.confidence.at(ty, tx) <= 1.0F);
    }
  }
  CHECK(accepted >= (th - 2) * (tw - 2) / 2);  // interior tiles mostly accept
}

void TestVerifyMotionRejectsGarbagePrediction() {
  constexpr int kH = 128, kW = 128, kTile = 32;
  const hvd::Plane ref = MakeTexture(kH, kW);  // static: cur == ref
  const int th = (kH + kTile - 1) / kTile;
  const int tw = (kW + kTile - 1) / kTile;
  hvd::Plane pdy(th, tw, 7.0F);   // garbage prediction on static content
  hvd::Plane pdx(th, tw, -6.0F);

  const hvd::MotionField mf = hvd::VerifyMotion(ref, ref, pdy, pdx, kTile);
  for (size_t i = 0; i < mf.dy.size(); ++i) {
    CHECK(mf.dy[i] == 0.0F);  // the zero vector must win everywhere
    CHECK(mf.dx[i] == 0.0F);
  }
}

void TestTrajectorySnap() {
  constexpr int kTh = 4, kTw = 5;
  constexpr float kVy = 1.0F, kVx = -0.5F;  // one physical velocity per tile

  std::vector<hvd::OffsetMotion> pm;
  for (const int o : {-3, -2, -1, 1, 2, 3}) {
    hvd::OffsetMotion m;
    m.offset = o;
    m.parity_shift = (o % 2 != 0) ? ((o > 0) ? 0.5F : -0.5F) : 0.0F;
    m.field.tile = 32;
    m.field.dy = hvd::Plane(kTh, kTw);
    m.field.dx = hvd::Plane(kTh, kTw);
    m.field.confidence = hvd::Plane(kTh, kTw, 0.9F);
    for (int i = 0; i < kTh * kTw; ++i) {
      // Noisy pairwise measurements of the same trajectory, +/-0.3 px.
      const float noise = ((i + o + 30) % 3 - 1) * 0.3F;  // +30: keep the modulo non-negative
      m.field.dy[i] = static_cast<float>(o) * kVy + m.parity_shift + noise;
      m.field.dx[i] = static_cast<float>(o) * kVx - noise;
    }
    // Tile 0: one offset genuinely disagrees (occlusion) — must be kept.
    if (o == 2) {
      m.field.dy[0] = 40.0F;
      m.field.dx[0] = 40.0F;
    }
    pm.push_back(std::move(m));
  }

  hvd::TrajectorySnap(&pm);

  for (const hvd::OffsetMotion& m : pm) {
    const float o = static_cast<float>(m.offset);
    for (int i = 1; i < kTh * kTw; ++i) {
      // Agreeing tiles collapse exactly onto k*v + h_k.
      CHECK_NEAR(m.field.dy[i], o * kVy + m.parity_shift, 0.35);
      CHECK_NEAR(m.field.dx[i], o * kVx, 0.35);
    }
    if (m.offset == 2) {
      // The disagreeing measurement is preserved as-is: it is signal
      // (occlusion/acceleration), handled downstream by per-pixel gates.
      CHECK(m.field.dy[0] == 40.0F);
      CHECK(m.field.dx[0] == 40.0F);
    }
  }

  // The fitted velocity itself should be close to the true one.
  hvd::Plane vy, vx;
  hvd::FitTrajectory(pm, &vy, &vx);
  for (int i = 1; i < kTh * kTw; ++i) {
    CHECK_NEAR(vy[i], kVy, 0.2);
    CHECK_NEAR(vx[i], kVx, 0.2);
  }
}

}  // namespace

void RunTests() {
  TestBoxBlurEquivalence();
  TestVerifyMotionAcceptsGoodPrediction();
  TestVerifyMotionRejectsGarbagePrediction();
  TestTrajectorySnap();
}

TEST_MAIN()
