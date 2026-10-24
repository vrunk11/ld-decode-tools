// SPDX-License-Identifier: GPL-3.0-or-later
//
// hvd_config.h — tunable knobs for the holographic-variational decoder.
//
// This is the C++ mirror of `DecoderConfig` in `reference/hvd/decoder.py`.
// Only the fields exercised by the ported 2-D woven-frame path are wired up in
// this first port; the 3-D / noise-reduction / drizzle fields are declared and
// documented so the temporal extensions can be added behind the existing engine
// interface without touching the SDK layer.
//
// Defaults are copied verbatim from the reference so that, for the same input,
// the C++ 2-D decode matches the Python 2-D decode within numerical tolerance.

#ifndef LDDECODE_HVD_ENGINE_HVD_CONFIG_H_
#define LDDECODE_HVD_ENGINE_HVD_CONFIG_H_

#include <string>

namespace hvd {

struct HvdConfig {
  // --- Arbitration priors (the heart of the method) -----------------------
  // lambda_c: chroma-smoothness vs luma-plausibility trade-off. Higher =
  // smoother chroma / less rainbowing; lower = sharper chroma / less dot crawl
  // pushed into luma.
  float lambda_c = 1.0F;
  // mu_h = chroma_aniso * lambda_c, mu_v = lambda_c. 0 = AUTO (default):
  // the right split is a property of the content's chroma orientation —
  // thin horizontal chroma bands / fsc-adjacent texture want the vertical
  // prior weak (-> 1.0), sharp horizontal chroma transitions (colour
  // bars) want the reference's 0.5; auto measures the init's p90 gradient
  // ratio per solve and maps it into [0.5, 1.0] (ResolveChromaAniso in
  // variational.cpp, with the measurements). Positive = forced fixed
  // value (reference behaviour at 0.5).
  float chroma_aniso = 0.0F;

  // NOT a user parameter: set from the source geometry by the engine
  // entry points (DecodeFrameBuffer / DecodeSequence set it from
  // FieldGeometry::standard, so it cannot be forgotten). It exists
  // because the AUTO chroma_aniso measurement above must cancel the
  // init's cross-colour leak before measuring, and the leak's
  // line-to-line structure DIFFERS between the standards:
  //   NTSC: conj(c) alternates sign        -> a 2-line average cancels it
  //   PAL : conj(c) alternates sign AND conjugation (the V-switch), and
  //         the carrier walks 270 deg/line -> only a 4-LINE average
  //         cancels it (the conjugation pattern repeats over 2 lines,
  //         and 4 * 270 = 1080 = 0 mod 360 closes the carrier walk).
  // Measuring PAL with the NTSC 2-line average leaves the leak in the
  // vertical gradient and makes AUTO pick a wrong aniso. See
  // reference-pal/THEORY-PAL.md section 3.
  bool is_pal = false;
  // Charbonnier edge-preservation scales (in IRE) for the luma and chroma
  // priors respectively.
  float charbonnier_eps = 0.5F;
  float chroma_eps = 1.0F;
  // Parallel-level-sets coupling: open the chroma diffusivity wherever the
  // residual luma sees an edge (kills hanging dots at vertical transitions).
  float structure_coupling = 0.25F;

  // Oriented (+/-45 deg) chroma prior weight, relative to mu_v and
  // distance-normalised by 1/2 (a diagonal step is sqrt(2) px); 0 disables.
  // Measured in the reference as a trade-off, not a win: -1.0 dB on
  // axis-aligned sharp chroma (SMPTE) vs +2.0 dB on diagonal cross-colour
  // torture (zoneplate) — hence off by default; a documented dial for
  // diagonal-artifact-heavy material (fine weaves, venetian blinds). The
  // horizontal/vertical/diagonal weights are renormalised together so the
  // TOTAL chroma prior mass is unchanged: the oriented terms redistribute
  // smoothing across directions, they don't add more of it. (THEORY 9e.)
  float diag_prior = 0.0F;

