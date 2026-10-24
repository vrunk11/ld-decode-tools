// SPDX-License-Identifier: GPL-3.0-or-later

#include "frame_bridge.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <thread>

#ifdef _OPENMP
#include <omp.h>
#endif
#include <cmath>

#include "engine/engine.h"
#include "engine/colour.h"
#include "engine/lockin.h"
#include <fstream>
#include <cstdio>

namespace hvd {

namespace {

int16_t ClampU10(float sample) {
  const long r = std::lround(sample);
  return static_cast<int16_t>(std::clamp<long>(r, 0, 1023));
}

// The chroma channel of a Y/C representation is a SIGNED, zero-centred
// oscillation, not an unsigned 10-bit code: the host's NTSC decoder
// (Comb::FrameBuffer::splitIQ_YC) demodulates chroma_buffer[h] directly, with
// no DC term subtracted first. So chroma must swing symmetrically about 0, and
// must NOT be clamped to [0, 1023] — that would destroy every negative
// half-cycle. Clamp to the signed 10-bit-magnitude range instead.
int16_t ClampS10(float sample) {
  const long r = std::lround(sample);
  return static_cast<int16_t>(std::clamp<long>(r, -1023, 1023));
}

}  // namespace

FieldGeometry FieldGeometryFromParams(const FrameParams& fp) {
  FieldGeometry g;
  g.field_width = fp.frame_width;
  g.field_height = fp.field1_lines;  // the larger field bounds the geometry
  g.active_video_start = fp.active_video_start;
  g.active_video_end = fp.active_video_end;
  g.colour_burst_start = fp.colour_burst_start;
  g.colour_burst_end = fp.colour_burst_end;
  // Active-line indices use the woven-frame convention: field_line = frame/2.
  g.first_active_field_line = fp.first_active_frame_line / 2;
  g.last_active_field_line =
      fp.last_active_frame_line != 0 ? fp.last_active_frame_line / 2 : 0;
  g.sample_rate = fp.sample_rate;
  g.subcarrier_hz = fp.subcarrier_hz;
  g.standard = fp.standard;
  return g;
}

void SplitFrameFields(const int16_t* frame, const FrameParams& fp,
                      FieldInput* top, FieldInput* bottom,
                      bool field1_is_bottom) {
  const int fw = fp.frame_width;
  const int fh = fp.frame_height;
  const int f1 = fp.field1_lines;
  const int f2 = fh - f1;  // field 2 line count (262 NTSC)
  const int field_h = f1;  // the larger field bounds the geometry

  top->samples = Plane(field_h, fw);
  bottom->samples = Plane(field_h, fw);
  // NOTE on naming: `top`/`bottom` here mean "buffer block 1"/"block 2"
  // (temporal order). The SPATIAL role is what these flags carry — the
  // engine's weave honours is_first_field ("true => top/even rows"), so
  // for BFF material block 1 is flagged as NOT-first and the engine swaps.
  top->is_first_field = !field1_is_bottom;
  bottom->is_first_field = field1_is_bottom;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < field_h; ++y) {
    // Per-line offsets, NOT a flat y * fw stride: PAL's non-orthogonal
    // lines make the two differ by 2 samples across the field boundary.
    // See FrameParams::extra_sample_lines.
    const int64_t off_top = FrameLineOffset(fp, y);
    const int64_t off_bot = FrameLineOffset(fp, f1 + y);
    for (int x = 0; x < fw; ++x) {
      const float st = (y < f1) ? static_cast<float>(frame[off_top + x])
                                : fp.blanking_level;
      const float sb = (y < f2) ? static_cast<float>(frame[off_bot + x])
                                : fp.blanking_level;
      // TRUE IRE: referenced to blanking, not black -- see SampleToIre.
      top->samples.at(y, x) =
          SampleToIre(st, fp.blanking_level, fp.white_level);
      bottom->samples.at(y, x) =
          SampleToIre(sb, fp.blanking_level, fp.white_level);
    }
  }
}


int DetectFieldParity(const FieldInput& block1, const FieldInput& block2,
                      const FrameParams& fp) {
  const FieldGeometry g = FieldGeometryFromParams(fp);
  const int fal = g.first_active_field_line;
  const int lal = g.last_active_line();
  const int a0 = fp.active_video_start;
  const int a1 = fp.active_video_end;
  const int lines = lal - fal;
  const int width = a1 - a0;
  if (lines < 8 || width < 32) return 0;

  // Low-pass + decimate horizontally so the 4fsc chroma carrier (pi/2
  // rad/px) can't masquerade as vertical structure: average groups of 4
  // samples (a null-adjacent comb for the carrier), sampled sparsely —
  // this is a vote, not a decode; a few thousand taps are plenty.
  const int xstep = 4;
  const int ystep = std::max(1, lines / 120);
  double e_plus = 0.0, e_minus = 0.0;
  long n = 0;
  for (int r = fal + 1; r + 1 < lal; r += ystep) {
    for (int x = a0; x + 4 <= a1; x += 4 * xstep) {
      auto lp = [&](const Plane& p, int row) {
        return 0.25F * (p.at(row, x) + p.at(row, x + 1) + p.at(row, x + 2) +
                        p.at(row, x + 3));
      };
      const float b = lp(block2.samples, r);
      const float a_c = lp(block1.samples, r);
      const float a_up = lp(block1.samples, r - 1);
      const float a_dn = lp(block1.samples, r + 1);
      // block1 = top: block2 row r sits at frame line 2r+1, i.e. BETWEEN
      // block1 rows r and r+1 -> interp at +0.5.  block1 = bottom: block2
      // row r sits at frame line 2r, between block1 rows r-1 and r ->
      // interp at -0.5.
      e_plus += std::fabs(b - 0.5F * (a_c + a_dn));
      e_minus += std::fabs(b - 0.5F * (a_c + a_up));
      ++n;
    }
  }
  if (n == 0) return 0;
  e_plus /= static_cast<double>(n);
  e_minus /= static_cast<double>(n);
  // Relative margin: flat content gives e_plus ~= e_minus (both tiny) —
  // no vote rather than a coin flip.
  const double lo = std::min(e_plus, e_minus);
  const double hi = std::max(e_plus, e_minus);
  if (hi <= 1e-9 || (hi - lo) / hi < 0.02) return 0;
  return e_plus < e_minus ? +1 : -1;
}

