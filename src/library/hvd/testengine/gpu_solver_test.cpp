// SPDX-License-Identifier: GPL-3.0-or-later
//
// The solver on a GPU (OpenCL) must give what the CPU gives: the same planes, to the rounding of the sums (they are made in float, in groups,
// on the device). The test runs the solvers of both and compares them: the 2D solve, the solve with the oriented prior, with two neighbours
// (3D), and the joint one with an anchor. Without an OpenCL GPU (or a build without OpenCL) it says so and passes: the GPU is an option.

#include <cmath>
#include <cstdio>
#include <vector>

#include "check.h"
#include "engine/ntsc_geometry.h"
#include "engine/sequence.h"
#include "engine/variational.h"
#include "gpu/gpu_solver.h"

namespace {

using hvd::Complex;
using hvd::ComplexPlane;
using hvd::FieldGeometry;
using hvd::HvdConfig;
using hvd::Plane;

struct Scene {
  Plane s;
  ComplexPlane carrier;
  ComplexPlane chi0;
  Plane y0;
};

Scene MakeScene(const FieldGeometry& g, int h, int w) {
  std::vector<float> theta(h, 0.0F);
  Scene sc;
  sc.carrier = hvd::MakeCarrier(theta, g);
  Plane y_true(h, w);
  ComplexPlane chi_true(h, w);
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      y_true.at(r, c) = 30.0F + 20.0F * std::sin(0.35F * static_cast<float>(r + c));
      const float diag = std::sin(0.4F * static_cast<float>(c - r));
      chi_true.at(r, c) = Complex{10.0F * diag, -6.0F * diag};
    }
  }
  sc.s = Plane(h, w);
  for (size_t i = 0; i < sc.s.size(); ++i) sc.s[i] = y_true[i] + (chi_true[i] * sc.carrier[i]).real();
  sc.chi0 = ComplexPlane(h, w);
  sc.y0 = Plane(h, w);
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      const float e = 3.0F * std::sin(0.9F * static_cast<float>(c)) * std::cos(0.7F * static_cast<float>(r));
      sc.chi0.at(r, c) = chi_true.at(r, c) + Complex{e, -e};
      sc.y0.at(r, c) = sc.s.at(r, c) - (sc.chi0.at(r, c) * sc.carrier.at(r, c)).real();
    }
  }
  return sc;
}

double RmsDiff(const ComplexPlane& a, const ComplexPlane& b) {
  double acc = 0.0;
  for (size_t i = 0; i < a.size(); ++i) acc += std::norm(a[i] - b[i]);
  return std::sqrt(acc / static_cast<double>(a.size()));
}

double RmsDiff(const Plane& a, const Plane& b) {
  double acc = 0.0;
  for (size_t i = 0; i < a.size(); ++i) acc += static_cast<double>(a[i] - b[i]) * (a[i] - b[i]);
  return std::sqrt(acc / static_cast<double>(a.size()));
}

// Two neighbours: the composite a little different, the carrier turned, the confidence between 0.5 and 1
std::vector<hvd::NeighborTerm> MakeNeighbours(const Scene& sc, const FieldGeometry& g, int h, int w) {
  std::vector<hvd::NeighborTerm> out;
  for (int k = 0; k < 2; ++k) {
    hvd::NeighborTerm t;
    std::vector<float> theta(h, 0.8F + 1.1F * static_cast<float>(k));
    t.carrier = hvd::MakeCarrier(theta, g);
    t.composite = Plane(h, w);
    t.confidence = Plane(h, w);
    for (int r = 0; r < h; ++r) {
      for (int c = 0; c < w; ++c) {
        t.composite.at(r, c) = sc.s.at(r, c) + 1.5F * std::sin(0.31F * static_cast<float>(r * (k + 1) + c));
        t.confidence.at(r, c) = 0.5F + 0.5F * std::fabs(std::sin(0.13F * static_cast<float>(r + c + k)));
      }
    }
    out.push_back(std::move(t));
  }
  return out;
}


