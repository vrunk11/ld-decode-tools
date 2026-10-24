// SPDX-License-Identifier: GPL-3.0-or-later
//
// frame_bridge.h — SDK-independent bridge between a decode-orc flat frame
// buffer and the HVD engine.
//
// This file has NO decode-orc dependency: it operates on a plain int16_t flat
// buffer plus a small `FrameParams` mirror of the fields the stage reads from
// orc::SourceParameters. That keeps the tricky, testable logic (level
// conversion, field-sequential de-weave/re-weave, lossless requantisation) out
// of the SDK glue.
//
// LAYOUT (verified against orc/stage/video_frame_representation.h and
// cvbs_signal_constants.h): a CVBS_U10_4FSC frame is a flat buffer laid out
// FIELD-SEQUENTIALLY — all `field1_lines` lines of field 1, then the remaining
// lines of field 2:  [field1_line0 .. field1_lineN | field2_line0 .. ].
// Field 1 is the top spatial field, so spatial row 2k = field1 line k and
// spatial row 2k+1 = field2 line k. The engine runs per-field lock-in, so we
// split the buffer by field, decode, and write the Y/C split back into the same
// field-sequential layout.
//
// The active-picture line indices in orc::SourceParameters
// (first/last_active_frame_line) follow the ld-decode woven convention where
// field_line = frame_line / 2; that halving is applied here.

#ifndef LDDECODE_HVD_FRAME_BRIDGE_H_
#define LDDECODE_HVD_FRAME_BRIDGE_H_

#include <cstdint>
#include <vector>

#include "engine/engine.h"
#include "engine/hvd_config.h"
#include "engine/ntsc_geometry.h"

namespace hvd {

// The subset of orc::SourceParameters the decode needs, in the 10-bit domain.
// Populated by the stage from the host's SourceParameters; kept SDK-free so the
// bridge and engine stay unit-testable without the host.
struct FrameParams {
  // Composite standard. Replaces the old `bool is_pal`, which could only
  // say "625-line PAL or not" and therefore routed PAL-M -- PAL chroma on
  // an NTSC raster -- down the NTSC path, decoding it with no V-switch and
  // a 180 deg/line carrier model instead of 90 deg/line. PAL-N is still
  // unsupported (it needs a 90.43 deg/line derivation) and is rejected by
  // the stage rather than silently mis-decoded.
  VideoStandard standard = VideoStandard::kNtsc;

  // Back-compat convenience for callers that only care about the V-switch.
  bool uses_vswitch() const {
    return standard == VideoStandard::kPal || standard == VideoStandard::kPalM;
  }

  int frame_width = 0;   // samples per line (910 NTSC / 1135 PAL / 909 PAL-M)
  int frame_height = 0;  // total flat lines (525 NTSC)
  int field1_lines = 0;  // lines of field 1 in the flat buffer (263 NTSC)
  int active_video_start = 0;
  int active_video_end = 0;
  int colour_burst_start = 0;
  int colour_burst_end = 0;
  int first_active_frame_line = 0;  // woven-frame convention (see header)
  int last_active_frame_line = 0;   // 0 => derive from field height
  float black_level = 0.0F;
  float white_level = 0.0F;
  float blanking_level = 0.0F;
  float chroma_dc = 0.0F;  // chroma DC centre for the C channel
  double sample_rate = kFs4Fsc;
  // Non-standard colour subcarrier (Hz), 0 => the standard's nominal fsc.
  // The stage sets this from HvdConfig::custom_subcarrier /
  // HvdConfig::subcarrier_hz; see FieldGeometry::subcarrier_hz.
  double subcarrier_hz = 0.0;