  // --- Solver budget ------------------------------------------------------
  // Total conjugate-gradient iterations across all IRLS outer passes.
  // 0 => pure holographic reconstruction (fast preview, no refinement).
  // Default kept small (2, not the reference's higher-quality values):
  // this is what a fresh/never-configured stage decodes with, and a
  // small number here means a first-run/live-preview/quick-test decode
  // is fast by default rather than accidentally expensive; raise it
  // explicitly once you're tuning for final quality.
  int cg_iterations = 2;
  // Number of IRLS (lagged-diffusivity) outer re-weightings.
  int irls_outer = 4;
  // CG relative gradient-norm early-exit: stop an inner CG loop once
  // ||g||^2 < tol^2 * ||g0||^2. 0 = auto (0.02 slow / 0.10 fast), matching
  // the reference's cg_tol. Iteration counts stay the CEILING; this lets
  // already-converged solves return early instead of burning the budget.
  float cg_tol = 0.0F;
  // FAST MODE (THEORY 9f — written as the optimisation spec for this port):
  // same algorithm, cheaper logistics. In this engine it currently wires up
  // the parts the frame-level path exercises — the 2/3 CG-budget cap, the
  // looser auto cg_tol (0.10), and tile-resolution confidence maps in
  // MotionCompensatePrev (bilinear interpolation of the 24x24-ish tile map
  // instead of a full-res squared upsample + radius-8 blur; ~256x cheaper,
  // visually identical). The decode_sequence-only components (shared
  // motion cache, predicted+verified ME for long offsets, deferred
  // coherence) have their building blocks ported (VerifyMotion,
  // FitTrajectory/TrajectorySnap, the MotionField* precompute hooks) and
  // activate when that pipeline lands. Reference measurement: >=2x wall
  // clock, never worse than 0.2 dB. Defaults ON (same reasoning as
  // cg_iterations above): the never-worse-than-0.2dB measurement means
  // there's little reason for a fresh/unconfigured stage to default to
  // the slower path.
  bool fast = true;

  // --- Parallelisation strategy (2D / decoupled-field decode only) --------
  // The comment in sequence.cpp's decode loop claiming "one field per core
  // beats one memory-bound loop across all cores" (i.e. parallelise ACROSS
  // fields, forcing each field's own internal OpenMP loops serial via
  // OpenMP's no-nested-parallelism default) predates any real profiling on
  // this project's actual hardware -- it's a plausible-sounding claim, not
  // a measured one. This flag makes it possible to A/B the two strategies
  // without recompiling:
  //   true  (default, unchanged behaviour): parallelise ACROSS fields
  //         (nf/cores fields decode concurrently, each single-threaded
  //         internally). Wins when nf (fields in the chunk -- see
  //         chunk_frames) is comparable to or above the core count.
  //   false: decode fields ONE AT A TIME, same shape as the GUI preview's
  //         single-frame decode -- each field's own internal OpenMP loops
  //         (variational.cpp etc.) are then free to use every core, since
  //         nothing outer is already inside a parallel region. Likely
  //         wins when nf is well below the core count (e.g. small
  //         chunk_frames on a high-core-count machine), where the "true"
  //         setting would otherwise leave cores idle.
  // Only affects the 2D/fast decoupled-field path (sequence.cpp); the
  // slow coupled-temporal path was already serial across fields for
  // correctness (Gauss-Seidel data dependencies), not performance, and
  // is unaffected either way.
  bool parallel_across_fields = true;

  // --- Holographic init bandwidths (sideband crop) ------------------------
  // NOTE: vestigial in the REFERENCE too — its holographic_init overrides
  // these at every call site (dataclasses.replace with 0.8/120, 1.8/30 for
  // the two Dubois variants and 1.3/60 for the symmetry certifier), so the
  // user-facing values are never consumed there either. Kept declared for
  // config-surface parity; the C++ init hardcodes the same three pairs.
  float init_lpf_h_mhz = 1.3F;    // horizontal chroma bandwidth (MHz)
  float init_lpf_v_cph = 60.0F;   // vertical bandwidth (cycles / picture height)
  // Enable the spectral-symmetry ("Transform NTSC, repaired") init variant.
  bool symmetry_variant = false;

