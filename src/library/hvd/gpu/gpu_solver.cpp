// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu_solver.cpp — the solver of the HVD decoder in OpenCL. See gpu_solver.h.
//
// The maths is the one of VariationalRefine (variational.cpp), pass for pass: IRLS weights at the present chroma (lagged diffusivity), then
// conjugate gradient on the quadratic with these weights. Each kernel is one pass over a plane, one work-item for each pixel; the transposed
// differences are written as what a pixel gets from its neighbours (a forward difference of a weighted plane, minus the one before it), so a
// pass makes the gradient of the objective in one go.

#include "gpu/gpu_solver.h"

#ifdef LDDECODE_HVD_OPENCL

#define CL_TARGET_OPENCL_VERSION 120
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace hvd {
namespace gpu {
namespace {

constexpr size_t kGroup = 256;  // work-items of a group: the sums are made in the groups, and added by the host
std::atomic<long long> g_uploadNs{0};   // (statistics) the planes going to the device
std::atomic<long long> g_computeNs{0};  // the iterations, with the sums coming back
std::atomic<long long> g_copyNs{0};     // (statistics) the copies of the planes around the device call
std::atomic<long long> g_deviceNs{0};   // the device call as a whole (Prepare, writes, iterations, reads)

const char* const kSource = R"CLC(
#define GS 256

inline float cabs2(float2 a) { return a.x * a.x + a.y * a.y; }
inline float rmul(float2 a, float2 b) { return a.x * b.x - a.y * b.y; }

// Y = S - Re[chi * carrier]
__kernel void k_luma(__global const float* s, __global const float2* chi, __global const float2* car, __global float* yc, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  yc[i] = s[i] - rmul(chi[i], car[i]);
}

// dc = carrier - c_w, the difference that the data term of a neighbour sees
__kernel void k_make_dc(__global const float2* car, __global const float2* cw, __global float2* dc, int N, int base) {
  const int i = get_global_id(0);
  if (i >= N) return;
  dc[base + i] = car[i] - cw[i];
}

// The IRLS weights at the present chroma: Charbonnier of the luma gradients, and of the chroma gradients coupled to the luma ones
__kernel void k_weights(__global const float* yc, __global const float2* chi, __global float* wx, __global float* wy, __global float* wcx, __global float* wcy,
                        __global float* wd1, __global float* wd2, int W, int H, float eps, float eps_c, float k, int diag) {
  const int i = get_global_id(0);
  if (i >= W * H) return;
  const int x = i % W;
  const int y = i / W;
  const float e2 = eps * eps;
  const float ec2 = eps_c * eps_c;
  const float gx = (x + 1 < W) ? yc[i + 1] - yc[i] : 0.0f;
  const float gy = (y + 1 < H) ? yc[i + W] - yc[i] : 0.0f;
  const float2 zero = (float2)(0.0f, 0.0f);
  const float2 cx = (x + 1 < W) ? chi[i + 1] - chi[i] : zero;
  const float2 cy = (y + 1 < H) ? chi[i + W] - chi[i] : zero;
  wx[i] = eps / sqrt(gx * gx + e2);
  wy[i] = eps / sqrt(gy * gy + e2);
  wcx[i] = eps_c / sqrt(cabs2(cx) + k * gx * gx + ec2);
  wcy[i] = eps_c / sqrt(cabs2(cy) + k * gy * gy + ec2);
  if (diag) {
    const float2 d1 = (y + 1 < H && x + 1 < W) ? chi[i + W + 1] - chi[i] : zero;
    const float2 d2 = (y + 1 < H && x >= 1) ? chi[i + W - 1] - chi[i] : zero;
    const float c1 = length(d1) / eps_c;
    const float c2 = length(d2) / eps_c;
    wd1[i] = 1.0f / sqrt(1.0f + c1 * c1);
    wd2[i] = 1.0f / sqrt(1.0f + c2 * c2);
  }
}

// The robust weight of a neighbour, and the sum that renormalises the priors: scale = 1 + nu * sum_j wt_j |dc_j|^2
__kernel void k_nbr_weight(__global const float* s, __global const float2* chi, __global const float* comp, __global const float2* dc,
                           __global const float* conf, __global float* wt, __global float* scale, int N, float et2, float nu, int first, int base) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float rt = (comp[base + i] - s[i]) + rmul(chi[i], dc[base + i]);
  const float w = conf[base + i] * et2 / (rt * rt + et2);
  wt[base + i] = w;
  scale[i] = (first ? 1.0f : scale[i]) + nu * w * cabs2(dc[base + i]);
}

__kernel void k_scale(__global float* wx, __global float* wy, __global float* wcx, __global float* wcy, __global float* wd1, __global float* wd2,
                      __global const float* scale, int N, int diag) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float f = scale[i];
  wx[i] *= f;
  wy[i] *= f;
  wcx[i] *= f;
  wcy[i] *= f;
  if (diag) {
    wd1[i] *= f;
    wd2[i] *= f;
  }
}

// g = -[2 (DxT(wx Dx Yc) + DyT(wy Dy Yc))] conj(carrier) + 2 (mu_h DxT(wcx Dx chi) + mu_v DyT(wcy Dy chi)) [+ oriented terms]
//     + 2 nu sum_j wt_j rt_j conj(dc_j)        (Yc is in yc, made from chi by k_luma)
__kernel void k_grad(__global const float* yc, __global const float2* chi, __global const float2* car, __global const float* wx, __global const float* wy,
                     __global const float* wcx, __global const float* wcy, __global const float* wd1, __global const float* wd2, __global const float* s,
                     __global const float* comp, __global const float2* dc, __global const float* wt, __global float2* out, int W, int H, float mu_h,
                     float mu_v, float mu_d, float nu, int nn) {
  const int N = W * H;
  const int i = get_global_id(0);
  if (i >= N) return;
  const int x = i % W;
  const int y = i / W;
  const float2 zero = (float2)(0.0f, 0.0f);

  // the transposed difference of the weighted difference: what the pixel gets from the one before it, minus what it gives to the one after
  float tx = 0.0f;
  float ty = 0.0f;
  if (x > 0) tx += wx[i - 1] * (yc[i] - yc[i - 1]);
  if (x + 1 < W) tx -= wx[i] * (yc[i + 1] - yc[i]);
  if (y > 0) ty += wy[i - W] * (yc[i] - yc[i - W]);
  if (y + 1 < H) ty -= wy[i] * (yc[i + W] - yc[i]);
  const float img = 2.0f * (tx + ty);

  float2 px = zero;
  float2 py = zero;
  if (x > 0) px += wcx[i - 1] * (chi[i] - chi[i - 1]);
  if (x + 1 < W) px -= wcx[i] * (chi[i + 1] - chi[i]);
  if (y > 0) py += wcy[i - W] * (chi[i] - chi[i - W]);
  if (y + 1 < H) py -= wcy[i] * (chi[i + W] - chi[i]);
  float2 cprior = 2.0f * (mu_h * px + mu_v * py);

  if (mu_d > 0.0f) {
    float2 q1 = zero;
    float2 q2 = zero;
    if (y + 1 < H && x + 1 < W) q1 -= wd1[i] * (chi[i + W + 1] - chi[i]);
    if (y >= 1 && x >= 1) q1 += wd1[i - W - 1] * (chi[i] - chi[i - W - 1]);
    if (y + 1 < H && x >= 1) q2 -= wd2[i] * (chi[i + W - 1] - chi[i]);
    if (y >= 1 && x + 1 < W) q2 += wd2[i - W + 1] * (chi[i] - chi[i - W + 1]);
    cprior += 2.0f * mu_d * (q1 + q2);
  }

  const float2 k = car[i];
  float2 o = (float2)(-img * k.x, img * k.y) + cprior;  // -img * conj(carrier)
  for (int j = 0; j < nn; ++j) {
    const size_t o2 = (size_t)j * (size_t)N + (size_t)i;
    const float rt = (comp[o2] - s[i]) + rmul(chi[i], dc[o2]);
    const float f = 2.0f * nu * wt[o2] * rt;
    o += (float2)(f * dc[o2].x, -f * dc[o2].y);  // f * conj(dc)
  }
  out[i] = o;
}

// dY = -Re[dC * carrier]
__kernel void k_dy(__global const float2* d, __global const float2* car, __global float* dyb, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  dyb[i] = -rmul(d[i], car[i]);
}

// The sum of a value of each pixel, made in the group (a tree in the local memory) and left for the host to add
inline void group_sum(__local float* sh, float v, __global float* partial, int poff) {
  const int l = get_local_id(0);
  sh[l] = v;
  barrier(CLK_LOCAL_MEM_FENCE);
  for (int step = GS / 2; step > 0; step >>= 1) {
    if (l < step) sh[l] += sh[l + step];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if (l == 0) partial[poff + get_group_id(0)] = sh[0];
}

// The curvature of the objective along d: sum wx (Dx dY)^2 + wy (Dy dY)^2 + mu_h wcx |Dx dC|^2 + mu_v wcy |Dy dC|^2 [+ oriented] + nu sum_j wt_j Re[dC dc_j]^2
__kernel void k_curv(__global const float* dyb, __global const float2* d, __global const float* wx, __global const float* wy, __global const float* wcx,
                     __global const float* wcy, __global const float* wd1, __global const float* wd2, __global const float2* dc, __global const float* wt,
                     __global float* partial, int poff, __local float* sh, int W, int H, float mu_h, float mu_v, float mu_d, float nu, int nn) {
  const int N = W * H;
  const int i = get_global_id(0);
  float h = 0.0f;
  if (i < N) {
    const int x = i % W;
    const int y = i / W;
    const float2 zero = (float2)(0.0f, 0.0f);
    const float dxdy = (x + 1 < W) ? dyb[i + 1] - dyb[i] : 0.0f;
    const float dydy = (y + 1 < H) ? dyb[i + W] - dyb[i] : 0.0f;
    const float2 dxc = (x + 1 < W) ? d[i + 1] - d[i] : zero;
    const float2 dyc = (y + 1 < H) ? d[i + W] - d[i] : zero;
    h = wx[i] * dxdy * dxdy + wy[i] * dydy * dydy + mu_h * wcx[i] * cabs2(dxc) + mu_v * wcy[i] * cabs2(dyc);
    if (mu_d > 0.0f) {
      const float2 d1 = (y + 1 < H && x + 1 < W) ? d[i + W + 1] - d[i] : zero;
      const float2 d2 = (y + 1 < H && x >= 1) ? d[i + W - 1] - d[i] : zero;
      h += mu_d * (wd1[i] * cabs2(d1) + wd2[i] * cabs2(d2));
    }
    for (int j = 0; j < nn; ++j) {
      const size_t o2 = (size_t)j * (size_t)N + (size_t)i;
      const float re = rmul(d[i], dc[o2]);
      h += nu * wt[o2] * re * re;
    }
  }
  group_sum(sh, h, partial, poff);
}

// sum of Re[conj(a) b]
__kernel void k_dot(__global const float2* a, __global const float2* b, __global float* partial, int poff, __local float* sh, int N) {
  const int i = get_global_id(0);
  float v = 0.0f;
  if (i < N) v = a[i].x * b[i].x + a[i].y * b[i].y;
  group_sum(sh, v, partial, poff);
}

__kernel void k_neg(__global float2* d, __global const float2* g, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  d[i] = -g[i];
}

__kernel void k_axpy(__global float2* chi, __global const float2* d, float alpha, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  chi[i] += alpha * d[i];
}

__kernel void k_dupdate(__global float2* d, __global const float2* g, float beta, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  d[i] = -g[i] + beta * d[i];
}

// ---- The joint solver (pass 2 and after, with the anchor): both the luma Y and the chroma chi are unknowns -------------------------------------

// The robust weight of a neighbour on its raw residual, and scale = 1 + nu * sum_j wt_j (here Y is free: the data term of a neighbour has the leverage of the base term)
__kernel void k_jnbr_weight(__global const float* Y, __global const float2* chi, __global const float* comp, __global const float2* cw, __global const float* conf,
                            __global float* wt, __global float* scale, int N, float et2, float nu, int first, int base) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float r = comp[base + i] - Y[i] - rmul(chi[i], cw[base + i]);
  const float w = conf[base + i] * et2 / (r * r + et2);
  wt[base + i] = w;
  scale[i] = (first ? 1.0f : scale[i]) + nu * w;
}

// The gradient of the joint objective: exact data term + the raw terms of the neighbours + the anchor + the spatial priors on Y and on chi
__kernel void k_jgrad(__global const float* Y, __global const float2* chi, __global const float2* car, __global const float* s, __global const float* wx,
                      __global const float* wy, __global const float* wcx, __global const float* wcy, __global const float* wd1, __global const float* wd2,
                      __global const float* comp, __global const float2* cw, __global const float* wt, __global const float* aw, __global const float* ay,
                      __global const float2* ac, __global float* gY, __global float2* gC, int W, int H, float mu_h, float mu_v, float mu_d, float nu, float nu_a,
                      int nn) {
  const int N = W * H;
  const int i = get_global_id(0);
  if (i >= N) return;
  const int x = i % W;
  const int y = i / W;
  const float2 zero = (float2)(0.0f, 0.0f);

  const float r0 = s[i] - Y[i] - rmul(chi[i], car[i]);
  float gy = -2.0f * r0;
  const float2 k = car[i];
  float2 gc = (float2)(-2.0f * r0 * k.x, 2.0f * r0 * k.y);  // -2 r0 conj(carrier)
  for (int j = 0; j < nn; ++j) {
    const size_t o2 = (size_t)j * (size_t)N + (size_t)i;
    const float rk = comp[o2] - Y[i] - rmul(chi[i], cw[o2]);
    const float f = -2.0f * nu * wt[o2] * rk;
    gy += f;
    gc += (float2)(f * cw[o2].x, -f * cw[o2].y);  // f * conj(carrier_j)
  }
  if (nu_a > 0.0f) {
    gy += 2.0f * nu_a * aw[i] * (Y[i] - ay[i]);
    gc += 2.0f * nu_a * aw[i] * (chi[i] - ac[i]);
  }

  float tx = 0.0f;
  float ty = 0.0f;
  if (x > 0) tx += wx[i - 1] * (Y[i] - Y[i - 1]);
  if (x + 1 < W) tx -= wx[i] * (Y[i + 1] - Y[i]);
  if (y > 0) ty += wy[i - W] * (Y[i] - Y[i - W]);
  if (y + 1 < H) ty -= wy[i] * (Y[i + W] - Y[i]);
  gy += 2.0f * (tx + ty);

  float2 px = zero;
  float2 py = zero;
  if (x > 0) px += wcx[i - 1] * (chi[i] - chi[i - 1]);
  if (x + 1 < W) px -= wcx[i] * (chi[i + 1] - chi[i]);
  if (y > 0) py += wcy[i - W] * (chi[i] - chi[i - W]);
  if (y + 1 < H) py -= wcy[i] * (chi[i + W] - chi[i]);
  gc += 2.0f * mu_h * px + 2.0f * mu_v * py;

  if (mu_d > 0.0f) {
    float2 q1 = zero;
    float2 q2 = zero;
    if (y + 1 < H && x + 1 < W) q1 -= wd1[i] * (chi[i + W + 1] - chi[i]);
    if (y >= 1 && x >= 1) q1 += wd1[i - W - 1] * (chi[i] - chi[i - W - 1]);
    if (y + 1 < H && x >= 1) q2 -= wd2[i] * (chi[i + W - 1] - chi[i]);
    if (y >= 1 && x + 1 < W) q2 += wd2[i - W + 1] * (chi[i] - chi[i - W + 1]);
    gc += 2.0f * mu_d * (q1 + q2);
  }
  gY[i] = gy;
  gC[i] = gc;
}

// The curvature of the joint objective along (dY, dC)
__kernel void k_jcurv(__global const float* dY, __global const float2* dC, __global const float2* car, __global const float* wx, __global const float* wy,
                      __global const float* wcx, __global const float* wcy, __global const float* wd1, __global const float* wd2, __global const float2* cw,
                      __global const float* wt, __global const float* aw, __global float* partial, int poff, __local float* sh, int W, int H, float mu_h,
                      float mu_v, float mu_d, float nu, float nu_a, int nn) {
  const int N = W * H;
  const int i = get_global_id(0);
  float h = 0.0f;
  if (i < N) {
    const int x = i % W;
    const int y = i / W;
    const float2 zero = (float2)(0.0f, 0.0f);
    const float base = dY[i] + rmul(dC[i], car[i]);
    h = base * base;
    for (int j = 0; j < nn; ++j) {
      const size_t o2 = (size_t)j * (size_t)N + (size_t)i;
      const float dk = dY[i] + rmul(dC[i], cw[o2]);
      h += nu * wt[o2] * dk * dk;
    }
    if (nu_a > 0.0f) h += nu_a * aw[i] * (dY[i] * dY[i] + cabs2(dC[i]));
    const float dxdy = (x + 1 < W) ? dY[i + 1] - dY[i] : 0.0f;
    const float dydy = (y + 1 < H) ? dY[i + W] - dY[i] : 0.0f;
    const float2 dxc = (x + 1 < W) ? dC[i + 1] - dC[i] : zero;
    const float2 dyc = (y + 1 < H) ? dC[i + W] - dC[i] : zero;
    h += wx[i] * dxdy * dxdy + wy[i] * dydy * dydy + mu_h * wcx[i] * cabs2(dxc) + mu_v * wcy[i] * cabs2(dyc);
    if (mu_d > 0.0f) {
      const float2 d1 = (y + 1 < H && x + 1 < W) ? dC[i + W + 1] - dC[i] : zero;
      const float2 d2 = (y + 1 < H && x >= 1) ? dC[i + W - 1] - dC[i] : zero;
      h += mu_d * (wd1[i] * cabs2(d1) + wd2[i] * cabs2(d2));
    }
  }
  group_sum(sh, h, partial, poff);
}

// sum of aY bY + Re[conj(aC) bC]
__kernel void k_jdot(__global const float* aY, __global const float* bY, __global const float2* aC, __global const float2* bC, __global float* partial, int poff,
                     __local float* sh, int N) {
  const int i = get_global_id(0);
  float v = 0.0f;
  if (i < N) v = aY[i] * bY[i] + aC[i].x * bC[i].x + aC[i].y * bC[i].y;
  group_sum(sh, v, partial, poff);
}

__kernel void k_jneg(__global float* dY, __global float2* dC, __global const float* gY, __global const float2* gC, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  dY[i] = -gY[i];
  dC[i] = -gC[i];
}

__kernel void k_jaxpy(__global float* Y, __global float2* chi, __global const float* dY, __global const float2* dC, float alpha, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  Y[i] += alpha * dY[i];
  chi[i] += alpha * dC[i];
}

__kernel void k_jdupdate(__global float* dY, __global float2* dC, __global const float* gY, __global const float2* gC, float beta, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  dY[i] = -gY[i] + beta * dY[i];
  dC[i] = -gC[i] + beta * dC[i];
}
)CLC";

