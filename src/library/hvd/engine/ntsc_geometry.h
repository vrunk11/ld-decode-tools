// SPDX-License-Identifier: GPL-3.0-or-later
//
// ntsc_geometry.h — NTSC 4fsc signal geometry and level conversions.
//
// Groups the small, dependency-free helpers that describe how a time-base
// corrected NTSC field maps onto the holographic model used by the decoder:
//
//   S(x, y) = Y(x, y) + Re[ chi(x, y) * exp(i * phi(x, y)) ],   chi = V - iU
//
// with the carrier phase phi(line, x) = theta[line] + (pi/2) * x  (90 deg per
// sample at 4fsc). These mirror `VideoParameters` and the phase/noise helpers in
// `reference/hvd/decoder.py` and `reference/hvd/tbc.py`.

#ifndef LDDECODE_HVD_ENGINE_NTSC_GEOMETRY_H_
#define LDDECODE_HVD_ENGINE_NTSC_GEOMETRY_H_

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/plane.h"

namespace hvd {

// NTSC 4fsc colour subcarrier and sample rate (Hz). fsc = 315e6 / 88.
constexpr double kFscNtsc = 315.0e6 / 88.0;      // 3 579 545.45... Hz
constexpr double kFs4Fsc = 4.0 * kFscNtsc;        // 14 318 181.8 Hz

// PAL 4fsc: fsc = 4 433 618.75 Hz, i.e. 283.7516 cycles/line -- NOT 283.75.
// The stored grid is 1135 samples/line but the true line is 1135.0064, so
// the carrier advances 270.576 deg per stored line (see
// FieldGeometry::line_advance, which carries the derivation and the
// measurement that motivated it). The lock-in still MEASURES the per-line
// phase; the model only has to be close enough that the deviation it
// smooths stays well inside WrapPi's +/-180 (the PAL reference in
// reference-pal/, THEORY-PAL.md sections 1-2).
constexpr double kFscPal = 4433618.75;
constexpr double kFs4FscPal = 4.0 * kFscPal;      // 17 734 475 Hz

// PAL-M (ITU-R BT.1700-1 Annex 1 Part B): 525 lines, 909 samples/line at
// 4fsc, fsc = (909/4) * fH. That is 227.25 cycles/line, i.e. the carrier
// advances 90 deg per line -- NOT the 180 deg of NTSC nor the 270 deg of
// 625-line PAL. The chroma is PAL (swinging burst + V-switch); only the
// line advance and the raster differ. Levels are the NTSC ones (7.5 IRE
// setup, 20 IRE burst).
constexpr double kFscPalM = (909.0 / 4.0) * (525.0 * 30000.0 / 1001.0);
constexpr double kFs4FscPalM = 4.0 * kFscPalM;

// Which composite standard a field's carrier obeys. This is the ONLY
// switch the numerical core dispatches on: everything downstream of the
// carrier builder (init, arbitration, temporal equations, anchor loop)
// is generic in the effective carrier c — NTSC is the special case
// c = exp(i*phi); the PAL family folds the V-switch into c (see
// MakeCarrierPal).
//
// kPalM is a first-class member rather than an alias of either neighbour:
// it shares PAL's V-switch and swinging burst but NTSC's raster and
// levels, so aliasing it to kNtsc (which is what is_pal = false used to
// do) silently decoded PAL-M chroma with no V-switch and a 180 deg/line
// carrier model instead of 90 deg/line.
enum class VideoStandard { kNtsc, kPal, kPalM };

// Composite geometry needed by the engine, expressed in the sample domain of a
// single field. Populated by the SDK layer from decode-orc SourceParameters (or
// by tests directly). All indices are 0-based sample / line positions.
struct FieldGeometry {
  int field_width = 910;          // samples per line (full, incl. sync/blanking)
  int field_height = 263;         // lines per field (incl. VBI)
  int active_video_start = 134;   // first active-picture sample (inclusive)
  int active_video_end = 894;     // one past last active-picture sample
  int colour_burst_start = 78;    // burst window start sample (inclusive)
  int colour_burst_end = 110;     // burst window end sample (exclusive)
  int first_active_field_line = 21;  // first active line within a field
  int last_active_field_line = 0;    // one past last active line; 0 => field_height
  double sample_rate = kFs4Fsc;   // Hz
  VideoStandard standard = VideoStandard::kNtsc;

  // NON-STANDARD SUBCARRIER (0 => use the standard's nominal fsc).
  //
  // Exists for captures whose composite is NTSC in every respect the host
  // measures (line rate, sync, blanking, burst window, 4fsc-nominal sample
  // grid) but whose colour subcarrier is NOT fs/4 — e.g. a disc/tape format
  // with a deliberately lowered subcarrier. The sample RATE is unchanged
  // (the host still stores CVBS_U10_4FSC on the standard's grid); only the
  // carrier that rides on that grid moves.
  //
  // Nothing else in the engine reads this directly: it feeds phase_per_sample()
  // and line_advance() below, and every equation downstream is generic in the
  // effective carrier c. See docs for what does NOT follow automatically.
  double subcarrier_hz = 0.0;