bool ResolveField1IsBottom(const FieldInput& block1, const FieldInput& block2,
                           const FrameParams& fp, const HvdConfig& cfg) {
  if (cfg.field_order == 1) return false;
  if (cfg.field_order == 2) return true;
  const int vote = DetectFieldParity(block1, block2, fp);
  // No confident vote: the ld-decode format convention (frames assembled
  // first-field-first, first field = even frame lines) is the default.
  return vote < 0;
}

YcFrameS16 DecodeFrameBuffer(const int16_t* frame, const FrameParams& fp,
                             const HvdConfig& cfg, HvdEngine& engine) {
  const int fw = fp.frame_width;
  const int fh = fp.frame_height;
  const int f1 = fp.field1_lines;
  const FieldGeometry g = FieldGeometryFromParams(fp);

  FieldInput top;     // field 1 = flat lines [0, f1)
  FieldInput bottom;  // field 2 = flat lines [f1, fh)
  SplitFrameFields(frame, fp, &top, &bottom);
  {
    // Per-frame resolution is safe against flicker by construction: the
    // detector only abstains (falling back to the format convention) when
    // the frame has no vertical detail — exactly the content on which the
    // two weaves are visually identical, so a momentary fallback cannot be
    // observed. The sequence path additionally majority-votes per window.
    const bool f1_bottom = ResolveField1IsBottom(top, bottom, fp, cfg);
    top.is_first_field = !f1_bottom;
    bottom.is_first_field = f1_bottom;
  }

  const FrameYc yc = engine.DecodeFrame(top, bottom, g, cfg);


  // --- Re-weave into a field-sequential Y/C split --------------------------
  YcFrameS16 out;
  out.width = fw;
  out.height = fh;
  out.chroma_dc = fp.chroma_dc;
  out.luma.assign(static_cast<size_t>(fw) * fh, 0);
  out.chroma.assign(static_cast<size_t>(fw) * fh, 0);
  out.u_plane.assign(static_cast<size_t>(fw) * fh, 0.0);
  out.v_plane.assign(static_cast<size_t>(fw) * fh, 0.0);

  // Default: pass the composite through as luma, chroma at zero. Outside the
  // active picture there is no colour information to carry. Copied line by
  // line through FrameLineOffset (not a flat memcpy) so PAL's
  // non-orthogonal lines stay aligned with the decoded active region
  // written below — see FrameParams::extra_sample_lines.
  for (int line = 0; line < fh; ++line) {
    const int64_t src = FrameLineOffset(fp, line);
    std::copy(frame + src, frame + src + fw,
              out.luma.begin() + static_cast<size_t>(line) * fw);
  }

  // Fill the active picture. The engine returns a woven plane of
  // (2 * active_field_lines) rows x active_width cols; row r belongs to field
  // (r % 2), field line = first_active_field_line + r / 2.
  const int fal = g.first_active_field_line;
  const int a0 = fp.active_video_start;
  const int active_h = yc.luma.height();
  const int active_w = yc.luma.width();
  // Codes per TRUE IRE -- (white - blanking) / 100, matching SampleToIre
  // above and the composite scale that orc::ComponentFrame /
  // orc::ColourFrameCarrier specify for chroma.
  const float scale = CodesPerIre(fp.blanking_level, fp.white_level);

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int r = 0; r < active_h; ++r) {
    const int field = r % 2;               // 0 = field 1 (top), 1 = field 2
    const int field_line = fal + (r / 2);  // line within that field
    const int flat_line = (field == 0) ? field_line : (f1 + field_line);
    if (flat_line < 0 || flat_line >= fh) continue;
    for (int c = 0; c < active_w; ++c) {
      const int flat_col = a0 + c;
      if (flat_col < 0 || flat_col >= fw) continue;
      const size_t idx = static_cast<size_t>(flat_line) * fw + flat_col;
      // Luma IRE is now blanking-referenced (NTSC-M black = 7.5 IRE),
      // so the reconstruction anchor is blanking, not black.
      out.luma[idx] = ClampU10(fp.blanking_level + yc.luma.at(r, c) * scale);
      // Zero-centred, signed — see ClampS10 above.
      out.chroma[idx] = ClampS10(yc.chroma.at(r, c) * scale);
      // Baseband chroma (chroma_phasor = V - iU), not the modulated `chroma`
      // channel above: this is what a colour preview needs directly.
      // Phase correction (cfg.chroma_phase_deg) is applied upstream, on the
      // burst-locked reference theta, before the engine solves for this
      // phasor — see engine.cpp — so no rotation happens here.
      // g = chroma_gain * acc_gain, matching reference/hvd/decoder.py's
      // `g = cfg.chroma_gain * acc_gain` (decoder.py:1490). acc_gain is
      // measured per-frame (engine.cpp, burst-amplitude ACC) and was
      // previously computed but never applied — this is the fix for that.
      // Deliberately NOT applied to `out.chroma` above: that channel must
      // stay the exact residual S - Y for the lossless split invariant
      // (luma + chroma == composite) to hold; gain only belongs on the
      // baseband preview/export path.
      const float g = cfg.chroma_gain * yc.acc_gain;
      const Complex phasor = yc.chroma_phasor.at(r, c);
      out.v_plane[idx] = g * static_cast<double>(phasor.real()) * scale;
      out.u_plane[idx] = g * static_cast<double>(-phasor.imag()) * scale;
    }
  }
  return out;
}

