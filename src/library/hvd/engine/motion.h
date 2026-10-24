// SPDX-License-Identifier: GPL-3.0-or-later
//
// motion.h — tiled block-matching motion estimation, for the (upcoming) 3-D
// temporal path. Faithful port of estimate_motion()/_bm_pass() in
// reference/hvd/decoder.py: coarse-to-fine integer-pel block matching (4x
// decimated exhaustive search, then a +/-3 px full-res refinement around the
// coarse vector), a parabolic half-pel sub-pixel fit, a "prefer zero motion"
// margin rule (regularises against spurious vectors in flat/noisy tiles),
// and a per-tile confidence estimate.
//
// STATUS: this is stage 1 of the 3-D port. It is
// self-contained and independently testable (tests/engine/motion_test.cpp)
// but not yet wired into HvdEngine — the temporal blending that consumes
// this motion field (motion-compensated reference synthesis, joint
// variational refinement across frames, drizzle) is later work, ported and
// tested incrementally rather than in one pass.

#ifndef LDDECODE_HVD_ENGINE_MOTION_H_
#define LDDECODE_HVD_ENGINE_MOTION_H_

#include <vector>

#include "engine/plane.h"

namespace hvd {

// Per-tile motion field. dy/dx/confidence are all (tiles_h x tiles_w),
// where tiles_h = ceil(height / tile), tiles_w = ceil(width / tile) of the
// planes passed to EstimateMotion. dy/dx are in full-resolution pixels
// (integer part + up to +/-0.5 px sub-pixel refinement); confidence is in
// [0, 1], with 0 meaning "don't trust this tile's vector".
struct MotionField {
  Plane dy;
  Plane dx;
  Plane confidence;
  int tile = 32;
};

// Estimate per-tile motion from y_ref (reference/previous field) to y_cur
// (current field), both same-shaped luma-like planes (any consistent linear
// unit — IRE or raw code deltas both work, only relative contrast matters).
// `tile`: block size in pixels (reference default 32).
// `search`: full-resolution search radius in pixels (reference default 8) —
// the actual full-res refinement pass is always +/-3 px around a coarse
// vector found by a 4x-decimated search over +/-ceil(search/4), which is
// what makes this ~50x cheaper than an exhaustive full-res search of the
// same radius while matching the reference's behaviour (see estimate_motion
// in reference/hvd/decoder.py for the rationale).
MotionField EstimateMotion(const Plane& y_ref, const Plane& y_cur,
                            int tile = 32, int search = 8);

// Separable box blur, radius r (kernel 2r+1), zero-padded 'same' with
// CONSTANT 1/(2r+1) normalisation per axis (edges attenuated, NOT
// renormalised — numpy.convolve semantics; the ME pre-blur depends on this
// and on the r=2 default). Implemented with per-axis prefix sums (integral
// images): O(n) regardless of radius, per THEORY 9f's "exact rewrites
// benefiting both modes". Exposed here (it used to be file-local) so the
// equivalence against a naive convolution can be unit-tested — the
// reference verified its own rewrite to 1e-16, and an earlier attempt that
// silently changed the default radius cost 0.75 dB before the benches
// caught it.
Plane BoxBlur(Plane a, int r);

// FAST-mode motion estimation for long temporal offsets (port of
// decoder.verify_motion): instead of the full pyramid search (~130 SSD
// evaluations), evaluate ONLY the trajectory-predicted vector and the zero
// vector (2 evaluations), keep the better per tile, and compute the
// standard confidence layers. The trajectory supplies the hypothesis; this
// supplies the audit. Long offsets are exactly where full search is least
// reliable anyway (more occlusion), so the quality cost is imperceptible
// while the ME cost drops ~60x on those offsets. pdy/pdx are per-tile
// float predictions (tiles_h x tiles_w); where the prediction wins, its
// sub-pixel part is kept.
MotionField VerifyMotion(const Plane& y_ref, const Plane& y_cur,
                         const Plane& pdy, const Plane& pdx, int tile = 32);

// One pairwise motion measurement inside a multi-offset trajectory fit:
// `offset` is the signed temporal offset k (fields), `parity_shift` is the
// known half-line geometry term h_k = (p_k - p_j) / 2 that static content
// of parity p_k exhibits when matched from parity p_j (THEORY 9e —
// derivation matters: the sign was got wrong once and doubled the bias
// instead of removing it).
struct OffsetMotion {
  int offset = 0;
  float parity_shift = 0.0F;
  MotionField field;
};

// Fit ONE per-tile velocity across all temporal offsets (they are up to six
// noisy measurements of one physical motion): the median over offsets of
// (dy_k - h_k) / k and dx_k / k, with low-confidence samples (conf <= 0.15)
// masked out of the median (empty tiles fall back to 0). Port of the
// trajectory-fit block in decode_sequence (THEORY 9e / 9f).
void FitTrajectory(const std::vector<OffsetMotion>& measurements,
                   Plane* vy, Plane* vx);

// Snap agreeing pairwise vectors onto the fitted trajectory k*v + h_k,
// under CONSENSUS: the snap only applies on tiles where >= 3 offsets agree
// with the trajectory within 1.5 px (L1 over dy+dx). Structured motion
// collapses onto the trajectory ("the 1D filter that follows the matter");
// noise-dominated matching and genuine disagreement (occlusion,
// acceleration — signal, not noise) are left untouched, handled downstream
// by the per-pixel gates. Requires >= 2 measurements; fewer is a no-op.
// Confidence planes are not modified. Reference measurement: +0.08/+0.09 dB
// on both benches, never negative.
void TrajectorySnap(std::vector<OffsetMotion>* measurements);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_MOTION_H_