struct GpuError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void Check(cl_int error, const char* what) {
  if (error != CL_SUCCESS) throw GpuError(std::string(what) + " (OpenCL error " + std::to_string(error) + ")");
}

// A buffer of the device, released with the object
class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() { Release(); }
  void Allocate(cl_context context, size_t bytes) {
    Release();
    cl_int error = CL_SUCCESS;
    mem_ = clCreateBuffer(context, CL_MEM_READ_WRITE, std::max<size_t>(bytes, 16), nullptr, &error);
    Check(error, "clCreateBuffer");
    bytes_ = bytes;
  }
  void Release() {
    if (mem_) clReleaseMemObject(mem_);
    mem_ = nullptr;
    bytes_ = 0;
  }
  cl_mem get() const { return mem_; }
  size_t bytes() const { return bytes_; }

 private:
  cl_mem mem_ = nullptr;
  size_t bytes_ = 0;
};

// What a solve sends to the device, in the order of the staging block: the caller packs the planes there (in pinned memory, before it has a worker) and
// the worker writes them to the device from there. The same layout for the one and the other.
inline size_t Align256(size_t v) { return (v + 255) & ~static_cast<size_t>(255); }

struct Layout {
  size_t s = 0, car = 0, chi = 0, y = 0, aw = 0, ay = 0, ac = 0;  // the planes of the field (y and the anchor for the joint solve)
  size_t nbr0 = 0, nbrStride = 0, plane = 0;                       // the neighbours: composite, confidence, carrier, one after the other
  size_t total = 0;
  size_t comp(size_t j) const { return nbr0 + j * nbrStride; }
  size_t conf(size_t j) const { return comp(j) + plane; }
  size_t cw(size_t j) const { return comp(j) + 2 * plane; }
};