YcFrameS16 DecodeFrameBuffer(const int16_t* frame, const FrameParams& fp,
                             const HvdConfig& cfg) {
  HvdEngine engine;
  return DecodeFrameBuffer(frame, fp, cfg, engine);
}

YcFrameS16 DecodeYcFrameBuffer(const int16_t* luma, const int16_t* chroma,
                               const FrameParams& fp, const HvdConfig& cfg,
                               HvdEngine& engine) {
  const int fw = fp.frame_width;
  const int fh = fp.frame_height;
  const int f1 = fp.field1_lines;
  const int f2 = fh - f1;
  const FieldGeometry g = FieldGeometryFromParams(fp);
  const int field_h = g.field_height;
  const float scale = CodesPerIre(fp.blanking_level, fp.white_level);

  // --- De-weave chroma only: luma passes straight through from the raw
  //     buffer below (it's already clean, nothing to decode), so there's no
  //     need to field-split or IRE-convert it here. Chroma is re-centred:
  //     raw code minus chroma_dc, on the SAME scale as luma so ACC's
  //     burst-amplitude assumptions and chroma_gain stay meaningful across
  //     both decode paths. -----------------------------------------------
  FieldInput chroma_top, chroma_bot;
  chroma_top.samples = Plane(field_h, fw);
  chroma_bot.samples = Plane(field_h, fw);
  // Auto detection needs luma vertical detail, which the C channel lacks;
  // auto therefore resolves to the format convention here, and the forced
  // modes pass through.
  chroma_top.is_first_field = cfg.field_order != 2;
  chroma_bot.is_first_field = cfg.field_order == 2;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int y = 0; y < field_h; ++y) {
    for (int x = 0; x < fw; ++x) {
      // Unsigned raw code -> signed, zero-mean, same numeric scale as IRE.
      const float ct = (y < f1)
                           ? static_cast<float>(chroma[FrameLineOffset(fp, y) + x])
                           : fp.chroma_dc;
      const float cb =
          (y < f2)
              ? static_cast<float>(chroma[FrameLineOffset(fp, f1 + y) + x])
              : fp.chroma_dc;
      chroma_top.samples.at(y, x) = (ct - fp.chroma_dc) / scale;
      chroma_bot.samples.at(y, x) = (cb - fp.chroma_dc) / scale;
    }
  }

  const FrameYc yc = engine.DecodeChromaOnly(chroma_top, chroma_bot, g, cfg);

  YcFrameS16 out;
  out.width = fw;
  out.height = fh;
  out.chroma_dc = fp.chroma_dc;
  out.luma.assign(static_cast<size_t>(fw) * fh, 0);
  out.chroma.assign(static_cast<size_t>(fw) * fh, 0);
  out.u_plane.assign(static_cast<size_t>(fw) * fh, 0.0);
  out.v_plane.assign(static_cast<size_t>(fw) * fh, 0.0);

  // Luma is already clean: pass the source's own Y channel through as-is,
  // outside the active picture included (nothing to decode either way).
  //
  // LINE BY LINE THROUGH FrameLineOffset, NOT ONE FLAT std::copy. This used
  // to be `std::copy(luma, luma + fw * fh, out.luma.begin())`, which is only
  // correct when the source buffer has a uniform fw stride. It does not on
  // 625-line PAL: the source frame is kPalFrameSamples = 709 379 samples,
  // with 2 extra samples on each of frame-flat lines 312 and 624
  // (FrameParams::extra_sample_lines). YcFrameS16's own planes ARE flat
  // fw * fh (every consumer, including the stage's ReorderToWoven, indexes
  // them as flat_line * fw + flat_col), so the conversion between the two
  // layouts has to happen HERE — a flat read of a non-flat buffer put the
  // WHOLE of field 2 two samples early, i.e. a 2 px horizontal shift on
  // every other display line, and 2 samples at 4fsc is also 180 deg of
  // subcarrier.
  //
  // It also left luma misregistered against this function's own chroma,
  // which was already being read correctly through FrameLineOffset just
  // above. The composite path (DecodeFrameBuffer) always did this properly;
  // only the Y/C path regressed.
  //
  // Still not an omp loop: 625 std::copy calls of 1135 samples each are
  // memory-bandwidth-bound with no per-element work, so thread launch
  // overhead would dwarf the actual copying.
  for (int line = 0; line < fh; ++line) {
    const int64_t src = FrameLineOffset(fp, line);
    std::copy(luma + src, luma + src + fw,
              out.luma.begin() + static_cast<size_t>(line) * fw);
  }
  std::fill(out.chroma.begin(), out.chroma.end(), 0);

  const int fal = g.first_active_field_line;
  const int a0 = fp.active_video_start;
  const int active_h = yc.chroma_phasor.height();
  const int active_w = yc.chroma_phasor.width();

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int r = 0; r < active_h; ++r) {
    const int field = r % 2;
    const int field_line = fal + (r / 2);
    const int flat_line = (field == 0) ? field_line : (f1 + field_line);
    if (flat_line < 0 || flat_line >= fh) continue;
    for (int c = 0; c < active_w; ++c) {
      const int flat_col = a0 + c;
      if (flat_col < 0 || flat_col >= fw) continue;
      const size_t idx = static_cast<size_t>(flat_line) * fw + flat_col;
      // out.luma[idx] already set above from the source's clean Y channel —
      // do NOT overwrite it with anything derived from yc (yc.luma is
      // meaningless here, see engine.h's DecodeChromaOnly doc comment).
      out.chroma[idx] = ClampS10(yc.chroma.at(r, c) * scale);
      const float gain = cfg.chroma_gain * yc.acc_gain;
      const Complex phasor = yc.chroma_phasor.at(r, c);
      out.v_plane[idx] = gain * static_cast<double>(phasor.real()) * scale;
      out.u_plane[idx] = gain * static_cast<double>(-phasor.imag()) * scale;
    }
  }
  return out;
}