  // --- Colour / levels ----------------------------------------------------
  // NOTE: no ntsc_j flag here. The Python reference has one because it
  // works from raw TBC files with no host calibration; in this plugin,
  // orc::SourceParameters::black_level already reflects the real measured
  // pedestal for this specific capture (0 IRE for NTSC-J, 7.5 IRE for
  // standard NTSC — see orc_source_parameters.h's black_level_override /
  // has_nonstandard_values), and frame_bridge.cpp already uses it
  // throughout. Re-applying a black-level shift here would double-correct
  // NTSC-J sources and silently corrupt standard NTSC-M ones.
  // Automatic Color Control: calibrate saturation from the measured burst
  // amplitude (nominal 20 IRE), as every analogue TV does.
  bool acc = true;
  float chroma_gain = 1.0F;
  bool monochrome = false;
  // Chroma phase correction, in degrees, applied to the burst-locked phase
  // reference (theta) BEFORE it's used to build the carrier — same idea as
  // Comb::Configuration::chromaPhase in the classic decoder (comb.cpp's
  // transformIQ, theta = (33 + chromaPhase) * pi/180), except here it's
  // injected at the actual phase-reference stage instead of rotating U/V
  // after the fact.
  //
  // DEFAULT IS NOW 0, NOT 180. The old 180 existed solely to cancel a sign
  // error in the NTSC burst derivation (lockin.cpp: theta = arg(z) + pi/2
  // where the correct relation is arg(z) - pi/2). That is fixed at source,
  // so this control is once again what it claims to be: a per-capture trim,
  // free for the user to spend.
  //
  // Worse, the old default was actively WRONG on PAL. BurstLockinPhasePal
  // derives theta from the swinging burst independently and was already
  // correct, so the global 180 rotated every 625-line PAL decode by half a
  // turn. Anyone who had dialled this back to 0 to make PAL look right must
  // now leave it at 0 for both standards.
  //
  // On the PAL family the rotation is applied with the V-switch sign (see
  // engine.cpp / sequence.cpp): MakeCarrierPal conjugates the carrier on
  // switched lines, so a naive theta += delta rotates the recovered phasor
  // by -delta on one line parity and +delta on the other — alternating hue
  // error, i.e. Hanover bars, for every value except 0 and 180. Signing the
  // offset by the parity makes the trim uniform and usable at any angle.
  float chroma_phase_deg = 0.0F;

