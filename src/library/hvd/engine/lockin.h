// SPDX-License-Identifier: GPL-3.0-or-later
//
// lockin.h — colour-burst phase recovery by lock-in detection.
//
// Ports `burst_lockin_phase`, `_tridiag_smooth` and `burst_amplitude_ire`
// from `reference/hvd/decoder.py`.
//
// The carrier phase is recovered per line with a lock-in amplifier on the
// colour burst (multiply by a local oscillator, integrate, read the complex
// output), then the per-line phase *trajectory* is smoothed by a weighted
// tridiagonal solve + one IRLS re-weighting. Because every downstream equation
// depends on phi, hardening it here hardens the whole decoder against real,
// damaged media (dropouts, head noise). The reference measures unchanged decode
// quality with 25 % of bursts destroyed.

#ifndef LDDECODE_HVD_ENGINE_LOCKIN_H_
#define LDDECODE_HVD_ENGINE_LOCKIN_H_

#include <cstdint>
#include <vector>

#include "engine/ntsc_geometry.h"
#include "engine/plane.h"

namespace hvd {

// Per-line subcarrier phase offset theta[line] such that
//   phi(line, x) = theta[line] + (pi/2) * x.
// `field_ire` is the full field in IRE (height x field_width). Lines with no
// detectable burst inherit the pi-per-line model phase.
std::vector<float> BurstLockinPhase(const Plane& field_ire,
                                    const FieldGeometry& g);

// Measured colour-burst amplitude (IRE), median over burst-bearing lines, for
// Automatic Color Control. Returns 20.0 (nominal) when no burst is found.
float BurstAmplitudeIre(const Plane& field_ire, const FieldGeometry& g);

// PAL swinging-burst JOINT phase + V-switch-parity estimation
// (THEORY-PAL.md section 2; reference-pal/hvd_pal/decoder.py
// burst_lockin). The burst phasor swings +/-45 deg about -U line to
// line, so one lock-in output per line carries BOTH unknowns:
//
//   z[l] ~ (B/2) exp(i*(theta[l] + pi/2 - s[l]*pi/4))
//
// mean direction -> carrier phase trajectory; sign of the alternating
// swing -> the V-switch parity, MEASURED (never derived from line
// indices, fieldPhaseID or the Bruch sequence). Parity is resolved by
// two-hypothesis amplitude-weighted scoring over all burst-bearing
// lines; the phase deviation from the 270 deg/line model is then
// smoothed by the same tridiagonal + IRLS trajectory solve as NTSC
// (which also absorbs the genuine slow drift of the 25 Hz offset).
// Reference measurement: exact parity + 0.46 deg max phase error with
// 25% of bursts destroyed.
struct PalBurstLockin {
  std::vector<float> theta;     // per-line carrier phase at sample 0
  std::vector<int8_t> vswitch;  // per-line V-switch sense, +1 / -1
};
PalBurstLockin BurstLockinPhasePal(const Plane& field_ire,
                                   const FieldGeometry& g);

// Solve (diag(a) + lam * L) x = a * d, with L the 1-D graph Laplacian, via the
// Thomas algorithm (O(n)). Exposed for unit testing. `a`, `d` have length n.
std::vector<float> TridiagSmooth(const std::vector<float>& d,
                                 const std::vector<float>& a, float lam);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_LOCKIN_H_