// A window of fields decoded on the CPU and with the window on the device (the neighbours, the anchor and the solves made there): the same decode.
// The scene moves, so the motion search, the warps and the gates of the confidence all have something to do.
void WindowTest() {
  const int nf = 8;
  const int h = 64;
  const int w = 96;
  FieldGeometry g;
  g.active_video_start = 0;
  g.active_video_end = w;
  std::vector<hvd::FieldObs> fields(nf);
  std::vector<hvd::DecodedField> inits(nf);
  for (int f = 0; f < nf; ++f) {
    std::vector<float> theta(h);
    for (int r = 0; r < h; ++r) theta[r] = 2.0F * 3.14159265F * (0.75F * static_cast<float>(f) + 0.5F * static_cast<float>(r));
    fields[f].carrier = hvd::MakeCarrier(theta, g);
    fields[f].parity = f % 2;
    fields[f].s = Plane(h, w);
    inits[f].chroma = ComplexPlane(h, w);
    const int shift = f;  // the picture moves a pixel a field
    for (int r = 0; r < h; ++r) {
      for (int c = 0; c < w; ++c) {
        const float x = static_cast<float>(c - shift);
        const float y = 40.0F + 25.0F * std::sin(0.21F * x) * std::cos(0.13F * static_cast<float>(r)) + 6.0F * std::sin(0.9F * x + 0.3F * static_cast<float>(r));
        const Complex chi{12.0F * std::sin(0.11F * x + 0.05F * static_cast<float>(r)), -7.0F * std::cos(0.09F * x)};
        const float noise = 0.4F * std::sin(1.7F * static_cast<float>(r * w + c) + static_cast<float>(f));
        fields[f].s.at(r, c) = y + (chi * fields[f].carrier.at(r, c)).real() + noise;
      }
    }
  }
  HvdConfig cpu;
  cpu.fast = true;
  cpu.passes = 2;
  cpu.cg_iterations = 30;
  cpu.temporal_strength = 0.5F;
  HvdConfig gpu = cpu;
  gpu.use_gpu = true;
  const std::vector<hvd::DecodedField> a = hvd::DecodeFieldWindowWithInits(fields, inits, g, cpu);
  const std::vector<hvd::DecodedField> b = hvd::DecodeFieldWindowWithInits(fields, inits, g, gpu);
  double worst = 0.0;
  for (int f = 0; f < nf; ++f) {
    worst = std::fmax(worst, RmsDiff(a[f].chroma, b[f].chroma));
    worst = std::fmax(worst, RmsDiff(a[f].luma, b[f].luma));
  }
  std::fprintf(stderr, "window: the worst RMS difference between the CPU and the device is %.5f\n", worst);
  CHECK(worst < 5e-2);
}

}  // namespace

void RunTests() {
  if (!hvd::gpu::Available()) {
    std::fprintf(stderr, "gpu_solver_test: no OpenCL GPU here (or a build without OpenCL): skipped\n");
    return;
  }
  std::fprintf(stderr, "gpu_solver_test: %s\n", hvd::gpu::DeviceDescription().c_str());

  const int h = 48;
  const int w = 56;
  FieldGeometry g;
  g.active_video_start = 0;
  g.active_video_end = w;
  const Scene sc = MakeScene(g, h, w);
  const double tolerance = 2e-2;  // IRE: the chroma has an amplitude of ten

  HvdConfig cpu;
  cpu.cg_iterations = 60;
  HvdConfig gpu = cpu;
  gpu.use_gpu = true;

  // 2D
  {
    const hvd::RefineResult a = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, cpu);
    const hvd::RefineResult b = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, gpu);
    CHECK(RmsDiff(a.chroma, b.chroma) < tolerance);
    CHECK(RmsDiff(a.luma, b.luma) < tolerance);
    // (the lossless split holds on the GPU as on the CPU)
    double worst = 0.0;
    for (size_t i = 0; i < sc.s.size(); ++i) worst = std::fmax(worst, std::fabs(sc.s[i] - b.luma[i] - (b.chroma[i] * sc.carrier[i]).real()));
    CHECK(worst < 1e-3);
  }
  // 2D with the oriented prior and the slow mode
  {
    HvdConfig c = cpu;
    c.diag_prior = 0.7F;
    c.fast = false;
    HvdConfig d = c;
    d.use_gpu = true;
    const hvd::RefineResult a = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, c);
    const hvd::RefineResult b = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, d);
    CHECK(RmsDiff(a.chroma, b.chroma) < tolerance);
  }
  // 3D: two neighbours
  {
    const std::vector<hvd::NeighborTerm> nb = MakeNeighbours(sc, g, h, w);
    HvdConfig c = cpu;
    c.temporal_strength = 0.6F;
    c.temporal_eps = 4.0F;
    HvdConfig d = c;
    d.use_gpu = true;
    const hvd::RefineResult a = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, c, nb);
    const hvd::RefineResult b = hvd::VariationalRefine(sc.s, sc.carrier, sc.chi0, d, nb);
    std::fprintf(stderr, "3D: chroma %.5f, luma %.5f\n",RmsDiff(a.chroma, b.chroma), RmsDiff(a.luma, b.luma));
    CHECK(RmsDiff(a.chroma, b.chroma) < tolerance);
    CHECK(RmsDiff(a.luma, b.luma) < tolerance);
  }
  WindowTest();
  // joint: neighbours and an anchor
  {
    const std::vector<hvd::NeighborTerm> nb = MakeNeighbours(sc, g, h, w);
    hvd::AnchorTerm anchor;
    anchor.luma = sc.y0;
    anchor.chroma = sc.chi0;
    anchor.weight = Plane(h, w, 0.7F);
    HvdConfig c = cpu;
    c.temporal_strength = 0.6F;
    c.temporal_eps = 4.0F;
    c.nr_anchor = 0.3F;
    HvdConfig d = c;
    d.use_gpu = true;
    const hvd::RefineResult a = hvd::VariationalRefineJoint(sc.s, sc.carrier, sc.y0, sc.chi0, c, nb, &anchor);
    const hvd::RefineResult b = hvd::VariationalRefineJoint(sc.s, sc.carrier, sc.y0, sc.chi0, d, nb, &anchor);
    std::fprintf(stderr, "joint: chroma %.5f, luma %.5f\n",RmsDiff(a.chroma, b.chroma), RmsDiff(a.luma, b.luma));
    CHECK(RmsDiff(a.chroma, b.chroma) < tolerance);
    CHECK(RmsDiff(a.luma, b.luma) < tolerance);
  }
}

TEST_MAIN()