  // --- Non-standard subcarrier -------------------------------------------
  // Some composite sources are NTSC in every respect the host measures
  // (line rate, sync, blanking, burst window, 4fsc-nominal sample grid)
  // but carry the colour subcarrier somewhere other than fs/4. The sample
  // RATE does not change; only the carrier riding on that grid moves.
  // The motivating case is JVC VHD, whose subcarrier sits at 2 556 818 Hz.
  //
  // custom_subcarrier is the on/off switch, subcarrier_hz the value, kept
  // separate for the same reason enable_temporal and temporal_strength are:
  // so a dialled-in frequency survives toggling the feature off and on.
  //
  // TYPE IS double, NOT float, unlike every other numeric field here. float's
  // ULP at 4.4 MHz is 0.25 Hz, coarser than the control feeding this, so a
  // float would silently swallow individual spinbox steps and could not hold
  // an exact standard fsc. Everything downstream (FrameParams::subcarrier_hz,
  // FieldGeometry::subcarrier_hz) is already double; this closes the one gap.
  //
  // UNITS ARE Hz. They used to be kHz, and that was not survivable: the host
  // renders every DOUBLE parameter with a hardcoded 4 decimal places
  // (stageparameterdialog.cpp, setDecimals(4)) and no setSingleStep, which a
  // plugin cannot override. In kHz that means
  //   * 4 decimals = 0.0001 kHz, so PAL's 4433.61875 kHz — which needs FIVE —
  //     was literally not typeable; the operator got 4433.6187 or 4433.6188,
  //     i.e. the standard's own subcarrier was unreachable through the
  //     control whose entire job is to reach a subcarrier;
  //   * the default single step of 1.0 stepped by a whole kHz per click,
  //     useless for the "sweep it while watching a flat colour area"
  //     workflow the descriptor recommends.
  // In Hz the same hardcoded 4 decimals give 0.0001 Hz of typing resolution
  // and the 1.0 step becomes 1 Hz per click. Reference values in these units:
  // VHD 2556818.2, NTSC 3579545.5, PAL 4433618.75.
  //
  // MIGRATION: the stage's set_parameters() still accepts the old
  // "subcarrier_khz" key and multiplies by 1000, so projects saved before
  // this change keep their dialled-in frequency. get_parameters() only ever
  // emits the new "subcarrier_hz" key, so a project rewrites itself on the
  // first save.
  //
  // WHAT STILL HOLDS AT VHD. 2 556 818.2 Hz is exactly 162.5 * fH —
  // an ODD MULTIPLE OF HALF THE LINE RATE, structurally the same choice as
  // standard NTSC's 227.5 * fH. So line_advance() comes out at 180 deg and
  // the two assumptions that depend on it stay valid: the AUTO chroma_aniso
  // measurement (its 2-line average still cancels the init's cross-colour
  // leak) and the sequence path's ambiguity measurement (same-parity fields
  // still see the carrier flipped 180 deg). The adaptive modes can be left
  // alone here. The default below is the EXACT line lock 2 556 818.2 (180
  // deg/line); a rounded 2 556 800 gives 179.584 deg/line and quietly
  // breaks both assumptions above. The GUI descriptor used to default to
  // the rounded value while this struct held the exact one — two sources
  // of truth for one default, which is the same class of bug as the old
  // chroma_phase_deg = 180. tests/stage_smoke_test.cpp now cross-checks
  // every descriptor default against this struct so it cannot recur.
  //
  // WHAT DOES NOT. Two carrier nulls are built on fs/fsc == 4 and VHD is at
  // fs/fsc = 5.6, so they stop being nulls:
  //   1. EstimateNoiseIre's stride-4 second difference no longer cancels
  //      chroma; it OVER-estimates sigma, loosening the auto-calibrated
  //      temporal/NR gates.
  //   2. The box-4 low-pass in DetectFieldParity (frame_bridge.cpp) and in
  //      the sequence path's ambiguity gate likewise.
  // Both are fixable exactly for VHD if it proves to matter: 5.6 = 28/5, so
  // 28 samples span exactly 5 carrier cycles — a stride-28 second difference
  // and a box-28 low-pass are exact nulls, at the cost of a wider kernel
  // (28 samples ~= 2 us) passing less luma curvature. Not done here.
  //
  // For a subcarrier that is NOT an odd multiple of fH/2, check
  // FieldGeometry::line_advance(): if it is not ~180 deg, the two assumptions
  // above are actively wrong for the source and the adaptive modes should be
  // replaced by forced values (chroma_aniso 0.5, a fixed temporal_strength).
  bool custom_subcarrier = false;
  double subcarrier_hz = 2556818.2;  // VHD, the exact 162.5 * fH line lock

  // --- Geometry -----------------------------------------------------------
  // Weave both fields into frame geometry before decoding (default, best
  // quality on static material). false => legacy per-field decode.
  bool frame_decode = true;

  // --- Engine performance (not in the Python reference — this only exists
  // because of how differently a single C++ process schedules threads
  // compared to a one-shot numpy script) ------------------------------------
  // DEAD as a user-facing knob: no longer exposed in the GUI parameter
  // list. This build links single-threaded fftw3f (see fft2d.cpp — a
  // threaded fftw3f dragged in a second OpenMP runtime, for a speed gain
  // that measured as negligible next to the already-threaded IRLS/CG
  // solver), so FFTW itself never actually threads its own transforms
  // regardless of this value. Kept only so HvdEngine::SetFftThreads()
  // (still called with this default) and Fft2d::SetThreadCount() keep
  // compiling; remove together if that plumbing is ever cleaned up too.
  int fft_threads = 4;