Layout MakeLayout(size_t N, size_t nn, bool joint, bool anchor) {
  Layout L;
  const size_t f = Align256(N * sizeof(float));
  const size_t c = Align256(N * sizeof(Complex));
  size_t at = 0;
  L.s = at;
  at += f;
  L.car = at;
  at += c;
  L.chi = at;
  at += c;
  if (joint) {
    L.y = at;
    at += f;
    if (anchor) {
      L.aw = at;
      at += f;
      L.ay = at;
      at += f;
      L.ac = at;
      at += c;
    }
  }
  L.plane = f;
  L.nbr0 = at;
  L.nbrStride = 2 * f + c;
  at += nn * L.nbrStride;
  L.total = at;
  return L;
}

// The planes packed for a solve, and the way to give the block back once the device has read it (after its first sum came back)
struct Staged {
  unsigned char* ptr = nullptr;
  std::function<void()> release;
  void Release() const {
    if (release) release();
  }
};

class Device {
 public:
  // Looks for a GPU, makes the context and the queue, builds the kernels. Throws GpuError when it cannot.
  Device() {
    cl_uint platformCount = 0;
    Check(clGetPlatformIDs(0, nullptr, &platformCount), "clGetPlatformIDs");
    if (platformCount == 0) throw GpuError("no OpenCL platform");
    std::vector<cl_platform_id> platforms(platformCount);
    Check(clGetPlatformIDs(platformCount, platforms.data(), nullptr), "clGetPlatformIDs");

    struct Candidate {
      cl_device_id id;
      std::string name;
      std::string version;
      double score;
    };
    std::vector<Candidate> candidates;
    for (cl_platform_id platform : platforms) {
      cl_uint count = 0;
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &count) != CL_SUCCESS || count == 0) continue;
      std::vector<cl_device_id> ids(count);
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, count, ids.data(), nullptr) != CL_SUCCESS) continue;
      for (cl_device_id id : ids) {
        cl_bool available = CL_FALSE;
        size_t group = 0;
        cl_ulong local = 0;
        cl_uint units = 0;
        cl_uint clock = 0;
        clGetDeviceInfo(id, CL_DEVICE_AVAILABLE, sizeof available, &available, nullptr);
        clGetDeviceInfo(id, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof group, &group, nullptr);
        clGetDeviceInfo(id, CL_DEVICE_LOCAL_MEM_SIZE, sizeof local, &local, nullptr);
        clGetDeviceInfo(id, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof units, &units, nullptr);
        clGetDeviceInfo(id, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof clock, &clock, nullptr);
        if (!available || group < kGroup || local < kGroup * sizeof(float)) continue;
        char name[256] = {0};
        char version[128] = {0};
        clGetDeviceInfo(id, CL_DEVICE_NAME, sizeof name - 1, name, nullptr);
        clGetDeviceInfo(id, CL_DEVICE_VERSION, sizeof version - 1, version, nullptr);
        candidates.push_back({id, name, version, static_cast<double>(units) * std::max<cl_uint>(clock, 1)});
      }
    }
    if (candidates.empty()) throw GpuError("no GPU found by OpenCL");

    // the strongest, unless HVD_OPENCL_DEVICE says which (the order of HVD_OPENCL_VERBOSE)
    size_t chosen = 0;
    for (size_t i = 1; i < candidates.size(); ++i) {
      if (candidates[i].score > candidates[chosen].score) chosen = i;
    }
    if (const char* wanted = std::getenv("HVD_OPENCL_DEVICE")) {
      const long index = std::strtol(wanted, nullptr, 10);
      if (index >= 0 && static_cast<size_t>(index) < candidates.size()) chosen = static_cast<size_t>(index);
    }
    if (std::getenv("HVD_OPENCL_VERBOSE")) {
      for (size_t i = 0; i < candidates.size(); ++i) {
        std::fprintf(stderr, "HVD OpenCL device %zu: %s (%s)%s\n", i, candidates[i].name.c_str(), candidates[i].version.c_str(), i == chosen ? "  <- used" : "");
      }
    }
    device_ = candidates[chosen].id;
    description_ = candidates[chosen].name + " (" + candidates[chosen].version + ")";

    cl_int error = CL_SUCCESS;
    context_ = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &error);
    Check(error, "clCreateContext");
    queue_ = clCreateCommandQueue(context_, device_, 0, &error);
    Check(error, "clCreateCommandQueue");
    const char* source = kSource;
    program_ = clCreateProgramWithSource(context_, 1, &source, nullptr, &error);
    Check(error, "clCreateProgramWithSource");
    error = clBuildProgram(program_, 1, &device_, "-cl-mad-enable", nullptr, nullptr);
    if (error != CL_SUCCESS) {
      size_t size = 0;
      clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
      std::string log(size, '\0');
      clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, size, &log[0], nullptr);
      throw GpuError("the kernels do not build: " + log);
    }
    MakeKernels();
  }

  // Another worker on the same device: its own queue, kernels and buffers (so that it runs beside the others and its transfers overlap their
  // iterations), the context and the program being shared. A kernel object holds its arguments, so two threads cannot use the same one.
  Device(const Device& shared, int /*another worker*/)
      : device_(shared.device_), context_(shared.context_), program_(shared.program_), description_(shared.description_) {
    cl_int error = CL_SUCCESS;
    queue_ = clCreateCommandQueue(context_, device_, 0, &error);
    Check(error, "clCreateCommandQueue");
    MakeKernels();
  }

  void MakeKernels() {
    luma_ = Kernel("k_luma");
    makeDc_ = Kernel("k_make_dc");
    weights_ = Kernel("k_weights");
    nbrWeight_ = Kernel("k_nbr_weight");
    scale_ = Kernel("k_scale");
    grad_ = Kernel("k_grad");
    dy_ = Kernel("k_dy");
    curv_ = Kernel("k_curv");
    dot_ = Kernel("k_dot");
    neg_ = Kernel("k_neg");
    axpy_ = Kernel("k_axpy");
    dupdate_ = Kernel("k_dupdate");
    jnbrWeight_ = Kernel("k_jnbr_weight");
    jgrad_ = Kernel("k_jgrad");
    jcurv_ = Kernel("k_jcurv");
    jdot_ = Kernel("k_jdot");
    jneg_ = Kernel("k_jneg");
    jaxpy_ = Kernel("k_jaxpy");
    jdupdate_ = Kernel("k_jdupdate");
  }

  const std::string& description() const { return description_; }
  cl_context context() const { return context_; }
  cl_device_id device() const { return device_; }

  // One solve. The planes are the host's, `chi` goes in with the holographic start and comes out refined.
  // The planes are in the staging block (packed by the caller, see Layout), and go to the device from there.
  void Solve(int H, int W, const SolveParameters& p, int nn, const Staged& st, ComplexPlane* chi) {
    const size_t N = static_cast<size_t>(H) * static_cast<size_t>(W);
    const int n = static_cast<int>(N);
    const bool diag = p.mu_d > 0.0F;
    const size_t groups = (N + kGroup - 1) / kGroup;
    const size_t global = groups * kGroup;
    const Layout L = MakeLayout(N, static_cast<size_t>(nn), false, false);
    Prepare(N, static_cast<size_t>(nn), groups);
    const auto tp0 = std::chrono::steady_clock::now();

    WriteAsync(b_.s, st.ptr + L.s, N * sizeof(float));
    WriteAsync(b_.car, st.ptr + L.car, N * sizeof(Complex));
    WriteAsync(b_.chi, st.ptr + L.chi, N * sizeof(Complex));
    for (int j = 0; j < nn; ++j) {
      const size_t at = static_cast<size_t>(j) * N;
      WriteAsync(b_.comp, st.ptr + L.comp(static_cast<size_t>(j)), N * sizeof(float), at * sizeof(float));
      WriteAsync(b_.conf, st.ptr + L.conf(static_cast<size_t>(j)), N * sizeof(float), at * sizeof(float));
      WriteAsync(b_.cw, st.ptr + L.cw(static_cast<size_t>(j)), N * sizeof(Complex));
      Run(makeDc_, global, kGroup, {Arg(b_.car), Arg(b_.cw), Arg(b_.dc), Arg(n), Arg(static_cast<int>(at))});
    }

    const auto tp1 = std::chrono::steady_clock::now();
    g_uploadNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tp1 - tp0).count();
    const float et2 = p.eps_t * p.eps_t;
    cl_mem chiBuf = b_.chi.get();
    cl_mem gBuf = b_.g.get();
    cl_mem gNew = b_.gn.get();
    const double nPixels = static_cast<double>(N);
    const size_t partialBytes = groups * sizeof(float);
    std::vector<float> partial(groups);

    // Sum of a pass that wrote its partial sums into region `region` of the buffer
    auto reduce = [&](size_t region) {
      Check(clEnqueueReadBuffer(queue_, b_.partial.get(), CL_TRUE, region * partialBytes, partialBytes, partial.data(), 0, nullptr, nullptr), "read of a sum");
      double sum = 0.0;
      for (float v : partial) sum += static_cast<double>(v);
      return sum;
    };
    auto lumaOf = [&](cl_mem chiMem) { Run(luma_, global, kGroup, {Arg(b_.s), ArgMem(chiMem), Arg(b_.car), Arg(b_.yc), Arg(n)}); };
    auto gradOf = [&](cl_mem out) {
      Run(grad_, global, kGroup,
          {Arg(b_.yc), ArgMem(chiBuf), Arg(b_.car), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.s), Arg(b_.comp), Arg(b_.dc),
           Arg(b_.wt), ArgMem(out), Arg(W), Arg(H), Arg(p.mu_h), Arg(p.mu_v), Arg(p.mu_d), Arg(p.nu), Arg(nn)});
    };
    auto sumAbs2 = [&](cl_mem a, size_t region) {
      Run(dot_, global, kGroup, {ArgMem(a), ArgMem(a), Arg(b_.partial), Arg(static_cast<int>(region * groups)), ArgLocal(kGroup * sizeof(float)), Arg(n)});
      return reduce(region);
    };

    for (int outer = 0; outer < p.irls_outer; ++outer) {
      // the weights at the present chroma
      lumaOf(chiBuf);
      Run(weights_, global, kGroup,
          {Arg(b_.yc), ArgMem(chiBuf), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(W), Arg(H), Arg(p.eps), Arg(p.eps_c), Arg(p.k),
           Arg(diag ? 1 : 0)});
      if (nn > 0) {
        for (int j = 0; j < nn; ++j) {
          const size_t at = static_cast<size_t>(j) * N;
          Run(nbrWeight_, global, kGroup,
              {Arg(b_.s), ArgMem(chiBuf), Arg(b_.comp), Arg(b_.dc), Arg(b_.conf), Arg(b_.wt), Arg(b_.scale), Arg(n), Arg(et2), Arg(p.nu), Arg(j == 0 ? 1 : 0),
               Arg(static_cast<int>(at))});
        }
        Run(scale_, global, kGroup, {Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.scale), Arg(n), Arg(diag ? 1 : 0)});
      }

      // conjugate gradient on the quadratic with these weights
      gradOf(gBuf);
      Run(neg_, global, kGroup, {Arg(b_.d), ArgMem(gBuf), Arg(n)});
      double gg = sumAbs2(gBuf, 0);
      st.Release();  // (the sum has come back: the device has read the staging block)
      const double gg0 = gg;
      for (int it = 0; it < p.n_inner; ++it) {
        Run(dy_, global, kGroup, {Arg(b_.d), Arg(b_.car), Arg(b_.dyb), Arg(n)});
        Run(curv_, global, kGroup,
            {Arg(b_.dyb), Arg(b_.d), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.dc), Arg(b_.wt), Arg(b_.partial), Arg(0),
             ArgLocal(kGroup * sizeof(float)), Arg(W), Arg(H), Arg(p.mu_h), Arg(p.mu_v), Arg(p.mu_d), Arg(p.nu), Arg(nn)});
        Run(dot_, global, kGroup, {ArgMem(gBuf), Arg(b_.d), Arg(b_.partial), Arg(static_cast<int>(groups)), ArgLocal(kGroup * sizeof(float)), Arg(n)});
        // the two sums come back together: the one of the curvature and the one of g . d
        std::vector<float> both(2 * groups);
        Check(clEnqueueReadBuffer(queue_, b_.partial.get(), CL_TRUE, 0, 2 * partialBytes, both.data(), 0, nullptr, nullptr), "read of the sums");
        double hh = 0.0;
        double gd = 0.0;
        for (size_t i = 0; i < groups; ++i) {
          hh += static_cast<double>(both[i]);
          gd += static_cast<double>(both[groups + i]);
        }
        if (hh <= 1e-12) break;
        const float alpha = static_cast<float>(-0.5 * gd / hh);
        Run(axpy_, global, kGroup, {ArgMem(chiBuf), Arg(b_.d), Arg(alpha), Arg(n)});

        lumaOf(chiBuf);
        gradOf(gNew);
        const double ggNew = sumAbs2(gNew, 0);
        const float beta = static_cast<float>(ggNew / std::max(gg, 1e-30));
        Run(dupdate_, global, kGroup, {Arg(b_.d), ArgMem(gNew), Arg(beta), Arg(n)});
        std::swap(gBuf, gNew);
        gg = ggNew;
        if (gg < static_cast<double>(p.cg_tol) * p.cg_tol * gg0) break;
        if (gg < 1e-10 * nPixels) break;
      }
    }
    g_computeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tp1).count();
    Check(clEnqueueReadBuffer(queue_, chiBuf, CL_TRUE, 0, N * sizeof(Complex), chi->data(), 0, nullptr, nullptr), "read of the chroma");
  }

  // The joint solve of the passes that have the anchor: Y and chi are both unknowns. `y` goes in with the first luma and comes out refined.
  void SolveJoint(int H, int W, const SolveParameters& p, float nu_a, int nn, bool withAnchor, const Staged& st, Plane* y, ComplexPlane* chi) {
    const size_t N = static_cast<size_t>(H) * static_cast<size_t>(W);
    const int n = static_cast<int>(N);
    const bool diag = p.mu_d > 0.0F;
    const size_t groups = (N + kGroup - 1) / kGroup;
    const size_t global = groups * kGroup;
    const Layout L = MakeLayout(N, static_cast<size_t>(nn), true, withAnchor);
    Prepare(N, static_cast<size_t>(nn), groups);
    const auto tp0 = std::chrono::steady_clock::now();

    WriteAsync(b_.s, st.ptr + L.s, N * sizeof(float));
    WriteAsync(b_.car, st.ptr + L.car, N * sizeof(Complex));
    WriteAsync(b_.chi, st.ptr + L.chi, N * sizeof(Complex));
    WriteAsync(b_.yc, st.ptr + L.y, N * sizeof(float));  // the luma Y (the buffer of the luma of the other solver)
    for (int j = 0; j < nn; ++j) {
      const size_t at = static_cast<size_t>(j) * N;
      WriteAsync(b_.comp, st.ptr + L.comp(static_cast<size_t>(j)), N * sizeof(float), at * sizeof(float));
      WriteAsync(b_.conf, st.ptr + L.conf(static_cast<size_t>(j)), N * sizeof(float), at * sizeof(float));
      WriteAsync(b_.dc, st.ptr + L.cw(static_cast<size_t>(j)), N * sizeof(Complex), at * sizeof(Complex));  // the carrier of the neighbour itself
    }
    if (withAnchor) {
      WriteAsync(b_.aw, st.ptr + L.aw, N * sizeof(float));
      WriteAsync(b_.ay, st.ptr + L.ay, N * sizeof(float));
      WriteAsync(b_.ac, st.ptr + L.ac, N * sizeof(Complex));
    }
    const float anchorStrength = withAnchor ? nu_a : 0.0F;

    const auto tp1 = std::chrono::steady_clock::now();
    g_uploadNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tp1 - tp0).count();
    const float et2 = p.eps_t * p.eps_t;
    cl_mem yBuf = b_.yc.get();
    cl_mem chiBuf = b_.chi.get();
    cl_mem gyBuf = b_.gy.get();
    cl_mem gyNew = b_.gyn.get();
    cl_mem gcBuf = b_.g.get();
    cl_mem gcNew = b_.gn.get();
    const double nPixels = static_cast<double>(N);
    const size_t partialBytes = groups * sizeof(float);
    std::vector<float> partial(2 * groups);

    auto gradOf = [&](cl_mem gyOut, cl_mem gcOut) {
      Run(jgrad_, global, kGroup,
          {ArgMem(yBuf), ArgMem(chiBuf), Arg(b_.car), Arg(b_.s), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.comp), Arg(b_.dc),
           Arg(b_.wt), Arg(b_.aw), Arg(b_.ay), Arg(b_.ac), ArgMem(gyOut), ArgMem(gcOut), Arg(W), Arg(H), Arg(p.mu_h), Arg(p.mu_v), Arg(p.mu_d), Arg(p.nu),
           Arg(anchorStrength), Arg(nn)});
    };
    // sum of |gY|^2 + |gC|^2, in the region 0 of the sums
    auto sumSq = [&](cl_mem gyMem, cl_mem gcMem) {
      Run(jdot_, global, kGroup, {ArgMem(gyMem), ArgMem(gyMem), ArgMem(gcMem), ArgMem(gcMem), Arg(b_.partial), Arg(0), ArgLocal(kGroup * sizeof(float)), Arg(n)});
      Check(clEnqueueReadBuffer(queue_, b_.partial.get(), CL_TRUE, 0, partialBytes, partial.data(), 0, nullptr, nullptr), "read of a sum");
      double sum = 0.0;
      for (size_t i = 0; i < groups; ++i) sum += static_cast<double>(partial[i]);
      return sum;
    };

    for (int outer = 0; outer < p.irls_outer; ++outer) {
      // the weights at the present (Y, chi): the luma gradients are those of Y itself
      Run(weights_, global, kGroup,
          {ArgMem(yBuf), ArgMem(chiBuf), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(W), Arg(H), Arg(p.eps), Arg(p.eps_c), Arg(p.k),
           Arg(diag ? 1 : 0)});
      if (nn > 0) {
        for (int j = 0; j < nn; ++j) {
          Run(jnbrWeight_, global, kGroup,
              {ArgMem(yBuf), ArgMem(chiBuf), Arg(b_.comp), Arg(b_.dc), Arg(b_.conf), Arg(b_.wt), Arg(b_.scale), Arg(n), Arg(et2), Arg(p.nu), Arg(j == 0 ? 1 : 0),
               Arg(static_cast<int>(static_cast<size_t>(j) * N))});
        }
        Run(scale_, global, kGroup, {Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.scale), Arg(n), Arg(diag ? 1 : 0)});
      }

      gradOf(gyBuf, gcBuf);
      Run(jneg_, global, kGroup, {Arg(b_.dyb), Arg(b_.d), ArgMem(gyBuf), ArgMem(gcBuf), Arg(n)});
      double gg = sumSq(gyBuf, gcBuf);
      st.Release();  // (the sum has come back: the device has read the staging block)
      const double gg0 = gg;
      for (int it = 0; it < p.n_inner; ++it) {
        Run(jcurv_, global, kGroup,
            {Arg(b_.dyb), Arg(b_.d), Arg(b_.car), Arg(b_.wx), Arg(b_.wy), Arg(b_.wcx), Arg(b_.wcy), Arg(b_.wd1), Arg(b_.wd2), Arg(b_.dc), Arg(b_.wt), Arg(b_.aw),
             Arg(b_.partial), Arg(0), ArgLocal(kGroup * sizeof(float)), Arg(W), Arg(H), Arg(p.mu_h), Arg(p.mu_v), Arg(p.mu_d), Arg(p.nu), Arg(anchorStrength),
             Arg(nn)});
        Run(jdot_, global, kGroup,
            {ArgMem(gyBuf), Arg(b_.dyb), ArgMem(gcBuf), Arg(b_.d), Arg(b_.partial), Arg(static_cast<int>(groups)), ArgLocal(kGroup * sizeof(float)), Arg(n)});
        Check(clEnqueueReadBuffer(queue_, b_.partial.get(), CL_TRUE, 0, 2 * partialBytes, partial.data(), 0, nullptr, nullptr), "read of the sums");
        double hh = 0.0;
        double gd = 0.0;
        for (size_t i = 0; i < groups; ++i) {
          hh += static_cast<double>(partial[i]);
          gd += static_cast<double>(partial[groups + i]);
        }
        if (hh <= 1e-12) break;
        const float alpha = static_cast<float>(-0.5 * gd / hh);
        Run(jaxpy_, global, kGroup, {ArgMem(yBuf), ArgMem(chiBuf), Arg(b_.dyb), Arg(b_.d), Arg(alpha), Arg(n)});

        gradOf(gyNew, gcNew);
        const double ggNew = sumSq(gyNew, gcNew);
        const float beta = static_cast<float>(ggNew / std::max(gg, 1e-30));
        Run(jdupdate_, global, kGroup, {Arg(b_.dyb), Arg(b_.d), ArgMem(gyNew), ArgMem(gcNew), Arg(beta), Arg(n)});
        std::swap(gyBuf, gyNew);
        std::swap(gcBuf, gcNew);
        gg = ggNew;
        if (gg < static_cast<double>(p.cg_tol) * p.cg_tol * gg0) break;
        if (gg < 1e-10 * nPixels) break;
      }
    }
    Check(clEnqueueReadBuffer(queue_, yBuf, CL_TRUE, 0, N * sizeof(float), y->data(), 0, nullptr, nullptr), "read of the luma");
    g_computeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tp1).count();
    Check(clEnqueueReadBuffer(queue_, chiBuf, CL_TRUE, 0, N * sizeof(Complex), chi->data(), 0, nullptr, nullptr), "read of the chroma");
  }

 private:
  // An argument of a kernel: a value, a buffer, or a local memory
  struct KernelArg {
    enum Kind { kValue, kMem, kLocal } kind;
    cl_mem mem = nullptr;
    size_t size = 0;
    unsigned char value[8] = {0};
  };
  template <typename T>
  static KernelArg Arg(T v) {
    KernelArg a{KernelArg::kValue};
    a.size = sizeof(T);
    std::memcpy(a.value, &v, sizeof(T));
    return a;
  }
  static KernelArg Arg(const Buffer& b) {
    KernelArg a{KernelArg::kMem};
    a.mem = b.get();
    return a;
  }
  static KernelArg ArgMem(cl_mem m) {
    KernelArg a{KernelArg::kMem};
    a.mem = m;
    return a;
  }
  static KernelArg ArgLocal(size_t bytes) {
    KernelArg a{KernelArg::kLocal};
    a.size = bytes;
    return a;
  }
  cl_kernel Kernel(const char* name) {
    cl_int error = CL_SUCCESS;
    cl_kernel k = clCreateKernel(program_, name, &error);
    Check(error, name);
    return k;
  }

  void Run(cl_kernel kernel, size_t global, size_t local, const std::vector<KernelArg>& args) {
    cl_uint index = 0;
    for (const KernelArg& a : args) {
      switch (a.kind) {
        case KernelArg::kValue:
          Check(clSetKernelArg(kernel, index, a.size, a.value), "clSetKernelArg");
          break;
        case KernelArg::kMem: {
          Check(clSetKernelArg(kernel, index, sizeof(cl_mem), &a.mem), "clSetKernelArg");
          break;
        }
        case KernelArg::kLocal:
          Check(clSetKernelArg(kernel, index, a.size, nullptr), "clSetKernelArg");
          break;
      }
      ++index;
    }
    Check(clEnqueueNDRangeKernel(queue_, kernel, 1, nullptr, &global, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
  }

  void Write(const Buffer& b, const void* data, size_t bytes, size_t byteOffset = 0) {
    Check(clEnqueueWriteBuffer(queue_, b.get(), CL_TRUE, byteOffset, bytes, data, 0, nullptr, nullptr), "clEnqueueWriteBuffer");
  }

  // From the staging block, without waiting: the queue goes in order, and the block is given back when the first sum has come back
  void WriteAsync(const Buffer& b, const void* data, size_t bytes, size_t byteOffset = 0) {
    Check(clEnqueueWriteBuffer(queue_, b.get(), CL_FALSE, byteOffset, bytes, data, 0, nullptr, nullptr), "clEnqueueWriteBuffer");
  }

  // The buffers for planes of N pixels and nn neighbours (kept for the next solve of the same size, which is every one of a run)
  void Prepare(size_t N, size_t nn, size_t groups) {
    if (b_ready_ && n_ == N && nn_ == nn) return;
    const size_t f = sizeof(float);
    const size_t c = sizeof(Complex);
    b_.s.Allocate(context_, N * f);
    b_.car.Allocate(context_, N * c);
    b_.chi.Allocate(context_, N * c);
    b_.g.Allocate(context_, N * c);
    b_.gn.Allocate(context_, N * c);
    b_.d.Allocate(context_, N * c);
    b_.yc.Allocate(context_, N * f);
    b_.dyb.Allocate(context_, N * f);
    b_.wx.Allocate(context_, N * f);
    b_.wy.Allocate(context_, N * f);
    b_.wcx.Allocate(context_, N * f);
    b_.wcy.Allocate(context_, N * f);
    b_.wd1.Allocate(context_, N * f);
    b_.wd2.Allocate(context_, N * f);
    b_.scale.Allocate(context_, N * f);
    b_.partial.Allocate(context_, 2 * groups * f);
    b_.cw.Allocate(context_, N * c);
    b_.gy.Allocate(context_, N * f);
    b_.gyn.Allocate(context_, N * f);
    b_.aw.Allocate(context_, N * f);
    b_.ay.Allocate(context_, N * f);
    b_.ac.Allocate(context_, N * c);
    b_.comp.Allocate(context_, std::max<size_t>(nn, 1) * N * f);
    b_.conf.Allocate(context_, std::max<size_t>(nn, 1) * N * f);
    b_.wt.Allocate(context_, std::max<size_t>(nn, 1) * N * f);
    b_.dc.Allocate(context_, std::max<size_t>(nn, 1) * N * c);
    n_ = N;
    nn_ = nn;
    b_ready_ = true;
  }

  struct Buffers {
    Buffer s, car, chi, g, gn, d, yc, dyb, wx, wy, wcx, wcy, wd1, wd2, scale, partial, cw, comp, conf, wt, dc, gy, gyn, aw, ay, ac;
  };

  cl_device_id device_ = nullptr;
  cl_context context_ = nullptr;
  cl_command_queue queue_ = nullptr;
  cl_program program_ = nullptr;
  cl_kernel luma_ = nullptr, makeDc_ = nullptr, weights_ = nullptr, nbrWeight_ = nullptr, scale_ = nullptr, grad_ = nullptr, dy_ = nullptr, curv_ = nullptr,
            dot_ = nullptr, neg_ = nullptr, axpy_ = nullptr, dupdate_ = nullptr, jnbrWeight_ = nullptr, jgrad_ = nullptr, jcurv_ = nullptr, jdot_ = nullptr,
            jneg_ = nullptr, jaxpy_ = nullptr, jdupdate_ = nullptr;
  std::string description_;
  Buffers b_;
  bool b_ready_ = false;
  size_t n_ = 0;
  size_t nn_ = 0;
};

// The workers: the first one makes the context and the program, the others share them. Each is used by one thread at a time; the threads that find
// them all busy wait. With several, the planes of one solve go to the device while another solve iterates, and the device is not idle while the
// host reads the sums of one (what a single worker did: the device was held for the whole solve, with nothing running most of the time).
std::mutex g_mutex;
std::condition_variable g_free_changed;
Device* g_device = nullptr;  // the first worker; made once and never destroyed: the objects of the driver are not to be released from a static destructor
std::vector<Device*> g_free;  // the workers that nobody is using
bool g_tried = false;
bool g_failed = false;

// What the solves cost, said at the exit when HVD_OPENCL_STATS is set: the time the threads waited for a worker, and the time they held one
struct Stats {
  std::atomic<long long> waitNs{0};
  std::atomic<long long> heldNs{0};
  std::atomic<long long> calls{0};
};
Stats g_stats;

void PrintStats() {
  std::fprintf(stderr, "HVD OpenCL: %lld solves, %.2f s held (uploads %.2f s, iterations %.2f s), %.2f s waited for a worker\n", g_stats.calls.load(),
               g_stats.heldNs.load() / 1e9, g_uploadNs.load() / 1e9, g_computeNs.load() / 1e9, g_stats.waitNs.load() / 1e9);
  std::fprintf(stderr, "HVD OpenCL: device calls %.2f s, copies of the plane %.2f s\n", g_deviceNs.load() / 1e9, g_copyNs.load() / 1e9);
}

class SolveTimer {
 public:
  SolveTimer() : start_(std::chrono::steady_clock::now()), locked_(start_) {}
  void Locked() { locked_ = std::chrono::steady_clock::now(); }
  ~SolveTimer() {
    const auto end = std::chrono::steady_clock::now();
    g_stats.waitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(locked_ - start_).count();
    g_stats.heldNs += std::chrono::duration_cast<std::chrono::nanoseconds>(end - locked_).count();
    ++g_stats.calls;
  }

 private:
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point locked_;
};

// The workers, made at the first use (g_mutex held); false when there is no device or it failed
bool OpenDevice() {
  if (g_failed) return false;
  if (!g_tried) {
    g_tried = true;
    try {
      g_device = new Device;
      if (std::getenv("HVD_OPENCL_STATS")) std::atexit(PrintStats);
      g_free.push_back(g_device);
      long count = 1;  // (more than one worker gave nothing on the NVIDIA driver: the solves wait on the host side; HVD_OPENCL_QUEUES tries more)
      if (const char* wanted = std::getenv("HVD_OPENCL_QUEUES")) count = std::strtol(wanted, nullptr, 10);
      count = std::min<long>(std::max<long>(count, 1), 16);
      for (long i = 1; i < count; ++i) {
        try {
          g_free.push_back(new Device(*g_device, 0));
        } catch (const std::exception&) {
          break;  // the ones that were made are enough
        }
      }
    } catch (const std::exception& e) {
      g_failed = true;
      if (std::getenv("HVD_OPENCL_VERBOSE")) std::fprintf(stderr, "HVD OpenCL: %s\n", e.what());
    }
  }
  return g_device != nullptr && !g_failed;
}

// A worker for the thread, waiting for one to be free; null when there is no device (or it failed)
Device* Acquire() {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (!OpenDevice()) return nullptr;
  g_free_changed.wait(lock, [] { return !g_free.empty() || g_failed; });
  if (g_failed) return nullptr;
  Device* worker = g_free.back();
  g_free.pop_back();
  return worker;
}

void Release(Device* worker) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_free.push_back(worker);
  }
  g_free_changed.notify_all();
}