YcFrameS16 DecodeYcFrameBuffer(const int16_t* luma, const int16_t* chroma,
                               const FrameParams& fp, const HvdConfig& cfg) {
  HvdEngine engine;
  return DecodeYcFrameBuffer(luma, chroma, fp, cfg, engine);
}

namespace {

// ---- Selective 3D (reference decode_sequence_selective) ---

struct SelBox {
  int fy0 = 0, fy1 = 0, x0 = 0, x1 = 0;  // FIELD coordinates
  bool valid = false;
};

// Bounding box of the most Y/C-ambiguous tiles of one decoded 2D field.
// Faithful port of _ambiguous_bbox: score = sqrt(luma_HF x chroma_HF)
// per tile (the §19 proxy, r=0.60 against measured per-tile 3D gain);
// threshold 4x median (fixed quantiles flag noise on uniform content);
// >=2-of-8-neighbor density filter (one stray tile inflates the single
// bbox to the whole frame); count-normalised box smoothing (zero-padded
// smoothing manufactures HF along the first row/column and flags it).
// Field geometry, so tile = 16 rows (~ the reference's 32 frame rows).
std::vector<SelBox> AmbiguousBoxes(const Plane& Y, const ComplexPlane& chi,
                                   float max_area) {
  constexpr int kTile = 16;
  constexpr int kHaloX = 48;
  // vertical halo only needs to cover motion search (mc_search 16) plus
  // margin — field geometry, so 16 field rows ~ 32 frame rows.
  constexpr int kHaloY = 16;
  const int h = Y.height();
  const int w = Y.width();
  const int th = (h + kTile - 1) / kTile;
  const int tw = (w + kTile - 1) / kTile;
  const int k = kTile / 2;

  // Count-normalised separable box mean.
  auto box2d = [&](const std::vector<float>& a, std::vector<float>* out) {
    std::vector<float> tmp(static_cast<size_t>(h) * w, 0.0F);
    for (int x = 0; x < w; ++x) {
      for (int y = 0; y < h; ++y) {
        float acc = 0.0F;
        int n = 0;
        for (int d = -k / 2; d < k - k / 2; ++d) {
          const int yy = y + d;
          if (yy < 0 || yy >= h) continue;
          acc += a[static_cast<size_t>(yy) * w + x];
          ++n;
        }
        tmp[static_cast<size_t>(y) * w + x] = acc / std::max(n, 1);
      }
    }
    out->assign(static_cast<size_t>(h) * w, 0.0F);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        float acc = 0.0F;
        int n = 0;
        for (int d = -k / 2; d < k - k / 2; ++d) {
          const int xx = x + d;
          if (xx < 0 || xx >= w) continue;
          acc += tmp[static_cast<size_t>(y) * w + xx];
          ++n;
        }
        (*out)[static_cast<size_t>(y) * w + x] = acc / std::max(n, 1);
      }
    }
  };

  std::vector<float> ya(static_cast<size_t>(h) * w);
  std::vector<float> ca(static_cast<size_t>(h) * w);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      ya[static_cast<size_t>(y) * w + x] = Y.at(y, x);
      ca[static_cast<size_t>(y) * w + x] = std::abs(chi.at(y, x));
    }
  std::vector<float> lpY;
  std::vector<float> lpC;
  box2d(ya, &lpY);
  box2d(ca, &lpC);

  std::vector<double> texY(static_cast<size_t>(th) * tw, 0.0);
  std::vector<double> texC(static_cast<size_t>(th) * tw, 0.0);
  std::vector<int> cnt(static_cast<size_t>(th) * tw, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const size_t t =
          static_cast<size_t>(y / kTile) * tw + (x / kTile);
      const double dy = ya[i] - lpY[i];
      const double dc = ca[i] - lpC[i];
      texY[t] += dy * dy;
      texC[t] += dc * dc;
      ++cnt[t];
    }
  std::vector<float> score(static_cast<size_t>(th) * tw, 0.0F);
  for (size_t t = 0; t < score.size(); ++t)
    score[t] = static_cast<float>(
        std::sqrt((texY[t] / std::max(cnt[t], 1)) *
                  (texC[t] / std::max(cnt[t], 1))));

  std::vector<float> sorted = score;
  const size_t mid = sorted.size() / 2;
  std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
  const float med = sorted[mid];
  const float thr = 4.0F * med;

  std::vector<char> flagged(score.size(), 0);
  for (size_t t = 0; t < score.size(); ++t) flagged[t] = score[t] >= thr;
  // density filter: >= 2 of 8 flagged neighbors
  std::vector<char> dense(score.size(), 0);
  for (int ty = 0; ty < th; ++ty)
    for (int tx = 0; tx < tw; ++tx) {
      if (!flagged[static_cast<size_t>(ty) * tw + tx]) continue;
      int nb = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (dy == 0 && dx == 0) continue;
          const int yy = ty + dy;
          const int xx = tx + dx;
          if (yy < 0 || yy >= th || xx < 0 || xx >= tw) continue;
          nb += flagged[static_cast<size_t>(yy) * tw + xx];
        }
      // outer 1-tile border ring excluded: border tiles straddle the
      // capture-mask transition (black pillars, head-switch area) and
      // are never useful ROI targets; a flagged border stripe otherwise
      // bridges or widens every band (measured on real capture).
      const bool border =
          ty == 0 || ty == th - 1 || tx == 0 || tx == tw - 1;
      dense[static_cast<size_t>(ty) * tw + tx] = !border && nb >= 2;
    }
  // Row-banded boxes, one per vertical band of flagged tile-rows,
  // NOT a single hull: the field-reported case is thin horizontal
  // artifact zones at the TOP, MIDDLE and BOTTOM of the frame, whose
  // common hull is ~the whole frame -> silent 2D fallback while full
  // 3D fixes the zones. max_area caps the TOTAL area of all bands.
  std::vector<SelBox> boxes;
  std::vector<std::pair<int, int>> bands;
  int band_start = -1, band_prev = -1;
  for (int ty = 0; ty < th; ++ty) {
    // >= 2 flagged tiles to count as a band row: a one-tile-wide
    // vertical stripe (real capture: the black side-pillar edge flags
    // column 0 top to bottom) otherwise bridges every artifact band
    // into one giant one that overflows the area cap.
    int row_n = 0;
    for (int tx = 0; tx < tw; ++tx)
      row_n += dense[static_cast<size_t>(ty) * tw + tx];
    const bool any = row_n >= 2;
    if (any) {
      if (band_start < 0) band_start = ty;
      else if (ty - band_prev > 1) {
        bands.push_back({band_start, band_prev});
        band_start = ty;
      }
      band_prev = ty;
    }
  }
  if (band_start >= 0) bands.push_back({band_start, band_prev});
  if (bands.empty()) {
    // No localized peaks: disambiguate uniform-CLEAN from uniform-
    // AMBIGUOUS with an absolute level. Measured medians (IRE^2, tile
    // 16, field geometry): SMPTE chart 0.62, flat noise sigma=2.5 1.00,
    // real LD footage with frame-wide blinds 5.66, diffuse photo
    // texture 8.87. Above 3.0 the frame is globally ambiguous and
    // deserves FULL 3D, not the 2D fallback; a purely relative
    // threshold cannot tell these two apart.
    if (med > 3.0F) boxes.push_back(SelBox{});  // sentinel: full 3D
    return boxes;
  }
  constexpr size_t kMaxBoxes = 4;
  while (bands.size() > kMaxBoxes) {
    size_t bi = 0;
    int best = INT_MAX;
    for (size_t i = 0; i + 1 < bands.size(); ++i) {
      const int gap = bands[i + 1].first - bands[i].second;
      if (gap < best) { best = gap; bi = i; }
    }
    bands[bi].second = bands[bi + 1].second;
    bands.erase(bands.begin() + bi + 1);
  }
  double total = 0.0;
  for (const auto& [tr0, tr1] : bands) {
    // One box PER contiguous column run (gap <= 2), single-tile runs
    // dropped, overlapping halo-expanded runs merged. History: v18's
    // min..max span let one stray tile widen a band to full width; the
    // first fix kept only the LONGEST run, which on the real capture
    // cut off the right half of the circled sill artifact (its band
    // held runs at ~x256-576 and ~x560-720; only the first survived) --
    // user-visible symptom: "selective precisely avoids my artifacts".
    std::vector<char> colf(static_cast<size_t>(tw), 0);
    for (int ty = tr0; ty <= tr1; ++ty)
      for (int tx = 0; tx < tw; ++tx)
        colf[tx] |= dense[static_cast<size_t>(ty) * tw + tx];
    const int y0raw = std::max(0, tr0 * kTile - kHaloY);
    const int y0 = y0raw - (y0raw % 2);
    const int y1 = std::min(h, (tr1 + 1) * kTile + kHaloY);
    std::vector<std::pair<int, int>> runs;
    int r0 = -1, rprev = -1;
    for (int tx = 0; tx < tw; ++tx) {
      if (!colf[tx]) continue;
      if (r0 < 0) { r0 = rprev = tx; continue; }
      if (tx - rprev <= 2) { rprev = tx; continue; }
      runs.push_back({r0, rprev});
      r0 = rprev = tx;
    }
    if (r0 >= 0) runs.push_back({r0, rprev});
    size_t band_first = boxes.size();
    for (const auto& [c0, c1] : runs) {
      if (c1 - c0 < 1) continue;  // single-tile stray
      const int x0 = std::max(0, c0 * kTile - kHaloX);
      const int x1 = std::min(w, (c1 + 1) * kTile + kHaloX);
      if (boxes.size() > band_first && x0 <= boxes.back().x1) {
        boxes.back().x1 = x1;     // merge halo-overlapping runs
      } else {
        SelBox b;
        b.fy0 = y0;
        b.fy1 = y1;
        b.x0 = x0;
        b.x1 = x1;
        b.valid = true;
        boxes.push_back(b);
      }
    }
  }
  total = 0.0;
  for (const auto& b : boxes)
    total += static_cast<double>(b.fy1 - b.fy0) * (b.x1 - b.x0);
  if (total > static_cast<double>(max_area) * h * w) {
    // overflow sentinel: one INVALID box = "ambiguity everywhere, run
    // full 3D" (distinct from empty = "clean, stay 2D").
    boxes.clear();
    boxes.push_back(SelBox{});
  }
  return boxes;
}

