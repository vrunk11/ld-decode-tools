// SPDX-License-Identifier: GPL-3.0-or-later
//
// fft2d.h — thin 2-D complex FFT used by the holographic init.
//
// Backed by FFTW3 (single precision). FFTW is declared as a dependency of the
// plugin's own CMake target — the decode-orc SDK explicitly allows plugins to
// bring their own third-party libraries (FFmpeg, FFTW, ...); they are NOT
// inherited from the host.
//
// The reference uses numpy's fft2/ifft2 with the standard normalisation
// (forward unnormalised, inverse divided by N). This wrapper matches that so
// the demodulate -> crop -> inverse pipeline in holographic_init is a faithful
// translation. Plans are cached per (height, width) and reused across frames.

#ifndef LDDECODE_HVD_ENGINE_FFT2D_H_
#define LDDECODE_HVD_ENGINE_FFT2D_H_

#include "engine/plane.h"

namespace hvd {

class Fft2d {
 public:
  Fft2d();
  ~Fft2d();

  Fft2d(const Fft2d&) = delete;
  Fft2d& operator=(const Fft2d&) = delete;

  // In-place semantics via return value. `Forward` is unnormalised;
  // `Inverse` divides by (height * width), matching numpy.
  ComplexPlane Forward(const ComplexPlane& in);
  ComplexPlane Inverse(const ComplexPlane& in);

  // Configure how many threads FFTW itself should use internally for each
  // transform executed through this Fft2d instance. Defaults to
  // std::thread::hardware_concurrency() at construction (use every core for
  // the one frame usually being decoded — the preview/normal path).
  //
  // FFTW is built WITHOUT threading support in this project (see
  // vcpkg.json / CMakeLists.txt: a threaded fftw3f pulls in its own OpenMP
  // runtime alongside this project's own -fopenmp one, which caused DLL/
  // symbol conflicts the host's own decode-orc doesn't have — for FFT sizes
  // this small, the speed difference measured as negligible against the
  // OpenMP-parallel IRLS/CG solver that does the actual work). So this is
  // ALWAYS a no-op now: transforms always run on 1 thread internally,
  // silently, regardless of n. Kept (rather than removed) purely so
  // callers — export workers pinning this to 1 per hvd_chroma_decoder_
  // stage.cpp's oversubscription-avoidance logic, and the config_.
  // fft_threads GUI knob — don't need conditional compilation of their own.
  void SetThreadCount(int n);

 private:
  ComplexPlane Run(const ComplexPlane& in, int sign, bool normalise);

  struct Impl;
  Impl* impl_;  // owns cached FFTW plans; freed in the destructor
};

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_FFT2D_H_