  // NON-ORTHOGONAL LINES (PAL only).
  //
  // 625-line PAL is not orthogonally sampled: a frame is 709 379 samples,
  // not 625 * 1135 = 709 375. EBU Tech. 3280-E §1.2 puts the 4 extra
  // samples on the last line of each field, so frame-flat lines 312 and 624
  // carry 1137 samples and every other line carries 1135. The host says so
  // explicitly (orc::kPalExtraSampleLines, frame_line_sample_offset() in
  // <orc/support/frame_line_util.h>, and VideoFrameRepresentation's own
  // "PAL frames have non-uniform line lengths" contract).
  //
  // Reading such a buffer with a flat `line * frame_width` stride -- which
  // is what this bridge used to do -- puts the whole of field 2 two samples
  // early, because lines 312's two extras sit before it. Two samples at
  // 4fsc is 180 deg of subcarrier and a 2 px horizontal misregistration
  // between the fields, which quietly corrupts the weave and every
  // inter-field carrier relationship the engine depends on. (The per-line
  // lock-in hides it in the demodulation itself: the burst shifts with the
  // rest of the line, so each line stays self-consistent. Only the
  // field-to-field geometry breaks -- which is exactly the part that is
  // hardest to spot by eye.)
  //
  // Populated by the stage from the SDK helper; an empty table means a
  // uniform stride (NTSC, PAL-M, and every synthetic test geometry). Held
  // as a fixed array rather than a vector so FrameParams stays cheap to
  // copy -- the stage returns it by value on every frame.
  static constexpr int kMaxExtraSampleLines = 8;
  int32_t extra_sample_lines[kMaxExtraSampleLines] = {};
  int extra_sample_line_count = 0;
};

// Sample offset of frame-flat line `line` within the SOURCE buffer.
// Equivalent to orc::frame_line_sample_offset(), re-derived here so this
// header keeps its no-SDK-dependency property.
inline int64_t FrameLineOffset(const FrameParams& fp, int line) {
  int64_t offset = static_cast<int64_t>(line) * fp.frame_width;
  for (int i = 0; i < fp.extra_sample_line_count; ++i) {
    if (fp.extra_sample_lines[i] < line) ++offset;
  }
  return offset;
}

// A decoded Y/C frame in the 10-bit sample domain (int16_t == VFR sample_type),
// same field-sequential geometry as the input. By construction
// luma[i] + (chroma[i] - chroma_dc) reconstructs the composite to within one
// code. Both buffers are frame_width * frame_height samples.
//
// u_plane/v_plane carry the BASEBAND chrominance (chroma_phasor = V - iU),
// not the modulated `chroma` channel above — they are what a colour preview
// needs (no carrier to demodulate), on the same code-delta numeric scale as
// luma. Zero outside the active picture. Same frame_width * frame_height size.
struct YcFrameS16 {
  int width = 0;
  int height = 0;
  float chroma_dc = 0.0F;
  std::vector<int16_t> luma;
  std::vector<int16_t> chroma;
  std::vector<double> u_plane;
  std::vector<double> v_plane;
};

// Build the engine's field geometry from the host frame parameters.
FieldGeometry FieldGeometryFromParams(const FrameParams& fp);

// Decode one field-sequential CVBS frame into a lossless Y/C split.
// `engine` is reused across calls by the caller (its FFTW plan cache only
// pays the planning cost once per distinct frame size, not every frame —
// see engine.h). NOT safe to call concurrently on the same `engine` from
// multiple threads; each thread needs its own.
//   `frame` : frame_width * frame_height samples (row-major, 10-bit codes).
//
// Frame-weave 2D decode of one composite frame — the LAST-RESORT fallback
// only: the composite pipeline is field-granularity everywhere
// (DecodeFrameSequenceWindow below), because weaving before decoding
// contaminates the separation across time on anything moving (THEORY
// section 5). No temporal state: the frame-level 3D path was removed with
// the field migration.
YcFrameS16 DecodeFrameBuffer(const int16_t* frame, const FrameParams& fp,
                             const HvdConfig& cfg, HvdEngine& engine);

// De-weave a field-sequential composite frame buffer into its two fields
// (IRE), exactly as DecodeFrameBuffer does internally — exposed so the
// sequence path shares one definition of the layout.
// `field1_is_bottom` sets the is_first_field flags accordingly (block 1 of
// the buffer is the temporally-first field; whether it is the TOP field is
// resolved by ResolveField1IsBottom below).
void SplitFrameFields(const int16_t* frame, const FrameParams& fp,
                      FieldInput* top, FieldInput* bottom,
                      bool field1_is_bottom = false);

// Signal-measured field-order detection: under the true spatial order each
// field's lines interpolate the other's at +0.5 line, under the inverted
// order at -0.5 — so comparing |block2 - interp(block1, +0.5)| against
// |block2 - interp(block1, -0.5)| on low-passed active luma votes the
// order deterministically wherever the content has vertical detail
// (motion inflates both sides ~equally; the margin still points the same
// way). Returns +1 (field 1 = top), -1 (field 1 = bottom), or 0 (no
// confident vote — content too flat).
int DetectFieldParity(const FieldInput& block1, const FieldInput& block2,
                      const FrameParams& fp);

// Resolve HvdConfig::field_order for one frame: forced values pass
// through; auto measures with DetectFieldParity and falls back to the
// ld-decode format convention (field 1 = top) on a 0 vote.
bool ResolveField1IsBottom(const FieldInput& block1, const FieldInput& block2,
                           const FrameParams& fp, const HvdConfig& cfg);

// The selective 3D decode of a window of fields (cfg.selective_3d): the 2D decode of the whole window, and the 3D decode again on the
// crop of the tiles where luma and chroma are most ambiguous, blended in. Falls back to full 3D when the ambiguity is too wide to crop.
std::vector<DecodedField> DecodeFieldsSelective(
    const std::vector<FieldObs>& fields, const FieldGeometry& g,
    const HvdConfig& cfg, HvdEngine& engine, SequenceDiagnostics* diag);

// Decode a WINDOW of consecutive frames through the field-granularity
// sequence pipeline (engine/sequence.h — the reference's decode_sequence,
// its only validated 3D and the designed answer to frame-weave combing on
// motion). `frames` are the raw field-sequential buffers of frames
// [w0, w0+frames.size()); the returned vector holds one YcFrameS16 per
// CORE frame [core_begin, core_end) (window-relative indices) — the
// overlap frames are context only. Field parity falls back to index
// order (field1 = top), the same fallback the reference uses when
// per-field metadata is unavailable; FrameParams carries none.
//
// ACC is measured over the whole window (median burst amplitude across
// its fields), matching the reference. With cfg.output_fidelity (default)
// the lossless split invariant luma + chroma == composite holds inside
// the active picture, exactly like the frame path; with it off, luma is
// the NR/anchored estimate and the invariant intentionally does not hold.
// `base_frame_id`: absolute FrameID of frames[0], used only to name the
// diagnostic maps (cfg.debug_dir) — pass 0 if unused.
std::vector<YcFrameS16> DecodeFrameSequenceWindow(
    const std::vector<const int16_t*>& frames, int core_begin, int core_end,
    const FrameParams& fp, const HvdConfig& cfg, HvdEngine& engine,
    int64_t base_frame_id = 0);

// Convenience overload for tests/one-off calls: constructs a throwaway
// engine internally. Prefer the engine-taking overload above for any
// repeated/per-sequence decoding (which is every real use in the plugin) —
// this one re-plans FFTW from scratch on every call.
YcFrameS16 DecodeFrameBuffer(const int16_t* frame, const FrameParams& fp,
                             const HvdConfig& cfg);

// For sources that are ALREADY Y/C separated at capture (host's
// has_separate_channels() == true, e.g. get_frame_luma()/get_frame_chroma()
// on an S-Video-style or hi-fi-VHS TBC): `luma` and `chroma` are two
// separate field-sequential buffers, same geometry as `frame` above.
// `chroma` is expected UNSIGNED with fp.chroma_dc as its zero point (raw
// TBC convention — see orc_source_parameters.h's chroma_dc_offset) rather
// than already centred; this function does the re-centring.
// There is no Y/C separation problem to solve here (the source solved it),
// so this is much cheaper than DecodeFrameBuffer: luma passes through
// directly, and only the chroma undergoes burst lock-in + demodulation.
// Do NOT feed such a source's get_frame() (composite) into
// DecodeFrameBuffer instead — for a Y/C source that call returns the luma
// plane with no chroma in it at all, which silently decodes to zero
// chroma.
// Same engine-reuse and thread-safety notes as DecodeFrameBuffer above.
YcFrameS16 DecodeYcFrameBuffer(const int16_t* luma, const int16_t* chroma,
                               const FrameParams& fp, const HvdConfig& cfg,
                               HvdEngine& engine);

YcFrameS16 DecodeYcFrameBuffer(const int16_t* luma, const int16_t* chroma,
                               const FrameParams& fp, const HvdConfig& cfg);

}  // namespace hvd

#endif  // LDDECODE_HVD_FRAME_BRIDGE_H_