// The GPU failed in a solve: it is off for the rest of the run, for every thread
void Failed(const std::exception& e) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_failed) return;
    g_failed = true;
  }
  std::fprintf(stderr, "HVD decoder: the GPU failed (%s), the CPU goes on\n", e.what());
  g_free_changed.notify_all();
}

// A worker in use, given back when the solve is over
struct Lease {
  Device* worker;
  ~Lease() {
    if (worker) Release(worker);
  }
};

// ---- The staging blocks ------------------------------------------------------------------------------------------------------------------------
// The planes of a solve (up to some fifty megabytes with the neighbours) go to the device at three times the speed from pinned memory than from the
// memory of a plane (measured: 54 GB/s against 18). A thread packs them into a block of pinned memory before it has a worker, which is also
// work that the other threads do while the device iterates; the worker writes them from there, and the block is free again when the first sum of
// the iterations has come back (the device has read it by then).
struct StageBlock {
  cl_mem mem = nullptr;
  unsigned char* ptr = nullptr;
  size_t capacity = 0;
};

cl_context g_stage_context = nullptr;
cl_command_queue g_stage_queue = nullptr;  // for the maps of the blocks only
std::vector<StageBlock*> g_stage_free;

// (g_mutex held) a block with room for `bytes`: made again, with some room to spare, when it is too small
void GrowStage(StageBlock* block, size_t bytes) {
  if (block->mem) {
    clEnqueueUnmapMemObject(g_stage_queue, block->mem, block->ptr, 0, nullptr, nullptr);
    clFinish(g_stage_queue);
    clReleaseMemObject(block->mem);
    block->mem = nullptr;
    block->ptr = nullptr;
    block->capacity = 0;
  }
  const size_t size = bytes + bytes / 4;
  cl_int error = CL_SUCCESS;
  cl_mem mem = clCreateBuffer(g_stage_context, CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_ONLY, size, nullptr, &error);
  Check(error, "clCreateBuffer (staging)");
  void* ptr = clEnqueueMapBuffer(g_stage_queue, mem, CL_TRUE, CL_MAP_WRITE, 0, size, 0, nullptr, nullptr, &error);
  if (error != CL_SUCCESS) {
    clReleaseMemObject(mem);
    Check(error, "clEnqueueMapBuffer (staging)");
  }
  block->mem = mem;
  block->ptr = static_cast<unsigned char*>(ptr);
  block->capacity = size;
}