FieldObs CropObs(const FieldObs& f, const SelBox& b) {
  FieldObs c;
  c.parity = f.parity;
  c.s = Plane(b.fy1 - b.fy0, b.x1 - b.x0);
  c.carrier = ComplexPlane(b.fy1 - b.fy0, b.x1 - b.x0);
  for (int y = b.fy0; y < b.fy1; ++y)
    for (int x = b.x0; x < b.x1; ++x) {
      c.s.at(y - b.fy0, x - b.x0) = f.s.at(y, x);
      c.carrier.at(y - b.fy0, x - b.x0) = f.carrier.at(y, x);
    }
  return c;
}

// Full-window 2D + cropped full-3D, feather-blended at field level.
std::vector<DecodedField> DecodeWindowSelective(
    const std::vector<FieldObs>& fields, const FieldGeometry& g,
    const HvdConfig& cfg, HvdEngine& engine, SequenceDiagnostics* diag) {
  // 2D base: strength < 0 is the driver's explicit "3D OFF" convention
  // (0 would mean adaptive), which also short-circuits passes/anchor and
  // the whole motion machinery — this is the cheap full-frame decode.
  HvdConfig c2d = cfg;
  c2d.temporal_strength = -1.0F;
  std::vector<DecodedField> dec =
      engine.DecodeSequenceWindow(fields, g, c2d, diag);
  if (fields.empty()) return dec;

  const std::vector<SelBox> boxes =
      AmbiguousBoxes(dec[0].luma, dec[0].chroma, cfg.selective_max_area);
  if (boxes.empty()) return dec;  // nothing flagged: clean, stay 2D
  if (!boxes[0].valid) {
    // ambiguity present but too widespread to crop profitably: full 3D,
    // never 2D. Selective's contract is "at least full-3D quality,
    // cheaper when possible" -- the earlier 2D fallback here is exactly
    // how real footage with a border stripe ended up with its artifact
    // zones untreated.
    return engine.DecodeSequenceWindow(fields, g, cfg, nullptr);
  }

  // Boxes are independent, so process them concurrently rather than one
  // at a time. NOT naive thread-per-box: the engine already parallelises
  // internally per pixel row (OpenMP, holographic_init.cpp/motion.cpp/
  // sequence.cpp) — stacking a second, unbounded layer of threads on top
  // would oversubscribe the machine and could easily come out SLOWER, not
  // faster. Instead each box's OWN OpenMP team is capped to hardware
  // concurrency / box count, so the total thread count across all
  // concurrently-running boxes stays bounded to what the machine has. A
  // plain std::thread (not an OpenMP thread) is free to set its own
  // omp_set_num_threads default, so this needs no change to the engine
  // itself. Genuine win specifically when a box's row count is too small
  // to keep the full core count busy on its own (the common case here:
  // artifact bands are a handful of tile-rows tall) — the machine was
  // otherwise sitting partly idle during every single-box call.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const int per_box_threads =
      std::max(1, static_cast<int>(hw) / std::max<int>(1, boxes.size()));
  std::vector<std::vector<DecodedField>> dec3_all(boxes.size());
  {
    std::vector<std::thread> workers;
    workers.reserve(boxes.size());
    for (size_t bi = 0; bi < boxes.size(); ++bi) {
      workers.emplace_back([&, bi]() {
#ifdef _OPENMP
        omp_set_num_threads(per_box_threads);
#endif
        std::vector<FieldObs> crop;
        crop.reserve(fields.size());
        for (const FieldObs& f : fields) crop.push_back(CropObs(f, boxes[bi]));
        dec3_all[bi] = engine.DecodeSequenceWindow(crop, g, cfg, nullptr);
      });
    }
    for (auto& t : workers) t.join();
  }

  for (size_t bi = 0; bi < boxes.size(); ++bi) {
    const SelBox& box = boxes[bi];
    const std::vector<DecodedField>& dec3 = dec3_all[bi];

    const int bh = box.fy1 - box.fy0;
    const int bw = box.x1 - box.x0;
    const int hy = std::min(16, bh / 3);
    const int hx = std::min(48, bw / 3);
    for (size_t j = 0; j < dec.size(); ++j) {
      for (int y = 0; y < bh; ++y) {
        float ry = 1.0F;
        if (y < hy)
          ry = static_cast<float>(y) / hy;
        else if (y >= bh - hy)
          ry = static_cast<float>(bh - 1 - y) / hy;
        for (int x = 0; x < bw; ++x) {
          float rx = 1.0F;
          if (x < hx)
            rx = static_cast<float>(x) / hx;
          else if (x >= bw - hx)
            rx = static_cast<float>(bw - 1 - x) / hx;
          const float m = ry * rx;
          float& Yd = dec[j].luma.at(box.fy0 + y, box.x0 + x);
          Complex& Cd = dec[j].chroma.at(box.fy0 + y, box.x0 + x);
          Yd = Yd * (1.0F - m) + dec3[j].luma.at(y, x) * m;
          Cd = Cd * (1.0F - m) + dec3[j].chroma.at(y, x) * m;
        }
      }
    }
  }
  return dec;
}

}  // namespace