  // True when the standard carries a PAL V-switch (and therefore a swinging
  // burst): 625-line PAL and PAL-M. This -- not `standard == kPal` -- is
  // what every V-switch dispatch in the engine must test.
  bool uses_vswitch() const {
    return standard == VideoStandard::kPal || standard == VideoStandard::kPalM;
  }

  // Effective colour subcarrier frequency (Hz).
  double fsc() const {
    if (subcarrier_hz > 0.0) return subcarrier_hz;
    switch (standard) {
      case VideoStandard::kPal:
        return kFscPal;
      case VideoStandard::kPalM:
        return kFscPalM;
      default:
        return kFscNtsc;
    }
  }

  // Carrier phase advance per SAMPLE (rad).
  //
  // When no non-standard subcarrier is declared this returns exactly pi/2 as
  // a CONSTANT, and deliberately does not recompute it from fsc()/sample_rate.
  // The stored grid IS 4fsc by definition of the source format, so pi/2 is
  // the definition rather than a measurement — and callers legitimately build
  // FieldGeometry values whose sample_rate is not physically meaningful (the
  // engine tests set standard = kPal while leaving sample_rate at its NTSC
  // default). Deriving unconditionally silently handed those callers a wrong
  // carrier. Derivation happens only when the caller has explicitly opted in
  // by setting subcarrier_hz, and is then their responsibility to feed with a
  // real sample_rate.
  double phase_per_sample() const {
    if (subcarrier_hz <= 0.0) return 0.5 * 3.14159265358979323846;
    return 2.0 * 3.14159265358979323846 * subcarrier_hz / sample_rate;
  }

  // Per-line carrier phase advance on the stored grid.
  //
  // Standard subcarrier: the tabulated 180 deg (NTSC, 227.5 cycles/line) or
  // 270 deg (PAL, 283.75 cycles/line), for the same reason as above —
  // field_width is a real samples-per-line only in production geometries,
  // whereas tests set it to whatever their synthetic plane is wide.
  //
  // Non-standard subcarrier: derived as cycles/line = fsc * field_width /
  // sample_rate. This is diagnostic — several assumptions in this engine (the
  // 2-line leak cancellation, the same-parity 180 deg flip) hold only at
  // 180 deg, so a value far from it is a warning about the source.
  double line_advance() const {
    if (subcarrier_hz <= 0.0) {
      switch (standard) {
        case VideoStandard::kPal:
          // 270.576 deg, NOT 270. EBU Tech. 3280-E Table 1: fsc = 283.7516 x
          // fH, not 283.75 -- the 25 Hz offset. On the stored 1135-sample
          // grid the true line is 1135.0064 samples, so the carrier advances
          // 1135.0064 * 90 deg = 270.576 deg per STORED line. (The 4 extra
          // samples per frame that EBU puts on lines 312 and 624 close the
          // 625 * 0.0064 = 4.0 sample gap over a frame; FrameLineOffset
          // already indexes them, but the PHASE MODEL here still has to
          // carry the drift WITHIN each field.)
          //
          // The old 1.5 * pi left a 0.576 deg/line ramp in the lock-in's
          // deviation d = wrap(theta_meas - model), which accumulates to
          // ~176 deg between the first burst-bearing line and the end of the
          // field -- i.e. it sat just under WrapPi's +/-180 boundary.
          // Measured on a synthetic PAL field (313 lines, VBI without burst,
          // 64 initial phases): residual phase error 4.8 deg with 1.5 * pi
          // against 0.7 deg here, and with 2 IRE of noise the ramp crossed
          // pi on ~1 field in 64 -- wrapping d by 360 deg, which the
          // tridiagonal smoother then spread across the field AND which
          // flipped the V-switch parity vote (355 deg of phase error on that
          // field). This constant removes the ramp, and with it the cliff.
          return 2.0 * 3.14159265358979323846 * 0.7516;
        case VideoStandard::kPalM:
          return 0.5 * 3.14159265358979323846;  // 90 deg, 227.25 cyc/line
        default:
          return 3.14159265358979323846;        // 180 deg, 227.5 cyc/line
      }
    }
    const double cycles =
        subcarrier_hz * static_cast<double>(field_width) / sample_rate;
    const double frac = cycles - std::floor(cycles);
    return 2.0 * 3.14159265358979323846 * frac;
  }
  // Nominal burst amplitude for ACC, in TRUE IRE (see SampleToIre below):
  // 20 IRE for NTSC-M and PAL-M (40 IRE p-p, SMPTE 170M Table 1 /
  // ITU-R BT.1700-1 Annex 1 Part B), 21.43 IRE for 625-line PAL
  // (+/-150 mV on the 700 mV white, EBU Tech. 3280-E).
  float nominal_burst_ire() const {
    return standard == VideoStandard::kPal ? 21.43F : 20.0F;
  }