  // --- 3-D / temporal --------------------------------------------------
  // Frame-level neighbour extension (decode_frame's own 3D mode in the
  // reference — NOT decode_sequence's separate, field-granularity chunked
  // pipeline, which is a richer but much larger alternative architecture
  // deferred for later). Wired into HvdEngine::
  // DecodeFrame via an optional list of previous WOVEN FRAMES' raw state
  // (luma/composite/carrier), motion-compensated with MotionCompensatePrev
  // and fed to VariationalRefine's neighbours parameter.
  //
  // enable_temporal is the actual on/off switch — NOT in the Python
  // reference, which just uses temporal_strength == 0 as "off" (no
  // separate toggle needed there since it's a one-shot script, not a UI
  // with a value you want to keep dialled in while flipping 3D on/off).
  // Decoupling them means temporal_strength can default to an actually-
  // useful working value instead of 0, without that turning 3D on by
  // itself — this default (unlike every other value in this file) is NOT
  // from the reference, since the reference has no equivalent "on but at
  // a sensible strength" state; treat it as a starting point to tune, not
  // a verified-correct constant.
  // FIELD ORDER. The TBC stores fields in TEMPORAL order; for a
  // well-formed ld-decode .tbc the FORMAT already fixes the spatial
  // mapping (frames are assembled first-field-first and the first field
  // carries the even frame lines) — so there is nothing to guess. The
  // host API surface, however, exposes no per-field isFirstField, so
  // rather than trusting an assumption across every host/capture, AUTO
  // (default) MEASURES the order from the signal itself: under the true
  // order, each field's lines interpolate the other field's at +0.5 line,
  // under the inverted order at -0.5 — a deterministic half-line vertical
  // correlation test (DetectFieldParity in frame_bridge), decided per
  // frame with a relative margin and falling back to the format
  // convention (field 1 = top) when content has no vertical detail to
  // vote with. 1/2 force the order manually (diagnosis: with the wrong
  // order, static horizontal edges serrate one line and motion combs even
  // through a player's deinterlacer).
  // DIAGNOSTIC MAPS. When non-empty, the sequence export writes, per
  // exported frame, a PGM map of the RESIDUAL CARRIER-BAND ENERGY in the
  // decoded luma — i.e. the rainbow/dot-crawl the eye sees, measured
  // (triangle-7 demod of Y by the field's own carrier, woven, 0..8 IRE
  // mapped to 0..255) — plus a per-chunk diag.txt with the decoder's
  // decisions (resolved adaptive strength, measured ambiguity, noise,
  // gates, field-order vote). Exists because artifact reports that don't
  // reproduce synthetically need the decoder's view of the USER's own
  // content: send the map of a bad zone instead of describing it.
  std::string debug_dir;
  int field_order = 0;  // 0 = auto (measured), 1 = field 1 top, 2 = field 1 bottom
  // (There is no frame-decode option: the source is interlaced by
  // definition, and weaving before decoding contaminates the separation
  // across time — THEORY section 5 calls it "a failure". The composite
  // pipeline is FIELD-granularity everywhere; the frame-weave core
  // survives only as an internal last-resort fallback.)
  bool enable_temporal = false;
  // 0 = ADAPTIVE (default): the right strength is a property of the
  // CONTENT, not a constant — measured repeatedly in this project: on
  // Y/C-ambiguous content (luma energy at the subcarrier: fine detail,
  // cross-colour) the neighbour equations resolve what 2D cannot and
  // want to be strong; on unambiguous content they can only lift chroma
  // noise (the |dc|^2 leverage rebalances data vs prior) and want to be
  // weak. Auto measures the ambiguity per window from the phase physics
  // itself: demodulate S by the carrier per field; between same-parity
  // fields (carrier flipped 180 deg) true chroma is COHERENT while
  // luma-leak flips sign, so (d_j - d_{j+2})/2 isolates the ambiguous
  // energy (plus noise, subtracted via the same sigma the other gates
  // use). Mapped to [0.15, 1.5] around the reference's --3d value (0.5).
  // Any positive value forces that fixed strength (reference behaviour);
  // enable_temporal remains the on/off switch either way.
  float temporal_strength = 0.0F;
  bool bidirectional = true;       // declared for decode_sequence's richer
                                    // path; DecodeFrame only ever sees PAST
                                    // frames (a future frame isn't available
                                    // yet when decoding sequentially)
  int passes = 2;  // reference CLI default: pass 2+ engages the anchor                  // ditto — Gauss-Seidel passes, only
                                    // meaningful for the chunked pipeline
  // 0 => auto-calibrate from the measured composite noise (clip(7*sigma,
  // 4, 20), same formula decode_sequence itself uses) via
  // EstimateNoiseIre — NOT decode_frame's own literal Python (which just
  // uses cfg.temporal_eps as given, no auto-cal at that level): a fixed 0
  // default would otherwise make every neighbour weight collapse to ~0
  // silently (wt = conf*eps_t^2/(rt^2+eps_t^2) degenerates when eps_t==0),
  // exactly the "declared but does nothing" trap this project has hit
  // more than once already — auto-calibrating avoids reintroducing it here.
  float temporal_eps = 0.0F;
  float nr_anchor = 1.0F;
  float nr_eps = 0.0F;
  int nr_radius = 2;
  bool drizzle = false;
  int mc_tile = 32;    // block-matching tile size (px)
  int mc_search = 16;  // block-matching search radius (px)

