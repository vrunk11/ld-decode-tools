// SPDX-License-Identifier: GPL-3.0-or-later
//
// sequence.h — the FIELD-granularity chunked 3D pipeline: the port of
// `decode_sequence` in reference/hvd/decoder.py (the reference's ONLY
// validated 3D path).
//
// Real interlace: the two fields of a frame are 1/60 s apart. The decode
// unit here is therefore the FIELD; anything cross-field is a robust,
// motion-gated *term*, never an assumption — which is what removes the
// frame-weave's combing on motion. Temporal neighbours of field f:
//   f±2  same parity, 1 frame apart  -> carrier flip 180 deg
//   f±1  adjacent field, 1/60 s      -> carrier offset 3/4 cycle,
//        |dc| = sqrt(2): non-degenerate, and it restores the woven
//        vertical detail on static content through the optimiser
//   f±3  (extended_temporal)         -> extra same-and-cross-parity data
// All carrier offsets are measured per line via the burst, never assumed.
//
// The caller (stage / export) slices the stream into windows of
// cfg.chunk_frames with cfg.chunk_overlap frames of context on each side,
// prepares the window's fields with PrepareFieldObs, runs
// DecodeFieldWindow, and weaves the output fields into frames by TRUE
// parity. Memory stays bounded to one window.
//
// The drizzle output mode (DrizzleFrame below) is ported at engine level;
// only its stage/export wiring is deferred — a drizzled frame is
// (2 * lines * scale) rows tall, and the raw headerless RGB export stream
// has a fixed per-frame size contract that a mid-stream geometry change
// would corrupt. Wire it as a distinct export geometry when needed.

#ifndef LDDECODE_HVD_ENGINE_SEQUENCE_H_
#define LDDECODE_HVD_ENGINE_SEQUENCE_H_

#include <vector>

#include "engine/hvd_config.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace hvd {

class Fft2d;
struct FieldInput;

// One prepared field: the reference's prepare_field output, i.e. the
// field's ACTIVE picture (rows [first_active_field_line,
// last_active_field_line), cols [active_video_start, active_video_end)) and
// its measured, per-line burst-locked carrier exp(i*phi) with
// cfg.chroma_phase_deg already folded in (same treatment as the frame
// path's WeaveAndBuildCarrier). `parity` is the TRUE spatial parity
// (0 = top field) — pass metadata when available; index%2 breaks on
// captures that start on a second field (inverted weave everywhere).
struct FieldObs {
  Plane s;               // composite active picture (IRE), field geometry
  ComplexPlane carrier;  // exp(i*phi) on the same grid
  int parity = 0;        // 0 = top field, 1 = bottom field
};

// Prepare one field from its raw full-field samples (IRE, as FieldInput
// carries them): burst lock-in per line, active crop, carrier synthesis.
FieldObs PrepareFieldObs(const FieldInput& field, const FieldGeometry& g,
                         const HvdConfig& cfg, int parity);

// One decoded field out of the window.
struct DecodedField {
  Plane luma;           // Y (IRE): exact data fidelity S - Re[chi*carrier]
                        // when cfg.output_fidelity (purist output), else the
                        // anchored/NR luma Ys
  ComplexPlane chroma;  // chi = V - iU (IRE), fully guided separation
};

// What the driver DECIDED for a window — filled when the caller passes a
// non-null pointer; exists for the diagnostic maps (rainbow debugging:
// when an artifact report can't be reproduced synthetically, the first
// question is what the decoder chose on the user's own content).
struct SequenceDiagnostics {
  float resolved_strength = 0.0F;  // after the adaptive resolution
  float ambiguity_ire = 0.0F;      // the measured Y/C ambiguity
  float sigma_ire = 0.0F;          // measured composite noise
  float temporal_eps = 0.0F;       // resolved robust gate
  float nr_eps = 0.0F;
};

// Decode one window of consecutive fields (chunk + overlap, typically
// 2*(chunk_frames + 2*chunk_overlap) fields; all fields must share one
// shape). Ports the whole per-window body of decode_sequence:
// noise self-calibration of the robust gates (temporal_eps/nr_eps),
// holographic inits, Gauss-Seidel passes with per-field pair motion
// (trajectory-fit consensus snap; fast mode's shared motion cache and
// predicted+verified ME for long offsets), motion-compensated raw
// neighbour equations, the InSAR coherence gate (deferred to pass 1 in
// fast mode), the optional PSI closed-form init at pass 0, the
// synth-reference anchor + joint solve from pass 1, and the
// output-fidelity luma policy.
//
// `fft` is the engine's FFT context (holographic inits). The returned
// vector is index-aligned with `fields`; the caller keeps only the
// non-overlap core of the window.
std::vector<DecodedField> DecodeFieldWindow(const std::vector<FieldObs>& fields,
                                            const FieldGeometry& g,
                                            const HvdConfig& cfg, Fft2d* fft,
                                            SequenceDiagnostics* diag = nullptr);


// Test seam: same driver, but with the per-field initial (Y, chi) supplied
// by the caller instead of computed by HolographicInit — lets the whole 3D
// machinery be exercised (and unit-tested) without an FFT backend. `fft`
// may be null in this variant. `inits` must be index-aligned with
// `fields`; luma entries may be empty (Y0 is derived from chi0 when so).
// Astronomy-style 'drizzle' stacking (reference drizzle_frame, line 1186),
// adapted to interlaced NTSC: VERTICAL super-resolution by scatter
// accumulation. When many observations of one scene exist at different
// sub-pixel offsets — the free half-line parity phase between opposite
// fields, plus measured sub-pixel vertical motion — depositing each source
// sample at its mapped position on a finer grid recovers detail beyond any
// single observation's sampling (Fruchter & Hook, HST). True for NTSC only
// vertically: 480 scan lines with no optical vertical prefilter alias
// heavily. Robust Geman-McClure weights (agreement with the target field's
// own decode, times block-matching confidence) reject motion/occlusion
// outliers, so this degrades gracefully to a plain 2x weave-interpolation
// where no valid extra phases exist. Output: (2 * lines * scale) x width.
// Uses the ANCHORED fields (Ys), not the purist ones — drizzle is
// inherently a stacking mode.
struct DrizzleResult {
  Plane luma;
  ComplexPlane chroma;
};
DrizzleResult DrizzleFrame(int j0, const std::vector<Plane>& Ys,
                           const std::vector<ComplexPlane>& chis,
                           const std::vector<int>& parities,
                           const HvdConfig& cfg, int scale = 2);

std::vector<DecodedField> DecodeFieldWindowWithInits(
    const std::vector<FieldObs>& fields,
    const std::vector<DecodedField>& inits, const FieldGeometry& g,
    const HvdConfig& cfg, SequenceDiagnostics* diag = nullptr);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_SEQUENCE_H_