  int active_width() const { return active_video_end - active_video_start; }
  int last_active_line() const {
    return last_active_field_line != 0 ? last_active_field_line : field_height;
  }
  int active_lines() const {
    return last_active_line() - first_active_field_line;
  }
};

// Convert composite samples to TRUE IRE, and back.
//
// THE REFERENCE IS BLANKING, NOT BLACK. 0 IRE is defined by the blanking
// level and 100 IRE by peak white, so one IRE is (white - blanking) / 100
// codes. The setup pedestal (NTSC-M: black sits 7.5 IRE above blanking;
// PAL/PAL-M: black == blanking) is PART of the luma signal, not part of the
// scale.
//
// This used to be referenced to black — (sample - black) / ((white - black)
// / 100) — which on NTSC-M made one internal unit 5.18 codes instead of the
// true 5.60, i.e. every internal value came out 1.081x too large. That is
// invisible on PAL (black == blanking) and invisible in any round trip
// (the same wrong scale undoes it), but it corrupted every place an
// ABSOLUTE IRE reference is used:
//
//   * BurstAmplitudeIre measured a nominal 20 IRE burst as 21.62, so the
//     ACC applied a permanent 0.925 gain -> 7.5 % desaturation on NTSC;
//   * the exported u/v planes came out on a 5.18 codes/IRE scale, while
//     orc::ComponentFrame and orc::ColourFrameCarrier both specify chroma
//     on the composite scale, i.e. (cvbs_white - cvbs_blanking) / 100.
//
// Both are fixed at the source by referencing blanking here. Note the
// consequence downstream: on NTSC-M, picture black is now 7.5 IRE (not 0)
// in the engine's domain, which is exactly what colour.h's YuvToRgb16
// already assumed via its `black_ire` parameter.

// Codes per 1 IRE.
inline float CodesPerIre(float blanking_level, float white_level) {
  return (white_level - blanking_level) / 100.0F;
}

//   ire = (sample - blanking) / ((white - blanking) / 100)
inline float SampleToIre(float sample, float blanking_level,
                         float white_level) {
  return (sample - blanking_level) / CodesPerIre(blanking_level, white_level);
}

//   sample = blanking + ire * ((white - blanking) / 100)
inline float IreToSample(float ire, float blanking_level, float white_level) {
  return blanking_level + ire * CodesPerIre(blanking_level, white_level);
}

// Build the per-sample carrier phase map phi over the ACTIVE picture, given the
// per-line phase offsets theta (already restricted to the active lines) and the
// field geometry. Result has one row per entry in `theta` and `active_width()`
// columns. Matches `phase_map(...)[:, a0:a1]` in the reference.
ComplexPlane MakeCarrier(const std::vector<float>& theta, const FieldGeometry& g);

// PAL EFFECTIVE CARRIER (the load-bearing result of the PAL port,
// THEORY-PAL.md section 1). With one global chroma phasor chi = V - iU
// and the V-switch parity s[line] (+1 unswitched / -1 switched),
//
//   S = Y + Re[chi * c],   c = +exp(+i*phi) where s = +1
//                              -exp(-i*phi) where s = -1
//
// so the V-switch is a conjugated reference wave folded into a
// unit-modulus per-pixel carrier, and every equation of this engine
// applies unchanged. `vswitch` has one entry per row of `theta`.
ComplexPlane MakeCarrierPal(const std::vector<float>& theta,
                            const std::vector<int8_t>& vswitch,
                            const FieldGeometry& g);

// Apply HvdConfig::chroma_phase_deg to a burst-locked phase trajectory.
//
// `theta` and `vswitch` are per-line and must be the same length on the PAL
// family; `vswitch` is ignored (and may be empty) on NTSC. The offset is
// negated because the lock-in recovers (signal phase - LO phase), and it is
// signed by the V-switch sense because MakeCarrierPal conjugates the carrier
// on switched lines -- without that, the net rotation of the recovered
// phasor alternates in sign line to line (Hanover bars) at every angle other
// than 0 and 180. Shared by the frame path (engine.cpp) and the field
// sequence path (sequence.cpp) so the two can never drift apart.
inline void ApplyChromaPhase(float chroma_phase_deg, const FieldGeometry& g,
                             const std::vector<int8_t>& vswitch,
                             std::vector<float>* theta) {
  if (chroma_phase_deg == 0.0F || theta == nullptr) return;
  const float offset =
      -chroma_phase_deg * 3.14159265358979323846F / 180.0F;
  const bool signed_by_parity = g.uses_vswitch() &&
                                vswitch.size() == theta->size();
  for (size_t i = 0; i < theta->size(); ++i) {
    (*theta)[i] += signed_by_parity && vswitch[i] < 0 ? -offset : offset;
  }
}

// Robust per-field noise estimate (IRE), from the stride-4 horizontal second
// difference. At 4fsc the carrier completes 360 deg over 4 samples, so
// S[x] - 2 S[x+4] + S[x+8] cancels chroma AND smooth luma exactly, leaving
// noise. The 25th percentile of |centered d| is outlier-proof against sparse
// luma detail. Used to auto-calibrate the (future) temporal gates.
float EstimateNoiseIre(const Plane& s);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_NTSC_GEOMETRY_H_
