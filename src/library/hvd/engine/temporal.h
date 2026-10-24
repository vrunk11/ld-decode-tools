// SPDX-License-Identifier: GPL-3.0-or-later
//
// temporal.h — motion-compensated warp and envelope primitives for the
// (upcoming) 3-D temporal path. Faithful port of the RAW-COMPOSITE side of
// reference/hvd/decoder.py: _vectors_per_pixel, warp_by_tiles,
// _warp_bilinear_tiles, envelope_of, complex_coherence, mc_warp,
// motion_compensate_envelope, motion_compensate_prev.
//
// Deliberately NOT included here: synth_reference. That function builds a
// denoised reference from already-DECODED neighbor fields (Ys[k]/chis[k]),
// which is the Gauss-Seidel-within-a-chunk dependency discussed for the
// chunked pipeline — a later step once the chunk/window driver exists.
// Everything in this file only ever touches neighbors' RAW measurements
// (composite + measured carrier phase), which is why motion.h/this file
// have no inter-frame decode-order dependency at all.

#ifndef LDDECODE_HVD_ENGINE_TEMPORAL_H_
#define LDDECODE_HVD_ENGINE_TEMPORAL_H_

#include "engine/motion.h"
#include "engine/plane.h"

#include <vector>

namespace hvd {

// Bilinearly interpolates a per-tile motion field (mdy/mdx, shape
// tiles_h x tiles_w) between TILE CENTERS onto a full-resolution (h x w)
// per-pixel field, after a 3x3 component-wise median outlier-snap (a
// vector more than 3px away from its own 3x3 median is replaced by that
// median — squashes isolated garbage from flat tiles before it can bleed
// into neighbors through the interpolation; coherent motion clusters pass
// through untouched). This is what removes the visible tile-seam artefacts
// of a piecewise-constant warp.
struct PerPixelMotion {
  Plane dy;  // h x w
  Plane dx;  // h x w
};
PerPixelMotion VectorsPerPixel(const Plane& mdy, const Plane& mdx, int tile,
                               int out_h, int out_w);

// Nearest-tile, integer-pel warp: shifts each pixel of `a` by its
// (interpolated, then rounded) per-pixel vector. Matches warp_by_tiles —
// legal on a raw carrier-modulated signal, since it's a pure sample shift
// (no resampling of the carrier itself).
Plane WarpByTiles(const Plane& a, const Plane& mdy, const Plane& mdx,
                  int tile);
// Precomputed-vector overload (THEORY 9f, "per-pixel warp vectors computed
// once per warp trio instead of three times"): pass VectorsPerPixel's output
// so several warps of the SAME motion field share one interpolation.
Plane WarpByTiles(const Plane& a, const PerPixelMotion& v);

// Bilinear, sub-pixel warp of a BASEBAND array (post envelope_of — never
// call this on a still-modulated composite, sub-pixel interpolation of a
// carrier is not legal). `row_offset` is a constant extra vertical shift
// (grid alignment between two fields of different parity); the output grid
// size can differ from the input's (envelope arrays have one fewer row).
Plane WarpBilinearTiles(const Plane& a, const Plane& dyf, const Plane& dxf,
                        int tile, float row_offset, int out_h, int out_w);
// Precomputed-vector overload (see WarpByTiles above); `v` must have been
// interpolated at (out_h, out_w).
Plane WarpBilinearTiles(const Plane& a, const PerPixelMotion& v,
                        float row_offset, int out_h, int out_w);

// InSAR-style local complex coherence between two chroma phasor fields:
//   gamma = |<z1 * conj(z2)>| / sqrt(<|z1|^2> * <|z2|^2>)
// (box-blurred local averages, radius r). Phase-sensitive where a plain
// magnitude/SSD comparison isn't: two chroma fields can have similar
// energy yet decorrelated phase (motion residual, scene change) — exactly
// where a temporal chroma equation would otherwise turn toxic. ~1 where
// the phasors line up, -> 0 where they decorrelate. Output in [0, 1].
Plane ComplexCoherence(const ComplexPlane& z1, const ComplexPlane& z2, int r);

// Same integer-pel, nearest-tile warp as WarpByTiles, but for complex data.
// Only valid for an INTEGER shift with no interpolation (as used in
// MotionCompensatePrev's carrier warp) — warping a complex exponential by a
// pure sample shift commutes with warping its angle then re-exponentiating,
// which is exactly what makes this legal; it would NOT be legal to use this
// for a sub-pixel/bilinear warp of a still-modulated carrier.
ComplexPlane WarpByTilesComplex(const ComplexPlane& a, const Plane& mdy,
                                const Plane& mdx, int tile);
// Precomputed-vector overload (see WarpByTiles above).
ComplexPlane WarpByTilesComplex(const ComplexPlane& a, const PerPixelMotion& v);

// Upsamples a per-tile confidence field to per-pixel: NEAREST-tile (not
// VectorsPerPixel's smooth interpolation — matches the reference's
// conf[ty,tx] indexing exactly), squared (crushes weak matches harder than
// a linear scale would), then box-blurred (radius 8) to avoid hard tile
// seams in the confidence itself.
Plane UpsampleConfidence(const Plane& conf, int tile, int out_h, int out_w);

// FAST-mode confidence upsample (THEORY 9f): the per-tile confidence map,
// squared, bilinearly interpolated between tile centers (VectorsPerPixel's
// interpolation, median-snap included — the reference routes conf**2
// through _vectors_per_pixel itself). The full-res squared upsample +
// radius-8 blur above only ever smoothed at sub-tile scale, so
// interpolating the ~24x24 tile map is ~256x cheaper and visually
// identical.
Plane UpsampleConfidenceFast(const Plane& conf, int tile, int out_h,
                             int out_w);

// One neighbour frame's raw measurements. `luma` is a STABLE reference used
// only for motion matching (the reference uses the holographic-init luma
// here, never a fully-refined decode — deliberately, so multiple IRLS/CG
// passes can't feed back into the motion field they were themselves
// computed from); `composite`/`carrier` are the actual raw measurement
// (composite samples + measured carrier exp(i*phi)) that ends up in the
// synthesized data term.
struct NeighborRawState {
  Plane luma;
  Plane composite;
  ComplexPlane carrier;
  // The neighbour's decoded BASEBAND chroma phasor (chi = V - iU). Not a raw
  // measurement — it exists solely so the InSAR coherence gate
  // (ComplexCoherence, reference decode_sequence's `coherence_gate`) can
  // compare phasor fields and collapse the neighbour's confidence where the
  // chroma phase decorrelates (motion residual, content change) — exactly
  // the situation where a temporal chroma equation turns toxic. May be left
  // empty; the gate is skipped then.
  ComplexPlane chroma;
};

// Simpler neighbour resampling (motion_compensate_prev in the reference):
// integer-pel warp of the neighbour's raw composite AND raw carrier phase
// toward the current frame — no envelope separation, no sub-pixel warp, no
// re-encoding. (An envelope-resampled variant was tried in the reference
// and REJECTED — its 1-line comb leaks vertical luma gradients into
// chroma; the raw warp is the design, per the decode_sequence comment and
// THEORY 9g. The envelope machinery was removed in the cleanup pass.)
struct MotionCompensatedResult {
  Plane composite;       // warped raw composite (IRE)
  ComplexPlane carrier;  // warped carrier phasor
  Plane confidence;      // per-pixel confidence in [0, 1]
};

// `fast` selects the tile-resolution confidence upsample
// (UpsampleConfidenceFast) instead of the full-res squared+blurred one.
// `vpix`: optional precomputed per-pixel vectors for `motion` at the
// composite's resolution (THEORY 9f "once per warp trio" — the sequence
// driver shares one interpolation between this warp pair and the
// coherence gate's chi warp instead of computing it twice per neighbour).
MotionCompensatedResult MotionCompensatePrev(
    const NeighborRawState& prev, const Plane& y_cur_init, int tile,
    int search, const MotionField* motion = nullptr, bool fast = false,
    const PerPixelMotion* vpix = nullptr);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_TEMPORAL_H_
