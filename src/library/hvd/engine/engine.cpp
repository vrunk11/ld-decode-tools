// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/colour.h"
#include "engine/fft2d.h"
#include "engine/holographic_init.h"
#include "engine/lockin.h"
#include "engine/motion.h"
#include "engine/temporal.h"
#include "engine/variational.h"

namespace hvd {

namespace {

constexpr float kPi = 3.14159265358979323846F;

// Extract the active picture of one field: lines
// [first_active_field_line, last_active_line) x columns
// [active_video_start, active_video_end).
Plane ActivePicture(const Plane& field, const FieldGeometry& g) {
  const int fal = g.first_active_field_line;
  const int lal = g.last_active_line();
  const int a0 = g.active_video_start;
  const int a1 = g.active_video_end;
  const int lines = std::max(0, lal - fal);
  const int width = std::max(0, a1 - a0);
  Plane out(lines, width);
  for (int y = 0; y < lines; ++y)
    for (int x = 0; x < width; ++x) out.at(y, x) = field.at(fal + y, a0 + x);
  return out;
}

// Shared by DecodeFrame and DecodeChromaOnly: order the two fields (top on
// even rows), weave their active pictures into frame geometry, lock onto
// burst phase per field, apply the chroma-phase correction, and build the
// carrier. `s` is the woven active picture (composite for DecodeFrame,
// chroma-only for DecodeChromaOnly — the maths that follows doesn't care
// which, only the caller's interpretation of the result differs).
struct WovenFrame {
  Plane s;
  ComplexPlane carrier;
  const FieldInput* top = nullptr;
  const FieldInput* bot = nullptr;
};

WovenFrame WeaveAndBuildCarrier(const FieldInput& first, const FieldInput& second,
                                const FieldGeometry& g, const HvdConfig& cfg) {
  const FieldInput* top = &first;
  const FieldInput* bot = &second;
  if (!first.is_first_field && second.is_first_field) std::swap(top, bot);

  // Standard dispatch happens HERE and only here: NTSC's plain lock-in
  // vs PAL's joint phase+parity lock-in. Everything after the carrier is
  // built is standard-agnostic (generic in the effective carrier c).
  std::vector<float> theta_top;
  std::vector<float> theta_bot;
  std::vector<int8_t> sw_top;
  std::vector<int8_t> sw_bot;
  if (g.uses_vswitch()) {
    PalBurstLockin lt = BurstLockinPhasePal(top->samples, g);
    PalBurstLockin lb = BurstLockinPhasePal(bot->samples, g);
    theta_top = std::move(lt.theta);
    theta_bot = std::move(lb.theta);
    sw_top = std::move(lt.vswitch);
    sw_bot = std::move(lb.vswitch);
  } else {
    theta_top = BurstLockinPhase(top->samples, g);
    theta_bot = BurstLockinPhase(bot->samples, g);
  }

  const Plane s_top = ActivePicture(top->samples, g);
  const Plane s_bot = ActivePicture(bot->samples, g);

  const int lines = s_top.height();
  const int width = s_top.width();
  const int fal = g.first_active_field_line;

  WovenFrame w;
  w.top = top;
  w.bot = bot;
  w.s = Plane(2 * lines, width);
  std::vector<float> theta(2 * lines, 0.0F);
  std::vector<int8_t> vswitch(2 * lines, 1);
  for (int y = 0; y < lines; ++y) {
    for (int x = 0; x < width; ++x) {
      w.s.at(2 * y, x) = s_top.at(y, x);
      w.s.at(2 * y + 1, x) = s_bot.at(y, x);
    }
    theta[2 * y] = theta_top[fal + y];
    theta[2 * y + 1] = theta_bot[fal + y];
    if (g.uses_vswitch()) {
      vswitch[2 * y] = sw_top[fal + y];
      vswitch[2 * y + 1] = sw_bot[fal + y];
    }
  }

  // Chroma phase correction: rotate the burst-locked reference itself
  // (same spirit as Comb::FrameBuffer::transformIQ's `theta = (33 +
  // chromaPhase) * pi/180`), so the solver below decomposes the composite
  // against an already-corrected carrier instead of us rotating chi
  // afterwards. The recovered phasor's phase moves opposite to the
  // reference's (lock-in convention: recovered phase = signal phase - LO
  // phase), hence the minus sign.
  //
  // THE PARITY SIGN IS LOAD-BEARING ON THE PAL FAMILY. MakeCarrierPal
  // builds c = -exp(-i*phi) on V-switched lines, so adding a constant
  // delta to theta rotates the recovered chi by -delta on unswitched lines
  // and by +delta on switched ones: an alternating hue error, i.e. Hanover
  // bars, for every delta except 0 and 180 (where +delta == -delta). The
  // old code did exactly that and was consequently only ever usable at its
  // 180 default. Signing the offset by the V-switch sense makes the net
  // rotation of chi uniform at chi * exp(-i*delta) on every line, so the
  // control is now a real trim at any angle on NTSC, PAL and PAL-M alike.
  ApplyChromaPhase(cfg.chroma_phase_deg, g, vswitch, &theta);

  w.carrier = g.uses_vswitch() ? MakeCarrierPal(theta, vswitch, g)
                               : MakeCarrier(theta, g);
  return w;
}

}  // namespace

HvdEngine::HvdEngine() : fft_(std::make_unique<Fft2d>()) {}
HvdEngine::~HvdEngine() = default;

std::vector<DecodedField> HvdEngine::DecodeSequenceWindow(
    const std::vector<FieldObs>& fields, const FieldGeometry& g,
    const HvdConfig& cfg, SequenceDiagnostics* diag) {
  return DecodeFieldWindow(fields, g, cfg, fft_.get(), diag);
}

void HvdEngine::SetFftThreads(int n) { fft_->SetThreadCount(n); }

FrameYc HvdEngine::DecodeFrame(const FieldInput& first, const FieldInput& second,
                               const FieldGeometry& g, const HvdConfig& cfg_in) {
  // Derive the engine-internal standard flag from the geometry so callers
  // cannot forget it (HvdConfig::is_pal is NOT a user parameter — it
  // selects the standard-correct leak cancellation in the AUTO
  // chroma_aniso measurement; see hvd_config.h).
  HvdConfig cfg = cfg_in;
  cfg.is_pal = g.uses_vswitch();

  const WovenFrame w = WeaveAndBuildCarrier(first, second, g, cfg);
  const Plane& s = w.s;
  const ComplexPlane& carrier = w.carrier;

  // Holographic init, then (optionally) variational refinement.
  HoloInit init = HolographicInit(s, carrier, g, cfg, fft_.get());
  Plane luma;
  ComplexPlane chi;
  if (cfg.cg_iterations > 0 && !cfg.monochrome) {
    // (The frame-level temporal path that used to live here — motion-
    // compensated previous-frame equations + a coherence gate — is gone:
    // the composite pipeline is FIELD-granularity everywhere now
    // (engine/sequence.h), the reference's only validated 3D. DecodeFrame
    // survives purely as the 2D frame-weave core for the last-resort
    // fallback and its tests; it takes no temporal state.)
    RefineResult r = VariationalRefine(s, carrier, init.chroma, cfg);
    luma = std::move(r.luma);
    chi = std::move(r.chroma);
  } else {
    luma = std::move(init.luma);
    chi = std::move(init.chroma);
  }

  if (cfg.monochrome) {
    for (size_t i = 0; i < chi.size(); ++i) chi[i] = Complex{0.0F, 0.0F};
    // chroma == 0 => luma must equal the composite for the split to hold.
    for (size_t i = 0; i < luma.size(); ++i) luma[i] = s[i];
  }

  // Lossless Y/C split: chroma = S - Y (== Re[chi * carrier] when not mono).
  FrameYc out;
  out.composite = s;
  out.carrier = carrier;
  out.luma = luma;
  out.chroma = Plane(s.height(), s.width());
  for (size_t i = 0; i < out.chroma.size(); ++i) out.chroma[i] = s[i] - luma[i];
  out.chroma_phasor = std::move(chi);

  // ACC gain (for the colour path only; does not touch the split above).
  if (cfg.acc) {
    const float a_top = BurstAmplitudeIre(w.top->samples, g);
    const float a_bot = BurstAmplitudeIre(w.bot->samples, g);
    out.acc_gain = AccGain(0.5F * (a_top + a_bot), g.nominal_burst_ire());
  }
  return out;
}

FrameYc HvdEngine::DecodeChromaOnly(const FieldInput& first,
                                    const FieldInput& second,
                                    const FieldGeometry& g,
                                    const HvdConfig& cfg_in) {
  // Same derivation as DecodeFrame and DecodeFieldWindow: is_pal is NOT a
  // user parameter, it is read off the geometry so callers cannot forget it
  // (hvd_config.h). Inert on this path today — DecodeChromaOnly skips the
  // variational stage, and ResolveChromaAniso is the only reader — but
  // leaving the one entry point that DOESN'T derive it is a trap for
  // whatever gets added here next.
  HvdConfig cfg = cfg_in;
  cfg.is_pal = g.uses_vswitch();

  const WovenFrame w = WeaveAndBuildCarrier(first, second, g, cfg);
  const Plane& s = w.s;  // chroma-only, signed, zero-mean — no luma mixed in
  const ComplexPlane& carrier = w.carrier;

  // No separation problem to solve (the source already separated Y from C),
  // so just the holographic bandwidth crop — no IRLS/CG arbitration needed.
  // `cfg.monochrome`/`cfg.cg_iterations` intentionally don't apply here:
  // there is no luma-vs-chroma trade-off to make on an input that's
  // already pure chroma.
  const HoloInit init = HolographicInit(s, carrier, g, cfg, fft_.get());

  FrameYc out;
  out.composite = s;
  out.carrier = carrier;
  out.luma = Plane(s.height(), s.width());  // meaningless; caller ignores it
  out.chroma_phasor = init.chroma;
  out.chroma = Plane(s.height(), s.width());
  for (size_t i = 0; i < out.chroma.size(); ++i) {
    out.chroma[i] = (out.chroma_phasor[i] * carrier[i]).real();
  }

  if (cfg.acc) {
    const float a_top = BurstAmplitudeIre(w.top->samples, g);
    const float a_bot = BurstAmplitudeIre(w.bot->samples, g);
    out.acc_gain = AccGain(0.5F * (a_top + a_bot), g.nominal_burst_ire());
  }
  return out;
}

}  // namespace hvd
