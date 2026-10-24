// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/fft2d.h"

#include <fftw3.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace hvd {

namespace {
// FFTW's single-precision complex type is layout-compatible with
// std::complex<float> (an array of two floats: {re, im}). We reinterpret rather
// than copy element-by-element.
static_assert(sizeof(Complex) == sizeof(fftwf_complex),
              "std::complex<float> must match fftwf_complex layout");

// fftwf_plan_dft_2d touches FFTW's global planner state, which is NOT
// thread-safe to call concurrently from multiple threads (same reason the
// host's own multi-threaded decoders serialize planning — see
// video_sink_stage.cpp's fftwPlanMutex). This only matters once more than
// one HvdEngine can be planning at the same time, e.g. worker-thread
// parallel export — each thread's Fft2d::PlanFor() call takes this lock for
// the (cheap, FFTW_ESTIMATE) planning step only; fftwf_execute_dft() itself
// is safe to run concurrently on independent plans/buffers with no lock.
//
// This same lock also guards fftwf_plan_with_nthreads() (see PlanFor
// below): that call sets a GLOBAL "how many threads should the next plan
// use" value with no locking of its own, so it has to happen immediately
// before — and under the same lock as — the fftwf_plan_dft_2d() call that
// consumes it, or two threads planning concurrently could race and one
// plan could silently pick up the wrong thread count.
std::mutex& PlanningMutex() {
  static std::mutex m;
  return m;
}
}  // namespace

struct Fft2d::Impl {
  // One reusable plan per (height, width, sign). FFTW_ESTIMATE keeps planning
  // cheap and deterministic (no measurement pass), which suits per-frame use
  // where sizes are stable but the buffer contents change every call.
  std::map<std::tuple<int, int, int>, fftwf_plan> plans;
  // Scratch in/out buffers sized to the largest plane seen so far.
  ComplexPlane scratch_in;
  ComplexPlane scratch_out;

  // Thread count requested via Fft2d::SetThreadCount / HvdEngine::SetFftThreads.
  // Stored so callers (export workers pinning this to 1, see fft2d.h) keep
  // compiling and behaving the same, but FFTW itself no longer threads its
  // own transforms — this build links the single-threaded fftw3f (see the
  // vcpkg.json / CMakeLists comment on HVD_HAVE_FFTW_THREADS's removal): the
  // library problems from a second OpenMP runtime bundled into a threaded
  // fftw3f outweighed a speed gain that measured as negligible next to the
  // OpenMP-parallel IRLS/CG solver, which remains fully threaded.
  int thread_count = 1;

  ~Impl() {
    std::lock_guard<std::mutex> lock(PlanningMutex());
    for (auto& kv : plans) fftwf_destroy_plan(kv.second);
  }

  fftwf_plan PlanFor(int h, int w, int sign, fftwf_complex* in,
                     fftwf_complex* out) {
    const auto key = std::make_tuple(h, w, sign);
    auto it = plans.find(key);
    if (it != plans.end()) return it->second;
    fftwf_plan p;
    {
      std::lock_guard<std::mutex> lock(PlanningMutex());
      p = fftwf_plan_dft_2d(h, w, in, out, sign, FFTW_ESTIMATE);
    }
    plans.emplace(key, p);
    return p;
  }
};

Fft2d::Fft2d() : impl_(new Impl) {}

Fft2d::~Fft2d() { delete impl_; }

void Fft2d::SetThreadCount(int n) { impl_->thread_count = std::max(1, n); }

ComplexPlane Fft2d::Run(const ComplexPlane& in, int sign, bool normalise) {
  const int h = in.height();
  const int w = in.width();
  ComplexPlane out(h, w);
  if (h == 0 || w == 0) return out;

  // Copy input into a fresh buffer (FFTW may overwrite the input for some
  // plans; copying also decouples the caller's plane from FFTW alignment).
  ComplexPlane work = in;
  auto* in_ptr = reinterpret_cast<fftwf_complex*>(work.data());
  auto* out_ptr = reinterpret_cast<fftwf_complex*>(out.data());

  fftwf_plan plan = impl_->PlanFor(h, w, sign, in_ptr, out_ptr);
  // Guaranteed-aligned same-shape buffers => safe to reuse the cached plan
  // with the new-array execute API.
  fftwf_execute_dft(plan, in_ptr, out_ptr);

  if (normalise) {
    const float inv = 1.0F / static_cast<float>(static_cast<long>(h) * w);
    for (size_t i = 0; i < out.size(); ++i) out[i] *= inv;
  }
  return out;
}

ComplexPlane Fft2d::Forward(const ComplexPlane& in) {
  return Run(in, FFTW_FORWARD, /*normalise=*/false);
}

ComplexPlane Fft2d::Inverse(const ComplexPlane& in) {
  return Run(in, FFTW_BACKWARD, /*normalise=*/true);
}

}  // namespace hvd
