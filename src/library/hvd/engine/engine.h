// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine.h — the HVD decoder facade.
//
// This is the single, narrow interface the decode-orc SDK layer talks to. It
// contains NO SDK types, so it can be unit-tested and reasoned about in
// isolation. It orchestrates the ported 2-D woven-frame pipeline:
//
//   per field:  lock-in burst phase  (lockin.h)
//   weave:      interleave the two fields' active picture into frame geometry
//   init:       holographic reconstruction of chi           (holographic_init.h)
//   refine:     Y/C arbitration by IRLS + conjugate gradient (variational.h)
//   output:     lossless Y/C split  luma + chroma == composite
//
// A single Fft2d (FFTW plan cache) is owned by the engine and reused across
// frames, so repeated decoding of a sequence does not re-plan.
//
// 3-D / temporal decoding: DecodeFrame optionally takes a list of PREVIOUS
// woven frames' raw state (see NeighborRawState in temporal.h) and motion-
// compensates against them (temporal.h's MotionCompensatePrev), feeding the
// result to VariationalRefine's neighbours parameter. This is decode_frame's
// own frame-level 3D mode in the reference — a separate, much simpler
// architecture than decode_sequence's field-granularity chunked/Gauss-Seidel
// pipeline (bidirectional neighbours, multi-pass refinement, synth_reference
// NR anchor, drizzle), which is the sequence pipeline of
// sequence.h, used by DecodeSequenceWindow. HvdEngine::DecodeFrame only ever sees PAST frames as neighbours —
// a future frame isn't available yet when decoding sequentially one frame
// at a time; decode_sequence's bidirectional mode needs the chunked driver.

#ifndef LDDECODE_HVD_ENGINE_ENGINE_H_
#define LDDECODE_HVD_ENGINE_ENGINE_H_

#include <memory>
#include <vector>

#include "engine/hvd_config.h"
#include "engine/sequence.h"
#include "engine/ntsc_geometry.h"
#include "engine/plane.h"
#include "engine/temporal.h"

namespace hvd {

class Fft2d;  // forward declaration (owned via a unique_ptr)

// One field, in IRE, plus the metadata the weave needs. `samples` is
// (field_height x field_width).
struct FieldInput {
  Plane samples;        // composite, IRE
  bool is_first_field;  // true => this field's lines are the top (even) rows
};

// Woven-frame decode result over the active picture area. The three planes
// share the same geometry (2 * active_lines rows x active_width cols) and, by
// construction, satisfy  luma + chroma == composite  elementwise (the lossless
// Y/C split). `chroma_phasor` is the baseband chroma chi = V - iU, provided for
// an optional colour/preview path (not needed for the split output).
struct FrameYc {
  Plane luma;                  // Y (IRE)
  Plane chroma;                // C = S - Y = Re[chi * carrier]  (IRE, AC)
  Plane composite;             // S (IRE), for verification / passthrough
  ComplexPlane chroma_phasor;  // chi = V - iU (IRE)
  ComplexPlane carrier;        // exp(i*phi) used for this frame — retain this
                                // (with luma/composite) if you want to use
                                // this frame as a future DecodeFrame call's
                                // temporal neighbour (see NeighborRawState).
  float acc_gain = 1.0F;       // measured Automatic Color Control gain
};

class HvdEngine {
 public:
  HvdEngine();
  ~HvdEngine();

  HvdEngine(const HvdEngine&) = delete;
  HvdEngine& operator=(const HvdEngine&) = delete;

  // Decode one frame from its two fields. `first` and `second` may be given in
  // capture order; the engine orders first-field lines onto the even rows using
  // is_first_field. `g` is the field geometry, `cfg` the knobs.
  //
  // `prev_frames`: optional raw state (luma/composite/carrier) of one or
  // more PREVIOUS woven frames, used as motion-compensated temporal data
  // terms when cfg.temporal_strength > 0 (ignored otherwise, and ignored
  // if empty regardless of temporal_strength — see hvd_config.h). The
  // caller is responsible for keeping these around across calls (e.g. the
  // previous call's own FrameYc::luma/composite + the carrier used for
  // it); this engine has no memory of past frames itself.
  FrameYc DecodeFrame(const FieldInput& first, const FieldInput& second,
                      const FieldGeometry& g, const HvdConfig& cfg);

  // For sources that are ALREADY Y/C separated at capture (has_separate_
  // channels() on the host side — S-Video-style captures, some hi-fi VHS
  // formats): `first`/`second` here are the CHROMA-ONLY fields (already
  // re-centred to a signed zero-mean oscillation by the caller — see
  // frame_bridge.cpp's chroma_dc handling), with no luma mixed in. There is
  // no separation problem to solve here — the source already solved it —
  // so this skips the variational arbitration entirely (nothing to
  // arbitrate: residual luma is ~0 by construction) and just does burst
  // lock-in + a holographic-bandwidth crop to get chi. Cheaper than
  // DecodeFrame and correct for this input shape; using DecodeFrame here
  // instead (on a real composite reconstructed as luma-only, no chroma)
  // silently produces zero chroma — that used to be a real bug.
  // Returned FrameYc.luma is meaningless (approximately zero) — the caller
  // should get luma from the source's own clean Y channel directly instead.
  FrameYc DecodeChromaOnly(const FieldInput& first, const FieldInput& second,
                           const FieldGeometry& g, const HvdConfig& cfg);

  // Decode a window of consecutive fields through the sequence pipeline
  // (see sequence.h) using this engine's FFT context for the holographic
  // inits. Thin wrapper so callers don't need access to the private Fft2d.
  std::vector<DecodedField> DecodeSequenceWindow(
      const std::vector<FieldObs>& fields, const FieldGeometry& g,
      const HvdConfig& cfg, SequenceDiagnostics* diag = nullptr);

  // Forwards to Fft2d::SetThreadCount — see its doc comment in fft2d.h.
  // Parallel-export workers (hvd_chroma_decoder_stage.cpp) MUST call this
  // with 1 on their own per-thread HvdEngine before decoding anything, or
  // FFTW would fan its own transforms out across every core PER WORKER,
  // oversubscribing on top of the frame-level parallelism already in play.
  // The single shared engine used by the normal preview/cached path
  // doesn't need to call this — it defaults to using every core, which is
  // what a single-frame-at-a-time workload wants.
  void SetFftThreads(int n);

 private:
  std::unique_ptr<Fft2d> fft_;
};

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_ENGINE_H_