// A block for the thread, waiting for one to be free; null when there is no device (or it failed)
StageBlock* AcquireStage(size_t bytes) {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (!OpenDevice()) return nullptr;
  if (!g_stage_queue) {
    g_stage_context = g_device->context();
    cl_int error = CL_SUCCESS;
    g_stage_queue = clCreateCommandQueue(g_stage_context, g_device->device(), 0, &error);
    Check(error, "clCreateCommandQueue (staging)");
    for (int i = 0; i < 3; ++i) g_stage_free.push_back(new StageBlock);
  }
  g_free_changed.wait(lock, [] { return !g_stage_free.empty() || g_failed; });
  if (g_failed) return nullptr;
  StageBlock* block = g_stage_free.back();
  g_stage_free.pop_back();
  if (block->capacity < bytes) {
    try {
      GrowStage(block, bytes);
    } catch (...) {
      g_stage_free.push_back(block);
      throw;
    }
  }
  return block;
}

void ReleaseStage(StageBlock* block) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stage_free.push_back(block);
  }
  g_free_changed.notify_all();
}

// A block held by a solve: given back by the first sum of the iterations, or at the end of the solve
class StageLease {
 public:
  explicit StageLease(size_t bytes) : block_(AcquireStage(bytes)) {}
  StageLease(const StageLease&) = delete;
  StageLease& operator=(const StageLease&) = delete;
  ~StageLease() { Release(); }
  unsigned char* ptr() const { return block_ ? block_->ptr : nullptr; }
  void Release() {
    if (block_) ReleaseStage(block_);
    block_ = nullptr;
  }
  Staged staged() {
    Staged st;
    st.ptr = ptr();
    st.release = [this] { Release(); };
    return st;
  }