std::vector<DecodedField> DecodeFieldsSelective(
    const std::vector<FieldObs>& fields, const FieldGeometry& g,
    const HvdConfig& cfg, HvdEngine& engine, SequenceDiagnostics* diag) {
  return DecodeWindowSelective(fields, g, cfg, engine, diag);
}

std::vector<YcFrameS16> DecodeFrameSequenceWindow(
    const std::vector<const int16_t*>& frames, int core_begin, int core_end,
    const FrameParams& fp, const HvdConfig& cfg, HvdEngine& engine,
    int64_t base_frame_id) {
  std::vector<YcFrameS16> out;
  const int nframes = static_cast<int>(frames.size());
  if (nframes == 0 || core_begin < 0 || core_end > nframes ||
      core_begin >= core_end) {
    return out;
  }
  const int fw = fp.frame_width;
  const int fh = fp.frame_height;
  const int f1 = fp.field1_lines;
  const FieldGeometry g = FieldGeometryFromParams(fp);
  const float scale = CodesPerIre(fp.blanking_level, fp.white_level);

  // --- Prepare the window's fields (2 per frame) + measure ACC ------------
  // Parity: field1 = top (parity 0). FrameParams carries no per-field
  // metadata, so index order is the fallback — same as the reference's
  // field_parity when isFirstField isn't available.
  std::vector<FieldObs> fields;
  fields.reserve(static_cast<size_t>(nframes) * 2);
  std::vector<float> burst_amps;
  burst_amps.reserve(static_cast<size_t>(nframes) * 2);
  std::vector<std::pair<FieldInput, FieldInput>> field_pairs;
  field_pairs.reserve(nframes);
  for (int t = 0; t < nframes; ++t) {
    FieldInput top;
    FieldInput bottom;
    SplitFrameFields(frames[t], fp, &top, &bottom);
    burst_amps.push_back(BurstAmplitudeIre(top.samples, g));
    burst_amps.push_back(BurstAmplitudeIre(bottom.samples, g));
    field_pairs.push_back({std::move(top), std::move(bottom)});
  }
  // Resolve the field order ONCE for the whole window (majority of the
  // per-frame signal votes; a window must be spatially consistent — a
  // per-frame flip would be a weave error, not a property of the capture).
  bool f1_bottom = cfg.field_order == 2;
  if (cfg.field_order == 0) {
    int vote = 0;
    for (const auto& pr : field_pairs)
      vote += DetectFieldParity(pr.first, pr.second, fp);
    f1_bottom = vote < 0;
  }
  const int p_first = f1_bottom ? 1 : 0;
  for (auto& pr : field_pairs) {
    // TEMPORAL order preserved (block 1 first, as stored); the field order
    // only changes which spatial PARITY each block carries. The output
    // weave downstream places content by TRUE parity, so it handles
    // p0 = 1 natively.
    fields.push_back(PrepareFieldObs(pr.first, g, cfg, /*parity=*/p_first));
    fields.push_back(PrepareFieldObs(pr.second, g, cfg, /*parity=*/1 - p_first));
  }

  float acc_gain = 1.0F;
  if (cfg.acc && !burst_amps.empty()) {
    const size_t k = burst_amps.size() / 2;
    std::nth_element(burst_amps.begin(), burst_amps.begin() + k,
                     burst_amps.end());
    const float med = burst_amps[k];
    // AccGain(), not an inlined copy of it, and g.nominal_burst_ire(),
    // NOT a hardcoded 20.
    //
    // This is the DEFAULT composite path (the frame path below is the
    // last-resort fallback), and it was comparing every standard's
    // measured burst against the NTSC nominal. On 625-line PAL, whose
    // nominal burst is 21.43 IRE, that is 20 / 21.43 = 0.933 -- a
    // permanent 6.7 % desaturation on every PAL decode, on top of
    // whatever the source chain actually did. The frame path called
    // AccGain(..., g.nominal_burst_ire()) correctly all along, so the
    // two paths disagreed with each other as well.
    acc_gain = AccGain(med, g.nominal_burst_ire());
  }
  const float gain = cfg.chroma_gain * acc_gain;

  // --- The pipeline itself -------------------------------------------------
  SequenceDiagnostics diag;
  std::vector<DecodedField> dec;
  if (cfg.selective_3d && cfg.cg_iterations > 0 &&
      cfg.temporal_strength >= 0.0F) {
    dec = DecodeWindowSelective(fields, g, cfg, engine, &diag);
  } else {
    dec = engine.DecodeSequenceWindow(fields, g, cfg, &diag);
  }

  // ---- diagnostic maps (cfg.debug_dir non-empty) ---------------------------
  if (!cfg.debug_dir.empty()) {
    // Per-chunk decision log.
    {
      std::ofstream log(cfg.debug_dir + "/diag.txt", std::ios::app);
      log << "frames " << (base_frame_id + core_begin) << ".."
          << (base_frame_id + core_end - 1) << ": f1_bottom=" << f1_bottom
          << " sigma=" << diag.sigma_ire << " amb=" << diag.ambiguity_ire
          << " strength=" << diag.resolved_strength
          << " eps_t=" << diag.temporal_eps << " nr_eps=" << diag.nr_eps
          << " acc=" << acc_gain << "\n";
    }
    // Per exported frame: woven map of residual chroma-in-luma (the
    // visible rainbow, measured). NOT a plain demod of Y — that lights up
    // on any legitimate luma texture near f_sc (fine folds, fan grilles,
    // small text) even when separation succeeded, which sent the first
    // artifact hunt after ghosts. The discriminator is parity physics:
    // between same-parity fields the carrier has flipped, so legitimate
    // static luma flips sign in the demod and CANCELS in the pair SUM,
    // while residual chroma-in-Y (a correlated separation error) is
    // coherent and survives. Triangle-7 demod; 0..8 IRE -> 0..255.
    for (int t = core_begin; t < core_end; ++t) {
      const int lines = dec[2 * t].luma.height();
      const int aw = dec[2 * t].luma.width();
      const int nfields = static_cast<int>(dec.size());
      std::vector<uint8_t> img(static_cast<size_t>(2 * lines) * aw, 0);
      for (int half = 0; half < 2; ++half) {
        const int j = 2 * t + half;
        const int j2 = (j + 2 < nfields) ? j + 2 : j - 2;  // same parity
        const int p = fields[j].parity;
        static constexpr float kW[7] = {1, 2, 3, 4, 3, 2, 1};
        for (int r = 0; r < lines; ++r) {
          for (int x = 0; x + 7 <= aw; ++x) {
            Complex dj{0.0F, 0.0F};
            Complex dk{0.0F, 0.0F};
            for (int k = 0; k < 7; ++k) {
              dj += kW[k] * dec[j].luma.at(r, x + k) *
                    std::conj(fields[j].carrier.at(r, x + k));
              if (j2 >= 0)
                dk += kW[k] * dec[j2].luma.at(r, x + k) *
                      std::conj(fields[j2].carrier.at(r, x + k));
            }
            const float e = (j2 >= 0) ? std::abs(dj + dk) / 32.0F
                                      : std::abs(dj) / 16.0F;
            const int v = std::clamp(static_cast<int>(e * 32.0F), 0, 255);
            img[static_cast<size_t>(2 * r + p) * aw + x + 3] =
                static_cast<uint8_t>(v);
          }
        }
      }
      char name[64];
      std::snprintf(name, sizeof(name), "/rainbow_%06lld.pgm",
                    static_cast<long long>(base_frame_id + t));
      std::ofstream pgm(cfg.debug_dir + name, std::ios::binary);
      pgm << "P5\n" << aw << " " << 2 * lines << "\n255\n";
      pgm.write(reinterpret_cast<const char*>(img.data()),
                static_cast<std::streamsize>(img.size()));
    }
  }

  // --- Weave core frames by parity and repackage as YcFrameS16 -------------
  const int fal = g.first_active_field_line;
  const int a0 = fp.active_video_start;
  for (int t = core_begin; t < core_end; ++t) {
    const int j0 = 2 * t;
    const int j1 = j0 + 1;
    const DecodedField& d0 = dec[j0];
    const DecodedField& d1 = dec[j1];
    int p0 = fields[j0].parity;
    int p1 = fields[j1].parity;
    if (p0 == p1) {  // metadata anomaly: fall back to index order
      p0 = 0;
      p1 = 1;
    }

    YcFrameS16 yc;
    yc.width = fw;
    yc.height = fh;
    yc.chroma_dc = fp.chroma_dc;
    yc.luma.assign(static_cast<size_t>(fw) * fh, 0);
    yc.chroma.assign(static_cast<size_t>(fw) * fh, 0);
    yc.u_plane.assign(static_cast<size_t>(fw) * fh, 0.0);
    yc.v_plane.assign(static_cast<size_t>(fw) * fh, 0.0);
    // Outside the active picture: composite passthrough, zero chroma
    // (same contract as DecodeFrameBuffer).
    // Passthrough of the raw composite outside the active picture. Copy
    // line by line through FrameLineOffset so PAL's non-orthogonal lines
    // land on the same rows the decoded active region is written to; a
    // flat memcpy would shear field 2 by 2 samples relative to it.
    for (int line = 0; line < fh; ++line) {
      const int64_t src = FrameLineOffset(fp, line);
      std::copy(frames[t] + src, frames[t] + src + fw,
                yc.luma.begin() + static_cast<size_t>(line) * fw);
    }

    const int lines = d0.luma.height();
    const int active_w = d0.luma.width();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int r = 0; r < 2 * lines; ++r) {
      // Frame row r (within the active picture) shows the field whose TRUE
      // parity equals r % 2 — the anti-inverted-weave weave of the
      // reference (Yf[p0::2] = Yout[j0]). The PHYSICAL output block is a
      // separate question: YcFrameS16's field-sequential contract (and
      // ReorderToWoven downstream) maps even frame rows to the field1
      // block [0, f1) and odd rows to the field2 block [f1, fh),
      // regardless of which capture field supplied the content.
      const bool row_even = (r % 2) == 0;
      const bool use_first = (r % 2) == p0;
      const DecodedField& d = use_first ? d0 : d1;
      const int field_line = r / 2;
      if (field_line >= d.luma.height()) continue;
      const int flat_line =
          row_even ? (fal + field_line) : (f1 + fal + field_line);
      if (flat_line < 0 || flat_line >= fh) continue;
      for (int c = 0; c < active_w; ++c) {
        const int flat_col = a0 + c;
        if (flat_col < 0 || flat_col >= fw) continue;
        const size_t idx = static_cast<size_t>(flat_line) * fw + flat_col;
        const float y_ire = d.luma.at(field_line, c);
        // Monochrome: zero the chroma at OUTPUT only (the solve still ran
        // and shaped Y through the arbitration), matching the reference's
        // `Cf = np.zeros_like(Cf)` in decode_sequence.
        const Complex chi =
            cfg.monochrome ? Complex{} : d.chroma.at(field_line, c);
        const Complex car =
            fields[use_first ? j0 : j1].carrier.at(field_line, c);
        yc.luma[idx] = ClampU10(fp.blanking_level + y_ire * scale);
        // Modulated chroma residual, zero-centred/signed (see ClampS10):
        // with cfg.output_fidelity (default) luma + chroma == composite
        // holds exactly, matching the frame path's invariant.
        yc.chroma[idx] = ClampS10((chi * car).real() * scale);
        yc.v_plane[idx] = gain * static_cast<double>(chi.real()) * scale;
        yc.u_plane[idx] = gain * static_cast<double>(-chi.imag()) * scale;
      }
    }
    out.push_back(std::move(yc));
  }
  return out;
}

}  // namespace hvd