  // decode_sequence-only fields (the field-granularity chunked pipeline,
  // not yet ported). Declared with the reference's
  // own defaults so the config surface is complete and ready, but nothing
  // reads these yet; DecodeFrame's simpler frame-level 3D mode has no
  // concept of "extended" neighbour offsets (its neighbour list is just
  // whatever the caller passes) or of chunking/coherence gating at all.
  //
  // trajectory_fit (THEORY 9e): fit ONE per-tile velocity across all the
  // sequence pipeline's temporal offsets (six noisy measurements of one
  // physical motion) and snap agreeing pairwise vectors onto k*v under
  // consensus; disagreement is preserved (occlusion/acceleration is
  // signal). The engine primitives (FitTrajectory/TrajectorySnap in
  // motion.h) are ported and unit-tested; this flag gates their use once
  // the multi-offset pipeline exists. A single-neighbour DecodeFrame has
  // only one offset, so there is no trajectory to fit yet.
  bool trajectory_fit = true;
  bool extended_temporal = true;  // decode_sequence: also use fields f±3
  float coherence_gate = 0.6F;
  // Floor of the half-line envelope gate on ODD (opposite-parity) temporal
  // offsets. The gate is wt *= max(odd_gate_floor, eps_t^2/(eps_t^2 + b^2)),
  // so this is the fraction of an equation's weight that survives even where
  // the envelope says the opposite-parity field CANNOT see the feature.
  // 0.35 is the reference's compromise: hard-gating cost it 4.4 dB of 3-D
  // gain on step-edge-heavy content (saturated charts), where the odd
  // equations are biased but still informative. That compromise inverts on
  // content made ENTIRELY of sub-frame-line detail — hair, fur, fine fabric,
  // blinds — where every odd equation is voting on a feature its own field
  // never sampled, and the surviving 35% is 35% of a known-wrong answer with
  // a small (hence ungated) residual. Lower it toward 0 for such material;
  // 0 disables the odd offsets wherever vertical structure is present, which
  // costs 3-D gain on flat/edge content but removes the failure mode
  // outright. Content-dependent by nature — hence a dial, not a constant.
  float odd_gate_floor = 0.35F;
  int chunk_frames = 6;
  // Frames of temporal CONTEXT added on each side of an export chunk when
  // 3D is on (see decode_sequence_chunk_and_write_rgb24) -- and, since
  // this field is now the single shared source of truth for BOTH paths,
  // also the size of the preview's mini-3D window (id +/- chunk_overlap;
  // was hardcoded to +/-1 there before). Default 1 => a 3-frame window
  // (n-1, n, n+1): the 2D/3D difference is small enough that this is
  // plenty, and it keeps the preview and the export doing the SAME
  // amount of temporal work per frame by construction, rather than the
  // preview using a fixed, separately-hardcoded window while export
  // scaled with this value -- which was the actual reason "3D looked
  // fine in preview but was heavy on export" felt inconsistent.
  int chunk_overlap = 1;
  // Selective 3D (reference: decode_sequence_selective):
  // full-window 2D decode + the complete 3D machinery re-run on a crop of
  // the most Y/C-ambiguous tiles only, feather-blended in. Pays off on
  // LOCALIZED ambiguity (fan grilles, blinds, one textured region in a
  // flat scene): ~87% of the 3D rainbow fix at ~69% of full-3D time in
  // the reference measurement — and better global PSNR than full 3D
  // there, because the flat majority keeps the 2D solve. On diffuse
  // content the detector finds no worthwhile box and the window degrades
  // to plain 2D by design (the §19 measurement: no 30% crop captures
  // more than ~half the gain on diffuse scenes). Off by default.
  bool selective_3d = false;
  float selective_max_area = 0.45F;  // fall back to 2D above this fraction
  bool output_fidelity = true;
  bool psi_init = false;
};

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_HVD_CONFIG_H_