 private:
  StageBlock* block_;
};

// Pack the planes of the neighbours, as the layout says
void PackNeighbours(unsigned char* base, const Layout& L, const std::vector<NeighborTerm>& neighbors, size_t nn, size_t N) {
  for (size_t j = 0; j < nn; ++j) {
    std::memcpy(base + L.comp(j), neighbors[j].composite.data(), N * sizeof(float));
    std::memcpy(base + L.conf(j), neighbors[j].confidence.data(), N * sizeof(float));
    std::memcpy(base + L.cw(j), neighbors[j].carrier.data(), N * sizeof(Complex));
  }
}

}  // namespace

bool Available() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return OpenDevice();
}

std::string DeviceDescription() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return OpenDevice() ? g_device->description() : std::string();
}

bool Solve(const Plane& s, const ComplexPlane& carrier, const SolveParameters& p, const std::vector<NeighborTerm>& neighbors, ComplexPlane* chi) {
  SolveTimer timer;
  const size_t N = s.size();
  const int nn = p.nu > 0.0F ? static_cast<int>(neighbors.size()) : 0;
  const Layout L = MakeLayout(N, static_cast<size_t>(nn), false, false);
  try {
    StageLease stage(L.total);
    if (!stage.ptr()) return false;
    std::memcpy(stage.ptr() + L.s, s.data(), N * sizeof(float));
    std::memcpy(stage.ptr() + L.car, carrier.data(), N * sizeof(Complex));
    std::memcpy(stage.ptr() + L.chi, chi->data(), N * sizeof(Complex));
    PackNeighbours(stage.ptr(), L, neighbors, static_cast<size_t>(nn), N);
    Lease lease{Acquire()};
    timer.Locked();
    if (!lease.worker) return false;
    const auto tc0 = std::chrono::steady_clock::now();
    ComplexPlane result = *chi;
    const auto tc1 = std::chrono::steady_clock::now();
    lease.worker->Solve(s.height(), s.width(), p, nn, stage.staged(), &result);
    const auto tc2 = std::chrono::steady_clock::now();
    *chi = std::move(result);
    g_copyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tc1 - tc0).count();
    g_deviceNs += std::chrono::duration_cast<std::chrono::nanoseconds>(tc2 - tc1).count();
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool SolveJoint(const Plane& s, const ComplexPlane& carrier, const SolveParameters& p, float nu_a, const std::vector<NeighborTerm>& neighbors, const AnchorTerm* anchor,
                Plane* y, ComplexPlane* chi) {
  SolveTimer timer;
  const size_t N = s.size();
  const int nn = p.nu > 0.0F ? static_cast<int>(neighbors.size()) : 0;
  const bool withAnchor = nu_a > 0.0F && anchor != nullptr;
  const Layout L = MakeLayout(N, static_cast<size_t>(nn), true, withAnchor);
  try {
    StageLease stage(L.total);
    if (!stage.ptr()) return false;
    std::memcpy(stage.ptr() + L.s, s.data(), N * sizeof(float));
    std::memcpy(stage.ptr() + L.car, carrier.data(), N * sizeof(Complex));
    std::memcpy(stage.ptr() + L.chi, chi->data(), N * sizeof(Complex));
    std::memcpy(stage.ptr() + L.y, y->data(), N * sizeof(float));
    if (withAnchor) {
      std::memcpy(stage.ptr() + L.aw, anchor->weight.data(), N * sizeof(float));
      std::memcpy(stage.ptr() + L.ay, anchor->luma.data(), N * sizeof(float));
      std::memcpy(stage.ptr() + L.ac, anchor->chroma.data(), N * sizeof(Complex));
    }
    PackNeighbours(stage.ptr(), L, neighbors, static_cast<size_t>(nn), N);
    Lease lease{Acquire()};
    timer.Locked();
    if (!lease.worker) return false;
    Plane yResult = *y;
    ComplexPlane chiResult = *chi;
    lease.worker->SolveJoint(s.height(), s.width(), p, nu_a, nn, withAnchor, stage.staged(), &yResult, &chiResult);
    *y = std::move(yResult);
    *chi = std::move(chiResult);
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

}  // namespace gpu
}  // namespace hvd

#else  // no OpenCL in the build

namespace hvd {
namespace gpu {

bool Available() { return false; }
std::string DeviceDescription() { return std::string(); }
bool Solve(const Plane&, const ComplexPlane&, const SolveParameters&, const std::vector<NeighborTerm>&, ComplexPlane*) { return false; }
bool SolveJoint(const Plane&, const ComplexPlane&, const SolveParameters&, float, const std::vector<NeighborTerm>&, const AnchorTerm*, Plane*, ComplexPlane*) { return false; }

}  // namespace gpu
}  // namespace hvd

#endif
