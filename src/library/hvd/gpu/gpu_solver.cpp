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
#include <limits>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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
std::atomic<long long> g_motionCalls{0};

// HVD_OPENCL_PROFILE: the time of each kernel on the device, from the events of the queue, said at the exit
bool g_profile = false;
std::mutex g_profile_mutex;
std::vector<std::pair<std::string, cl_event>> g_profile_events;

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


const char* const kSourceWindow = R"CLC(
// ---- The planes of a window on the device: the motion compensation of the neighbours, the gates of their confidence, the anchor ---------------------

// The tile that brackets a pixel row or column, and the weight between the two tile centres (VectorsPerPixel of temporal.cpp)
inline void bracket(int coord, int n, int tile, int* lo, float* frac) {
  if (n > 1) {
    const float rel = (float)coord / (float)tile - 0.5f;
    const int l = clamp((int)floor(rel), 0, n - 2);
    const float centre = ((float)l + 0.5f) * (float)tile;
    *lo = l;
    *frac = clamp(((float)coord - centre) / (float)tile, 0.0f, 1.0f);
  } else {
    *lo = 0;
    *frac = 0.0f;
  }
}

inline float interp_tile(__global const float* v, int iy, int ix, int iy1, int ix1, int tw, float fy, float fx) {
  const float v00 = v[iy * tw + ix];
  const float v10 = v[iy1 * tw + ix];
  const float v01 = v[iy * tw + ix1];
  const float v11 = v[iy1 * tw + ix1];
  return v00 * (1 - fy) * (1 - fx) + v10 * fy * (1 - fx) + v01 * (1 - fy) * fx + v11 * fy * fx;
}

// The two tile grids of vectors (already cleaned of their outliers by the host) to a vector at each pixel
__kernel void k_interp2(__global const float* tiles, int oy, int ox, __global float* ody, __global float* odx, int th, int tw, int tile, int H, int W) {
  __global const float* mdy = tiles + oy;
  __global const float* mdx = tiles + ox;
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  int iy, ix;
  float fy, fx;
  bracket(y, th, tile, &iy, &fy);
  bracket(x, tw, tile, &ix, &fx);
  const int iy1 = min(iy + 1, th - 1);
  const int ix1 = min(ix + 1, tw - 1);
  ody[i] = interp_tile(mdy, iy, ix, iy1, ix1, tw, fy, fx);
  odx[i] = interp_tile(mdx, iy, ix, iy1, ix1, tw, fy, fx);
}

__kernel void k_interp1(__global const float* tiles, int off, __global float* out, int th, int tw, int tile, int H, int W, int base) {
  __global const float* grid = tiles + off;
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  int iy, ix;
  float fy, fx;
  bracket(y, th, tile, &iy, &fy);
  bracket(x, tw, tile, &ix, &fx);
  out[base + i] = interp_tile(grid, iy, ix, min(iy + 1, th - 1), min(ix + 1, tw - 1), tw, fy, fx);
}

// The confidence of a tile, squared, at each pixel by the tile it is in (the first step of UpsampleConfidence)
__kernel void k_conf_nearest(__global const float* tiles, int off, __global float* out, int th, int tw, int tile, int H, int W) {
  __global const float* grid = tiles + off;
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const float c = grid[min(y / tile, th - 1) * tw + min(x / tile, tw - 1)];
  out[i] = c * c;
}

// The raw warp: the pixel at the vector rounded to the nearest whole pixel
__kernel void k_warp_int(__global const float* src, __global const float* vdy, __global const float* vdx, __global float* dst, int H, int W, int base) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int sy = clamp(y - (int)round(vdy[i]), 0, H - 1);
  const int sx = clamp(x - (int)round(vdx[i]), 0, W - 1);
  dst[base + i] = src[sy * W + sx];
}

__kernel void k_warp_int_c(__global const float2* src, __global const float* vdy, __global const float* vdx, __global float2* dst, int H, int W, int base) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int sy = clamp(y - (int)round(vdy[i]), 0, H - 1);
  const int sx = clamp(x - (int)round(vdx[i]), 0, W - 1);
  dst[base + i] = src[sy * W + sx];
}

// The bilinear warp (WarpBilinearTiles), for a plane and for a complex plane
__kernel void k_warp_bil(__global const float* src, __global const float* vdy, __global const float* vdx, __global float* dst, int H, int W, float row_off) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  float sy = (float)y - vdy[i] + row_off;
  float sx = (float)x - vdx[i];
  sy = clamp(sy, 0.0f, (float)H - 1.001f);
  sx = clamp(sx, 0.0f, (float)W - 1.001f);
  const int y0 = (int)sy;
  const int x0 = (int)sx;
  const float fy = sy - (float)y0;
  const float fx = sx - (float)x0;
  const int y1 = min(y0 + 1, H - 1);
  const int x1 = min(x0 + 1, W - 1);
  dst[i] = src[y0 * W + x0] * (1 - fy) * (1 - fx) + src[y1 * W + x0] * fy * (1 - fx) + src[y0 * W + x1] * (1 - fy) * fx + src[y1 * W + x1] * fy * fx;
}

__kernel void k_warp_bil_c(__global const float2* src, __global const float* vdy, __global const float* vdx, __global float2* dst, int H, int W, float row_off) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  float sy = (float)y - vdy[i] + row_off;
  float sx = (float)x - vdx[i];
  sy = clamp(sy, 0.0f, (float)H - 1.001f);
  sx = clamp(sx, 0.0f, (float)W - 1.001f);
  const int y0 = (int)sy;
  const int x0 = (int)sx;
  const float fy = sy - (float)y0;
  const float fx = sx - (float)x0;
  const int y1 = min(y0 + 1, H - 1);
  const int x1 = min(x0 + 1, W - 1);
  dst[i] = src[y0 * W + x0] * (1 - fy) * (1 - fx) + src[y1 * W + x0] * fy * (1 - fx) + src[y0 * W + x1] * (1 - fy) * fx + src[y1 * W + x1] * fy * fx;
}

// The box blur of radius r, zero-padded, the sum over the clipped window times 1/(2r+1): along the columns of the picture (vertical) and along its rows
__kernel void k_box_v(__global const float* src, __global float* dst, int H, int W, int r) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int y0 = max(y - r, 0);
  const int y1 = min(y + r + 1, H);
  float acc = 0.0f;
  for (int yy = y0; yy < y1; ++yy) acc += src[yy * W + x];
  dst[i] = acc * (1.0f / (float)(2 * r + 1));
}

__kernel void k_box_h(__global const float* src, __global float* dst, int H, int W, int r) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int x0 = max(x - r, 0);
  const int x1 = min(x + r + 1, W);
  float acc = 0.0f;
  for (int xx = x0; xx < x1; ++xx) acc += src[y * W + xx];
  dst[i] = acc * (1.0f / (float)(2 * r + 1));
}

// ComplexCoherence: the planes to blur, and the coherence from the blurred ones
__kernel void k_coh_prep(__global const float2* z1, __global const float2* z2, __global float* cre, __global float* cim, __global float* m1, __global float* m2, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float2 a = z1[i];
  const float2 b = z2[i];
  cre[i] = a.x * b.x + a.y * b.y;  // Re[z1 conj(z2)]
  cim[i] = a.y * b.x - a.x * b.y;  // Im[z1 conj(z2)]
  m1[i] = a.x * a.x + a.y * a.y;
  m2[i] = b.x * b.x + b.y * b.y;
}

__kernel void k_coh_fin(__global const float* bre, __global const float* bim, __global const float* b1, __global const float* b2, __global float* out, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float num = sqrt(bre[i] * bre[i] + bim[i] * bim[i]);
  const float den = sqrt(max(0.0f, b1[i] * b2[i])) + 1e-6f;
  out[i] = clamp(num / den, 0.0f, 1.0f);
}

// conf *= (1 - a) + a gamma
__kernel void k_conf_gate(__global float* conf, __global const float* gamma, float a, int N, int base) {
  const int i = get_global_id(0);
  if (i >= N) return;
  conf[base + i] *= (1.0f - a) + a * gamma[i];
}

// conf *= max(floor_i, e2 / (e2 + vgrad^2)), the floor being the ceiling of the configuration times the multiplier of the pixel
__kernel void k_conf_odd(__global float* conf, __global const float* vgrad, __global const float* lapsm, float e2, float floor_max, int N, int base) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float b = vgrad[i];
  const float floor_i = floor_max * lapsm[i];
  conf[base + i] *= max(floor_i, e2 / (e2 + b * b));
}

// The vertical envelope of the field (its luma and chroma): the one-sided maximum of the squared differences, and the squared second difference
__kernel void k_oddmaps(__global const float* Y, __global const float2* chi, __global float* gmag, __global float* lapmag, int H, int W) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int ym = max(0, y - 1);
  const int yp = min(H - 1, y + 1);
  const int im = ym * W + x;
  const int ip = yp * W + x;
  const float dyu = Y[i] - Y[im];
  const float dyd = Y[ip] - Y[i];
  const float dcu = length(chi[i] - chi[im]);
  const float dcd = length(chi[ip] - chi[i]);
  const float mu = dyu * dyu + dcu * dcu;
  const float md = dyd * dyd + dcd * dcd;
  gmag[i] = max(mu, md);
  const float ly = Y[i] - 0.5f * (Y[im] + Y[ip]);
  const float lc = length(chi[i] - 0.5f * (chi[im] + chi[ip]));
  lapmag[i] = ly * ly + lc * lc;
}

// The smoothing along the row over [x-2, x+2], clipped to the row: the root of the mean
__kernel void k_smooth_rows(__global const float* src, __global float* dst, int H, int W) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int lo = max(x - 2, 0);
  const int hi = min(x + 2, W - 1);
  float acc = 0.0f;
  for (int xx = lo; xx <= hi; ++xx) acc += src[y * W + xx];
  dst[i] = sqrt(acc / (float)(hi - lo + 1));
}

// Every seventh value of the envelope, for the host to take the median of
__kernel void k_gather7(__global const float* src, __global float* dst, int N) {
  const int k = get_global_id(0);
  const int i = 7 * k;
  if (i < N) dst[k] = src[i];
}

// The envelope without its noise floor: sqrt(max(0, v^2 - fl2))
__kernel void k_floor(__global float* v, float fl2, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  v[i] = sqrt(max(0.0f, v[i] * v[i] - fl2));
}

// The multiplier of the floor of the gate: 1 - t^2, t being the second difference over the gradient (0 where there is no gradient)
__kernel void k_lapsm(__global const float* traw, __global const float* vgrad, __global float* lapsm, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float g = vgrad[i];
  const float t = g > 1e-6f ? clamp(traw[i] / g, 0.0f, 1.0f) : 0.0f;
  lapsm[i] = 1.0f - t * t;
}

// ---- the anchor of the passes after the first: the neighbours of the field blended, weighted by their agreement ----------------------------------

__kernel void k_synth_init(__global const float* Y, __global const float2* chi, __global float* accY, __global float2* accC, __global float* accW, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  accY[i] = Y[i];
  accC[i] = chi[i];
  accW[i] = 1.0f;
}

// One neighbour: yw and cw are its luma and chroma warped to the field, conf its confidence
__kernel void k_synth_accum(__global const float* yw, __global const float2* cw, __global const float* Yj, __global const float2* chij, __global const float* conf,
                            __global float* accY, __global float2* accC, __global float* accW, float eb2, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float dy = yw[i] - Yj[i];
  const float2 dc = cw[i] - chij[i];
  const float d2 = dy * dy + dc.x * dc.x + dc.y * dc.y;
  const float w = conf[i] * eb2 / (d2 + eb2);
  accY[i] += w * yw[i];
  accC[i] += w * cw[i];
  accW[i] += w;
}

__kernel void k_synth_fin(__global const float* accY, __global const float2* accC, __global const float* accW, __global const float2* car, __global const float* S,
                          __global float* ay, __global float2* ac, __global float* resid, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  const float luma = accY[i] / accW[i];
  const float2 chroma = accC[i] / accW[i];
  ay[i] = luma;
  ac[i] = chroma;
  const float s_hat = luma + (chroma.x * car[i].x - chroma.y * car[i].y);
  resid[i] = fabs(S[i] - s_hat);
}

__kernel void k_synth_weight(__global const float* r, __global float* aw, float eb2, int N) {
  const int i = get_global_id(0);
  if (i >= N) return;
  aw[i] = eb2 / (r[i] * r[i] + eb2);
}

// ---- The search for the motion on the luma that is on the device (EstimateMotion and VerifyMotion of motion.cpp) -----------------------------------

// The f x f block average of the picture, cropped to a whole number of blocks
__kernel void k_decimate(__global const float* src, __global float* dst, int H, int W, int f) {
  const int hh = H / f;
  const int ww = W / f;
  const int i = get_global_id(0);
  if (i >= hh * ww) return;
  const int y = i / ww;
  const int x = i % ww;
  float acc = 0.0f;
  for (int dy = 0; dy < f; ++dy) {
    for (int dx = 0; dx < f; ++dx) acc += src[(y * f + dy) * W + x * f + dx];
  }
  dst[i] = acc * (1.0f / (float)(f * f));
}

// The squared difference of a tile of A with the tile of B that its shift points at (the reference clamped to its edges), for each shift of each tile: one
// group for each. shifts[g] = (sy, sx) for the tile g % (th * tw) of the candidate g / (th * tw).
__kernel void k_bm_cost(__global const float* A, __global const float* B, int H, int W, int tile, int th, int tw, __global const int2* shifts, __global float* costs,
                        __local float* sh) {
  const int g = get_group_id(0);
  const int t = g % (th * tw);
  const int ty = t / tw;
  const int tx = t % tw;
  const int2 s = shifts[g];
  const int y0 = ty * tile;
  const int y1 = min(y0 + tile, H);
  const int x0 = tx * tile;
  const int x1 = min(x0 + tile, W);
  const int bw = x1 - x0;
  const int total = bw * (y1 - y0);
  float acc = 0.0f;
  for (int p = get_local_id(0); p < total; p += get_local_size(0)) {
    const int y = y0 + p / bw;
    const int x = x0 + p % bw;
    const int yy = clamp(y - s.x, 0, H - 1);
    const int xx = clamp(x - s.y, 0, W - 1);
    const float d = A[y * W + x] - B[yy * W + xx];
    acc += d * d;
  }
  const int l = get_local_id(0);
  sh[l] = acc;
  barrier(CLK_LOCAL_MEM_FENCE);
  for (int step = GS / 2; step > 0; step >>= 1) {
    if (l < step) sh[l] += sh[l + step];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if (l == 0) costs[g] = sh[0];
}

// The sum and the sum of squares of each tile of A, for the confidence of the motion
__kernel void k_tile_stats(__global const float* A, int H, int W, int tile, int th, int tw, __global float* sums, __global float* sums2, __local float* sh) {
  const int t = get_group_id(0);
  const int ty = t / tw;
  const int tx = t % tw;
  const int y0 = ty * tile;
  const int y1 = min(y0 + tile, H);
  const int x0 = tx * tile;
  const int x1 = min(x0 + tile, W);
  const int bw = x1 - x0;
  const int total = bw * (y1 - y0);
  float a = 0.0f;
  float b = 0.0f;
  for (int p = get_local_id(0); p < total; p += get_local_size(0)) {
    const float v = A[(y0 + p / bw) * W + x0 + p % bw];
    a += v;
    b += v * v;
  }
  const int l = get_local_id(0);
  sh[l] = a;
  sh[GS + l] = b;
  barrier(CLK_LOCAL_MEM_FENCE);
  for (int step = GS / 2; step > 0; step >>= 1) {
    if (l < step) {
      sh[l] += sh[l + step];
      sh[GS + l] += sh[GS + l + step];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if (l == 0) {
    sums[t] = sh[0];
    sums2[t] = sh[GS];
  }
}

// ---- Fused kernels: the same arithmetic, in fewer launches (the kernels are small, a launch costs as much as one) ---------------------------------

// The motion compensation of a neighbour in one pass: the vector at each pixel from the tile grids, the confidence at each pixel, the raw warp of its composite and
// of its carrier (to the carrier itself for the joint solve, to the difference with the carrier of the field for the other)
__kernel void k_mc(__global const float* tiles, int oy, int ox, int oc, int th, int tw, int tile, int H, int W, __global const float* ks, __global const float2* kcar,
                   __global const float2* car, __global float* vdy, __global float* vdx, __global float* conf, __global float* comp, __global float2* dst, int base,
                   int joint) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  int iy, ix;
  float fy, fx;
  bracket(y, th, tile, &iy, &fy);
  bracket(x, tw, tile, &ix, &fx);
  const int iy1 = min(iy + 1, th - 1);
  const int ix1 = min(ix + 1, tw - 1);
  const float vy = interp_tile(tiles + oy, iy, ix, iy1, ix1, tw, fy, fx);
  const float vx = interp_tile(tiles + ox, iy, ix, iy1, ix1, tw, fy, fx);
  vdy[i] = vy;
  vdx[i] = vx;
  conf[base + i] = interp_tile(tiles + oc, iy, ix, iy1, ix1, tw, fy, fx);
  const int sy = clamp(y - (int)round(vy), 0, H - 1);
  const int sx = clamp(x - (int)round(vx), 0, W - 1);
  comp[base + i] = ks[sy * W + sx];
  const float2 c = kcar[sy * W + sx];
  dst[base + i] = joint ? c : (car[i] - c);
}

// The coherence of the chroma of the field with the chroma of a neighbour warped to it: the vertical sums of the four planes that it blurs (the cross product,
// the two energies), then the horizontal sums, the coherence and its gate on the confidence
__kernel void k_coh_v(__global const float2* z1, __global const float2* z2, __global float* cre, __global float* cim, __global float* m1, __global float* m2, int H,
                      int W, int r) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int y0 = max(y - r, 0);
  const int y1 = min(y + r + 1, H);
  float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
  for (int yy = y0; yy < y1; ++yy) {
    const float2 p = z1[yy * W + x];
    const float2 q = z2[yy * W + x];
    a += p.x * q.x + p.y * q.y;
    b += p.y * q.x - p.x * q.y;
    c += p.x * p.x + p.y * p.y;
    d += q.x * q.x + q.y * q.y;
  }
  const float nrm = 1.0f / (float)(2 * r + 1);
  cre[i] = a * nrm;
  cim[i] = b * nrm;
  m1[i] = c * nrm;
  m2[i] = d * nrm;
}

__kernel void k_coh_h(__global const float* cre, __global const float* cim, __global const float* m1, __global const float* m2, __global float* conf, float a, int H,
                      int W, int r, int base) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  const int x0 = max(x - r, 0);
  const int x1 = min(x + r + 1, W);
  float sa = 0.0f, sb = 0.0f, sc = 0.0f, sd = 0.0f;
  for (int xx = x0; xx < x1; ++xx) {
    sa += cre[y * W + xx];
    sb += cim[y * W + xx];
    sc += m1[y * W + xx];
    sd += m2[y * W + xx];
  }
  const float nrm = 1.0f / (float)(2 * r + 1);
  const float bre = sa * nrm;
  const float bim = sb * nrm;
  const float b1 = sc * nrm;
  const float b2 = sd * nrm;
  const float num = sqrt(bre * bre + bim * bim);
  const float den = sqrt(max(0.0f, b1 * b2)) + 1e-6f;
  const float gamma = clamp(num / den, 0.0f, 1.0f);
  conf[base + i] *= (1.0f - a) + a * gamma;
}

// The robust weights of all the neighbours at once, and the renormalisation of the priors
__kernel void k_nbr_weight_all(__global const float* s, __global const float2* chi, __global const float* comp, __global const float2* dc, __global const float* conf,
                               __global float* wt, __global float* scale, int N, float et2, float nu, int nn) {
  const int i = get_global_id(0);
  if (i >= N) return;
  float acc = 1.0f;
  for (int j = 0; j < nn; ++j) {
    const size_t o = (size_t)j * (size_t)N + (size_t)i;
    const float rt = (comp[o] - s[i]) + rmul(chi[i], dc[o]);
    const float w = conf[o] * et2 / (rt * rt + et2);
    wt[o] = w;
    acc = acc + nu * w * cabs2(dc[o]);
  }
  scale[i] = acc;
}

__kernel void k_jnbr_weight_all(__global const float* Y, __global const float2* chi, __global const float* comp, __global const float2* cw, __global const float* conf,
                                __global float* wt, __global float* scale, int N, float et2, float nu, int nn) {
  const int i = get_global_id(0);
  if (i >= N) return;
  float acc = 1.0f;
  for (int j = 0; j < nn; ++j) {
    const size_t o = (size_t)j * (size_t)N + (size_t)i;
    const float r = comp[o] - Y[i] - rmul(chi[i], cw[o]);
    const float w = conf[o] * et2 / (r * r + et2);
    wt[o] = w;
    acc = acc + nu * w;
  }
  scale[i] = acc;
}

// The vector at each pixel and the squared confidence of the tile that it is in, for a neighbour of the anchor
__kernel void k_anchor_prep(__global const float* tiles, int oy, int ox, int oc, int th, int tw, int tile, int H, int W, __global float* vdy, __global float* vdx,
                            __global float* confS) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  int iy, ix;
  float fy, fx;
  bracket(y, th, tile, &iy, &fy);
  bracket(x, tw, tile, &ix, &fx);
  const int iy1 = min(iy + 1, th - 1);
  const int ix1 = min(ix + 1, tw - 1);
  vdy[i] = interp_tile(tiles + oy, iy, ix, iy1, ix1, tw, fy, fx);
  vdx[i] = interp_tile(tiles + ox, iy, ix, iy1, ix1, tw, fy, fx);
  const float c = tiles[oc + min(y / tile, th - 1) * tw + min(x / tile, tw - 1)];
  confS[i] = c * c;
}

// One neighbour of the anchor: the luma and the chroma of the neighbour warped (bilinear) to the field, and added to the blend
__kernel void k_synth_step(__global const float* Yk, __global const float2* chik, __global const float* vdy, __global const float* vdx, float row_off, int H, int W,
                           __global const float* Yj, __global const float2* chij, __global const float* conf, __global float* accY, __global float2* accC,
                           __global float* accW, float eb2) {
  const int i = get_global_id(0);
  if (i >= H * W) return;
  const int y = i / W;
  const int x = i % W;
  float sy = (float)y - vdy[i] + row_off;
  float sx = (float)x - vdx[i];
  sy = clamp(sy, 0.0f, (float)H - 1.001f);
  sx = clamp(sx, 0.0f, (float)W - 1.001f);
  const int y0 = (int)sy;
  const int x0 = (int)sx;
  const float fy = sy - (float)y0;
  const float fx = sx - (float)x0;
  const int y1 = min(y0 + 1, H - 1);
  const int x1 = min(x0 + 1, W - 1);
  const float yw = Yk[y0 * W + x0] * (1 - fy) * (1 - fx) + Yk[y1 * W + x0] * fy * (1 - fx) + Yk[y0 * W + x1] * (1 - fy) * fx + Yk[y1 * W + x1] * fy * fx;
  const float2 cw = chik[y0 * W + x0] * (1 - fy) * (1 - fx) + chik[y1 * W + x0] * fy * (1 - fx) + chik[y0 * W + x1] * (1 - fy) * fx + chik[y1 * W + x1] * fy * fx;
  const float dy = yw - Yj[i];
  const float2 dc = cw - chij[i];
  const float d2 = dy * dy + dc.x * dc.x + dc.y * dc.y;
  const float w = conf[i] * eb2 / (d2 + eb2);
  accY[i] += w * yw;
  accC[i] += w * cw;
  accW[i] += w;
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
// The buffers of a resident window that a solve writes: the chroma and the luma of the field
struct Resident {
  cl_mem chi = nullptr;
  cl_mem y = nullptr;
};

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
    queue_ = clCreateCommandQueue(context_, device_, g_profile ? CL_QUEUE_PROFILING_ENABLE : 0, &error);
    Check(error, "clCreateCommandQueue");
    const char* sources[2] = {kSource, kSourceWindow};
    program_ = clCreateProgramWithSource(context_, 2, sources, nullptr, &error);
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
    queue_ = clCreateCommandQueue(context_, device_, g_profile ? CL_QUEUE_PROFILING_ENABLE : 0, &error);
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
    for (const char* name : {"k_interp2", "k_interp1", "k_conf_nearest", "k_warp_int", "k_warp_int_c", "k_warp_bil", "k_warp_bil_c", "k_box_v", "k_box_h", "k_coh_prep", "k_coh_fin", "k_conf_gate", "k_conf_odd", "k_oddmaps", "k_smooth_rows", "k_gather7", "k_floor", "k_lapsm", "k_synth_init", "k_synth_accum", "k_synth_fin", "k_synth_weight", "k_decimate", "k_bm_cost", "k_tile_stats", "k_mc", "k_coh_v", "k_coh_h", "k_nbr_weight_all", "k_jnbr_weight_all", "k_anchor_prep", "k_synth_step"}) {
      kernels_[name] = Kernel(name);
    }
  }

  const std::string& description() const { return description_; }
  cl_context context() const { return context_; }
  cl_device_id device() const { return device_; }
  size_t globalMemory() const {
    cl_ulong bytes = 0;
    clGetDeviceInfo(device_, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof bytes, &bytes, nullptr);
    return static_cast<size_t>(bytes);
  }

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

    IteratePlain(H, W, p, nn, st, nullptr, chi, tp0);
  }

  // The device holds the state of a solve (the planes and the neighbours, in the buffers of the worker): the IRLS and the conjugate gradient, then the chroma
  // goes back to the host (`chi`), or to the buffers of a resident window (`out`: its chroma, and the luma that goes with it).
  void IteratePlain(int H, int W, const SolveParameters& p, int nn, const Staged& st, const Resident* out, ComplexPlane* chi,
                    std::chrono::steady_clock::time_point tp0) {
    const size_t N = static_cast<size_t>(H) * static_cast<size_t>(W);
    const int n = static_cast<int>(N);
    const bool diag = p.mu_d > 0.0F;
    const size_t groups = (N + kGroup - 1) / kGroup;
    const size_t global = groups * kGroup;
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
        Run(K("k_nbr_weight_all"), global, kGroup, {Arg(b_.s), ArgMem(chiBuf), Arg(b_.comp), Arg(b_.dc), Arg(b_.conf), Arg(b_.wt), Arg(b_.scale), Arg(n), Arg(et2), Arg(p.nu), Arg(nn)});
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
    if (out) {
      Check(clEnqueueCopyBuffer(queue_, chiBuf, out->chi, 0, 0, N * sizeof(Complex), 0, nullptr, nullptr), "copy of the chroma");
      Run(luma_, global, kGroup, {Arg(b_.s), ArgMem(chiBuf), Arg(b_.car), ArgMem(out->y), Arg(n)});  // (the luma of the solve: the signal less the chroma)
      return;
    }
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
    IterateJoint(H, W, p, withAnchor ? nu_a : 0.0F, nn, st, nullptr, y, chi, tp0);
  }

  // As IteratePlain, for the joint solve: Y and chi are unknowns, the anchor (when its strength is not 0) is in the buffers of the worker.
  void IterateJoint(int H, int W, const SolveParameters& p, float anchorStrength, int nn, const Staged& st, const Resident* out, Plane* y, ComplexPlane* chi,
                    std::chrono::steady_clock::time_point tp0) {
    const size_t N = static_cast<size_t>(H) * static_cast<size_t>(W);
    const int n = static_cast<int>(N);
    const bool diag = p.mu_d > 0.0F;
    const size_t groups = (N + kGroup - 1) / kGroup;
    const size_t global = groups * kGroup;
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
        Run(K("k_jnbr_weight_all"), global, kGroup, {ArgMem(yBuf), ArgMem(chiBuf), Arg(b_.comp), Arg(b_.dc), Arg(b_.conf), Arg(b_.wt), Arg(b_.scale), Arg(n), Arg(et2), Arg(p.nu), Arg(nn)});
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
    if (out) {
      Check(clEnqueueCopyBuffer(queue_, yBuf, out->y, 0, 0, N * sizeof(float), 0, nullptr, nullptr), "copy of the luma");
      Check(clEnqueueCopyBuffer(queue_, chiBuf, out->chi, 0, 0, N * sizeof(Complex), 0, nullptr, nullptr), "copy of the chroma");
      return;
    }
    Check(clEnqueueReadBuffer(queue_, yBuf, CL_TRUE, 0, N * sizeof(float), y->data(), 0, nullptr, nullptr), "read of the luma");
    g_computeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tp1).count();
    Check(clEnqueueReadBuffer(queue_, chiBuf, CL_TRUE, 0, N * sizeof(Complex), chi->data(), 0, nullptr, nullptr), "read of the chroma");
  }

  // ---- A window of fields that stays on the device ------------------------------------------------------------------------------------------------
  // The fields of a window (the composite, the carrier, the luma and the chroma of each) are sent once. The neighbours of a field are made from them on
  // the device (the warps, the gates of their confidence), and so is the anchor; the solve keeps its result there. Nothing of the neighbours crosses the
  // bus any more, and the host reads a field only when it needs it (the search for the motion, the end of the window).

  struct FieldBuffers {
    Buffer s, car, y, chi;
  };

  struct TileOffsets {
    int dy = 0, dx = 0, conf = 0;
  };

  void OpenWindow(int H, int W, int fields) {
    CloseWindow();
    const size_t N = static_cast<size_t>(H) * static_cast<size_t>(W);
    const size_t f = sizeof(float);
    const size_t c = sizeof(Complex);
    win_.H = H;
    win_.W = W;
    win_.N = N;
    for (int j = 0; j < fields; ++j) {
      win_.field.push_back(std::make_unique<FieldBuffers>());
      FieldBuffers& b = *win_.field.back();
      b.s.Allocate(context_, N * f);
      b.car.Allocate(context_, N * c);
      b.y.Allocate(context_, N * f);
      b.chi.Allocate(context_, N * c);
    }
    win_.tiles.Allocate(context_, N * sizeof(float));
    for (Buffer* plane : {&win_.vdy, &win_.vdx, &win_.tileA, &win_.tileB, &win_.tileC, &win_.gmag, &win_.lapmag, &win_.vgrad, &win_.lapsm, &win_.traw, &win_.cre,
                          &win_.cim, &win_.m1, &win_.m2, &win_.bre, &win_.bim, &win_.b1, &win_.b2, &win_.tmp, &win_.gamma, &win_.yw, &win_.confS, &win_.rr,
                          &win_.accY, &win_.accW, &win_.resid}) {
      plane->Allocate(context_, N * f);
    }
    for (Buffer* plane : {&win_.cwC, &win_.cwS, &win_.accC}) plane->Allocate(context_, N * c);
    win_.samples.Allocate(context_, (N / 7 + 2) * f);
    for (Buffer* plane : {&win_.mA, &win_.mB, &win_.mAd, &win_.mBd}) plane->Allocate(context_, N * f);
  }

  void CloseWindow() {
    win_.field.clear();
    for (Buffer* plane : {&win_.vdy, &win_.vdx, &win_.tileA, &win_.tileB, &win_.tileC, &win_.gmag, &win_.lapmag, &win_.vgrad, &win_.lapsm, &win_.traw, &win_.cre,
                          &win_.cim, &win_.m1, &win_.m2, &win_.bre, &win_.bim, &win_.b1, &win_.b2, &win_.tmp, &win_.gamma, &win_.yw, &win_.confS, &win_.rr,
                          &win_.accY, &win_.accW, &win_.resid, &win_.cwC, &win_.cwS, &win_.accC, &win_.samples, &win_.mA, &win_.mB, &win_.mAd, &win_.mBd, &win_.mCost, &win_.mShift, &win_.mStat, &win_.mStat2}) {
      plane->Release();
    }
  }

  void SetField(int j, const Plane& s, const ComplexPlane& carrier, const Plane& y, const ComplexPlane& chi) {
    FieldBuffers& b = *win_.field.at(static_cast<size_t>(j));
    const size_t N = win_.N;
    Write(b.s, s.data(), N * sizeof(float));
    Write(b.car, carrier.data(), N * sizeof(Complex));
    Write(b.y, y.data(), N * sizeof(float));
    Write(b.chi, chi.data(), N * sizeof(Complex));
  }

  void GetField(int j, Plane* y, ComplexPlane* chi) {
    FieldBuffers& b = *win_.field.at(static_cast<size_t>(j));
    const size_t N = win_.N;
    if (y) Check(clEnqueueReadBuffer(queue_, b.y.get(), CL_TRUE, 0, N * sizeof(float), y->data(), 0, nullptr, nullptr), "read of a luma");
    if (chi) Check(clEnqueueReadBuffer(queue_, b.chi.get(), CL_TRUE, 0, N * sizeof(Complex), chi->data(), 0, nullptr, nullptr), "read of a chroma");
  }

  // The neighbours of a field, its anchor if it has one, and the solve; the result is the new luma and chroma of the field, on the device.
  void SolveField(int j, const FieldJob& job, const SolveParameters& p) {
    const int H = win_.H;
    const int W = win_.W;
    const size_t N = win_.N;
    const int n = static_cast<int>(N);
    const size_t groups = (N + kGroup - 1) / kGroup;
    const size_t global = groups * kGroup;
    const int nn = p.nu > 0.0F ? static_cast<int>(job.neighbours.size()) : 0;
    Prepare(N, static_cast<size_t>(nn), groups);
    const auto tp0 = std::chrono::steady_clock::now();
    FieldBuffers& F = *win_.field.at(static_cast<size_t>(j));
    const size_t cb = N * sizeof(Complex);
    const size_t fb = N * sizeof(float);
    Check(clEnqueueCopyBuffer(queue_, F.s.get(), b_.s.get(), 0, 0, fb, 0, nullptr, nullptr), "copy");
    Check(clEnqueueCopyBuffer(queue_, F.car.get(), b_.car.get(), 0, 0, cb, 0, nullptr, nullptr), "copy");
    Check(clEnqueueCopyBuffer(queue_, F.chi.get(), b_.chi.get(), 0, 0, cb, 0, nullptr, nullptr), "copy");
    if (job.joint) Check(clEnqueueCopyBuffer(queue_, F.y.get(), b_.yc.get(), 0, 0, fb, 0, nullptr, nullptr), "copy");

    PackTiles(job);
    if (job.anyOdd) BuildOddMaps(F);
    for (int idx = 0; idx < nn; ++idx) BuildNeighbour(job, idx, F, job.joint);
    if (job.joint) BuildAnchor(job, F);

    Resident out;
    out.chi = F.chi.get();
    out.y = F.y.get();
    const Staged none{};
    if (job.joint) IterateJoint(H, W, p, job.anchorStrength, nn, none, &out, nullptr, nullptr, tp0);
    else IteratePlain(H, W, p, nn, none, &out, nullptr, tp0);
    (void)n;
    (void)global;
  }

  // For the tests: the neighbours of the field (and its anchor, when the job is joint) as the device makes them, read back
  void DebugBuild(int j, const FieldJob& job, WindowDebug* out) {
    const size_t N = win_.N;
    const size_t groups = (N + kGroup - 1) / kGroup;
    const int nn = static_cast<int>(job.neighbours.size());
    Prepare(N, static_cast<size_t>(nn), groups);
    FieldBuffers& F = *win_.field.at(static_cast<size_t>(j));
    const size_t cb = N * sizeof(Complex);
    const size_t fb = N * sizeof(float);
    Check(clEnqueueCopyBuffer(queue_, F.s.get(), b_.s.get(), 0, 0, fb, 0, nullptr, nullptr), "copy");
    Check(clEnqueueCopyBuffer(queue_, F.car.get(), b_.car.get(), 0, 0, cb, 0, nullptr, nullptr), "copy");
    Check(clEnqueueCopyBuffer(queue_, F.chi.get(), b_.chi.get(), 0, 0, cb, 0, nullptr, nullptr), "copy");
    PackTiles(job);
    if (job.anyOdd) BuildOddMaps(F);
    for (int idx = 0; idx < nn; ++idx) BuildNeighbour(job, idx, F, true);
    if (job.joint) BuildAnchor(job, F);
    out->comp.resize(static_cast<size_t>(nn) * N);
    out->conf.resize(static_cast<size_t>(nn) * N);
    out->car.resize(static_cast<size_t>(nn) * N);
    if (nn > 0) {
      Check(clEnqueueReadBuffer(queue_, b_.comp.get(), CL_TRUE, 0, out->comp.size() * sizeof(float), out->comp.data(), 0, nullptr, nullptr), "read");
      Check(clEnqueueReadBuffer(queue_, b_.conf.get(), CL_TRUE, 0, out->conf.size() * sizeof(float), out->conf.data(), 0, nullptr, nullptr), "read");
      Check(clEnqueueReadBuffer(queue_, b_.dc.get(), CL_TRUE, 0, out->car.size() * sizeof(Complex), out->car.data(), 0, nullptr, nullptr), "read");
    }
    if (job.anyOdd) {
      out->vgrad.resize(N);
      out->lapsm.resize(N);
      Check(clEnqueueReadBuffer(queue_, win_.vgrad.get(), CL_TRUE, 0, fb, out->vgrad.data(), 0, nullptr, nullptr), "read");
      Check(clEnqueueReadBuffer(queue_, win_.lapsm.get(), CL_TRUE, 0, fb, out->lapsm.data(), 0, nullptr, nullptr), "read");
    }
    if (job.joint) {
      out->ay.resize(N);
      out->aw.resize(N);
      out->ac.resize(N);
      Check(clEnqueueReadBuffer(queue_, b_.ay.get(), CL_TRUE, 0, fb, out->ay.data(), 0, nullptr, nullptr), "read");
      Check(clEnqueueReadBuffer(queue_, b_.aw.get(), CL_TRUE, 0, fb, out->aw.data(), 0, nullptr, nullptr), "read");
      Check(clEnqueueReadBuffer(queue_, b_.ac.get(), CL_TRUE, 0, cb, out->ac.data(), 0, nullptr, nullptr), "read");
    }
  }

  // The vertical envelope of the field and the multiplier of the floor of the gate of the odd neighbours (the sequence of SolveField in sequence.cpp)
  void BuildOddMaps(FieldBuffers& F) {
    const int H = win_.H;
    const int W = win_.W;
    const size_t N = win_.N;
    const int n = static_cast<int>(N);
    const size_t global = ((N + kGroup - 1) / kGroup) * kGroup;
    Run(K("k_oddmaps"), global, kGroup, {Arg(F.y), Arg(F.chi), Arg(win_.gmag), Arg(win_.lapmag), Arg(H), Arg(W)});
    Run(K("k_smooth_rows"), global, kGroup, {Arg(win_.gmag), Arg(win_.vgrad), Arg(H), Arg(W)});
    const size_t samples = (N + 6) / 7;
    Run(K("k_gather7"), ((samples + kGroup - 1) / kGroup) * kGroup, kGroup, {Arg(win_.vgrad), Arg(win_.samples), Arg(n)});
    std::vector<float> sample(samples);
    Check(clEnqueueReadBuffer(queue_, win_.samples.get(), CL_TRUE, 0, samples * sizeof(float), sample.data(), 0, nullptr, nullptr), "read of the envelope");
    const float med = MedianOfValues(std::move(sample));
    const float fl2 = 2.25F * med * med;  // (1.5 * median)^2: the floor of the noise of the envelope
    Run(K("k_floor"), global, kGroup, {Arg(win_.vgrad), Arg(fl2), Arg(n)});
    Run(K("k_smooth_rows"), global, kGroup, {Arg(win_.lapmag), Arg(win_.traw), Arg(H), Arg(W)});
    Run(K("k_lapsm"), global, kGroup, {Arg(win_.traw), Arg(win_.vgrad), Arg(win_.lapsm), Arg(n)});
  }

  // A box blur of radius r (the rows, then the columns) of `src` into `dst`
  void Box(const Buffer& src, const Buffer& dst, int r) {
    const size_t global = ((win_.N + kGroup - 1) / kGroup) * kGroup;
    Run(K("k_box_v"), global, kGroup, {Arg(src), Arg(win_.tmp), Arg(win_.H), Arg(win_.W), Arg(r)});
    Run(K("k_box_h"), global, kGroup, {Arg(win_.tmp), Arg(dst), Arg(win_.H), Arg(win_.W), Arg(r)});
  }

  // The vector at each pixel (vdy, vdx) from the tile grids of a neighbour
  // (the grids are in win_.tiles, at the offsets that PackTiles made)
  void PixelVectors(const TileOffsets& at, int th, int tw, int tile) {
    const size_t global = ((win_.N + kGroup - 1) / kGroup) * kGroup;
    Run(K("k_interp2"), global, kGroup, {Arg(win_.tiles), Arg(at.dy), Arg(at.dx), Arg(win_.vdy), Arg(win_.vdx), Arg(th), Arg(tw), Arg(tile), Arg(win_.H), Arg(win_.W)});
  }

  // The tile grids of the neighbours and of the anchor of a job, in one write (the small writes, one for each grid, made the host wait for the device at each of them)
  std::vector<TileOffsets> nbrTile_, anchorTile_;
  void PackTiles(const FieldJob& job) {
    std::vector<float> packed;
    auto put = [&packed](const Plane& plane) {
      const int off = static_cast<int>(packed.size());
      packed.insert(packed.end(), plane.data(), plane.data() + plane.size());
      return off;
    };
    nbrTile_.clear();
    anchorTile_.clear();
    for (const NeighbourSpec& spec : job.neighbours) nbrTile_.push_back({put(spec.mdy), put(spec.mdx), put(spec.conf)});
    for (const AnchorSpec& spec : job.anchor) anchorTile_.push_back({put(spec.mdy), put(spec.mdx), put(spec.conf)});
    if (packed.size() > win_.N) throw GpuError("the tile grids do not fit");
    if (!packed.empty()) Write(win_.tiles, packed.data(), packed.size() * sizeof(float));
  }

  // Neighbour number `idx` of the field: its composite, carrier and confidence, in the buffers that the solve reads
  void BuildNeighbour(const FieldJob& job, int idx, FieldBuffers& F, bool joint) {
    const int H = win_.H;
    const int W = win_.W;
    const size_t N = win_.N;
    const size_t global = ((N + kGroup - 1) / kGroup) * kGroup;
    const NeighbourSpec& spec = job.neighbours[static_cast<size_t>(idx)];
    FieldBuffers& K = *win_.field.at(static_cast<size_t>(spec.k));
    const int base = static_cast<int>(static_cast<size_t>(idx) * N);
    const TileOffsets& at = nbrTile_[static_cast<size_t>(idx)];
    // the vectors, the confidence and the warps of the composite and of the carrier, in one pass
    Run(K_("k_mc"), global, kGroup,
        {Arg(win_.tiles), Arg(at.dy), Arg(at.dx), Arg(at.conf), Arg(spec.mdy.height()), Arg(spec.mdy.width()), Arg(job.tile), Arg(H), Arg(W), Arg(K.s), Arg(K.car), Arg(b_.car),
         Arg(win_.vdy), Arg(win_.vdx), Arg(b_.conf), Arg(b_.comp), Arg(b_.dc), Arg(base), Arg(joint ? 1 : 0)});
    if (job.gateOn) {
      // the coherence of the chroma of the field with the chroma of the neighbour warped to it
      Run(K_("k_warp_bil_c"), global, kGroup, {Arg(K.chi), Arg(win_.vdy), Arg(win_.vdx), Arg(win_.cwC), Arg(H), Arg(W), Arg(0.0F)});
      Run(K_("k_coh_v"), global, kGroup, {Arg(F.chi), Arg(win_.cwC), Arg(win_.cre), Arg(win_.cim), Arg(win_.m1), Arg(win_.m2), Arg(H), Arg(W), Arg(6)});
      Run(K_("k_coh_h"), global, kGroup, {Arg(win_.cre), Arg(win_.cim), Arg(win_.m1), Arg(win_.m2), Arg(b_.conf), Arg(job.coherence), Arg(H), Arg(W), Arg(6), Arg(base)});
    }
    if (spec.odd && job.anyOdd) {
      const float e2 = job.temporalEps * job.temporalEps;
      Run(K_("k_conf_odd"), global, kGroup, {Arg(b_.conf), Arg(win_.vgrad), Arg(win_.lapsm), Arg(e2), Arg(job.oddFloorMax), Arg(static_cast<int>(N)), Arg(base)});
    }
  }

  // The anchor of the field (SynthReference): in the buffers of the solve
  void BuildAnchor(const FieldJob& job, FieldBuffers& F) {
    const int H = win_.H;
    const int W = win_.W;
    const size_t N = win_.N;
    const int n = static_cast<int>(N);
    const size_t global = ((N + kGroup - 1) / kGroup) * kGroup;
    const float eb2 = job.nrEps * job.nrEps;
    Run(K_("k_synth_init"), global, kGroup, {Arg(F.y), Arg(F.chi), Arg(win_.accY), Arg(win_.accC), Arg(win_.accW), Arg(n)});
    for (size_t ai = 0; ai < job.anchor.size(); ++ai) {
      const AnchorSpec& spec = job.anchor[ai];
      FieldBuffers& K = *win_.field.at(static_cast<size_t>(spec.k));
      const TileOffsets& at = anchorTile_[ai];
      Run(K_("k_anchor_prep"), global, kGroup, {Arg(win_.tiles), Arg(at.dy), Arg(at.dx), Arg(at.conf), Arg(spec.mdy.height()), Arg(spec.mdy.width()), Arg(job.tile), Arg(H), Arg(W),
                                                Arg(win_.vdy), Arg(win_.vdx), Arg(win_.confS)});
      Box(win_.confS, win_.rr, 8);
      Run(K_("k_synth_step"), global, kGroup, {Arg(K.y), Arg(K.chi), Arg(win_.vdy), Arg(win_.vdx), Arg(spec.rowOffset), Arg(H), Arg(W), Arg(F.y), Arg(F.chi), Arg(win_.rr),
                                               Arg(win_.accY), Arg(win_.accC), Arg(win_.accW), Arg(eb2)});
    }
    Run(K_("k_synth_fin"), global, kGroup, {Arg(win_.accY), Arg(win_.accC), Arg(win_.accW), Arg(F.car), Arg(F.s), Arg(b_.ay), Arg(b_.ac), Arg(win_.resid), Arg(n)});
    Box(win_.resid, win_.rr, 1);
    Run(K_("k_synth_weight"), global, kGroup, {Arg(win_.rr), Arg(b_.aw), Arg(eb2), Arg(n)});
  }

  static float MedianOfValues(std::vector<float> v) {
    if (v.empty()) return 0.0F;
    const size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    float m = v[k];
    if (v.size() % 2 == 0) {
      std::nth_element(v.begin(), v.begin() + static_cast<long>(k) - 1, v.begin() + static_cast<long>(k));
      m = 0.5F * (m + v[k - 1]);
    }
    return m;
  }

  cl_kernel K(const char* name) { return kernels_.at(name); }
  cl_kernel K_(const char* name) { return kernels_.at(name); }

  struct WindowBuffers {
    int H = 0, W = 0;
    size_t N = 0;
    std::vector<std::unique_ptr<FieldBuffers>> field;
    Buffer vdy, vdx, tiles, tileA, tileB, tileC, gmag, lapmag, vgrad, lapsm, traw, samples, cwC, cwS, accC, cre, cim, m1, m2, bre, bim, b1, b2, tmp, gamma, yw, confS, rr,
        accY, accW, resid, mA, mB, mAd, mBd, mCost, mShift, mStat, mStat2;
  };
  WindowBuffers win_;
  std::map<std::string, cl_kernel> kernels_;

  // ---- The search for the motion on the luma of the window ------------------------------------------------------------------------------------------
  // The squared differences of the tiles (the whole cost of the search) are made on the device, from the luma of the two fields that is already there; the
  // choice of the best shift of each tile is the one of motion.cpp on the host (the same order of candidates, the same bias, the same strict comparison), on
  // the few hundred costs that come back, so that the same costs give the same vectors.

  static std::vector<int> RangeCentre(int lo, int hi) {
    std::vector<int> v;
    if (lo > hi) return v;
    if (lo <= 0 && 0 <= hi) v.push_back(0);
    for (int d = 1; d <= std::max(-lo, hi); ++d) {
      if (-d >= lo && -d <= hi) v.push_back(-d);
      if (d >= lo && d <= hi) v.push_back(d);
    }
    if (v.empty()) {
      for (int i = lo; i <= hi; ++i) v.push_back(i);
    }
    return v;
  }

  void Grow(Buffer& buffer, size_t bytes) {
    if (buffer.bytes() < bytes) buffer.Allocate(context_, bytes + bytes / 4);
  }

  // The costs of `ncand` shifts for each of the th x tw tiles of A against B (`shifts`: sy, sx for each tile of each candidate)
  std::vector<float> BlockCosts(const Buffer& A, const Buffer& B, int H, int W, int tile, int th, int tw, const std::vector<int>& shifts, int ncand) {
    const size_t ntiles = static_cast<size_t>(th) * static_cast<size_t>(tw);
    Grow(win_.mShift, shifts.size() * sizeof(int));
    Grow(win_.mCost, static_cast<size_t>(ncand) * ntiles * sizeof(float));
    Write(win_.mShift, shifts.data(), shifts.size() * sizeof(int));
    Run(K("k_bm_cost"), static_cast<size_t>(ncand) * ntiles * kGroup, kGroup,
        {Arg(A), Arg(B), Arg(H), Arg(W), Arg(tile), Arg(th), Arg(tw), Arg(win_.mShift), Arg(win_.mCost), ArgLocal(kGroup * sizeof(float))});
    std::vector<float> costs(static_cast<size_t>(ncand) * ntiles);
    Check(clEnqueueReadBuffer(queue_, win_.mCost.get(), CL_TRUE, 0, costs.size() * sizeof(float), costs.data(), 0, nullptr, nullptr), "read of the costs");
    return costs;
  }

  // MotionConfidence of motion.cpp, from the sums of the tiles of the blurred luma
  static Plane ConfidenceOfTiles(const std::vector<float>& sum, const std::vector<float>& sum2, const Plane& best, int H, int W, int tile) {
    const int th = best.height();
    const int tw = best.width();
    Plane conf(th, tw);
    std::vector<float> npix(static_cast<size_t>(th) * tw);
    std::vector<float> tvar(npix.size());
    for (int ty = 0; ty < th; ++ty) {
      for (int tx = 0; tx < tw; ++tx) {
        const int i = ty * tw + tx;
        npix[i] = static_cast<float>((std::min((tx + 1) * tile, W) - tx * tile) * (std::min((ty + 1) * tile, H) - ty * tile));
        const float norm = 1.0F / std::max(1.0F, npix[i]);
        const float mean = sum[i] * norm;
        tvar[i] = sum2[i] * norm - mean * mean;
      }
    }
    for (int i = 0; i < th * tw; ++i) {
      const float energy = (tvar[i] + 1.0F) * std::max(1.0F, npix[i]);
      conf[i] = std::clamp(1.0F - best[i] / energy, 0.0F, 1.0F);
    }
    std::vector<float> ratios(npix.size());
    for (int i = 0; i < th * tw; ++i) ratios[i] = best[i] / std::max(1.0F, npix[i]);
    const float med = MedianOfValues(ratios);
    for (int i = 0; i < th * tw; ++i) {
      const float excess = std::max(0.0F, ratios[i] - med);
      conf[i] *= (med + 1.0F) / (med + 1.0F + excess);
      conf[i] *= 6.0F / (6.0F + med);
    }
    return conf;
  }

  // The tile sums of A, read back
  void TileSums(const Buffer& A, int tile, int th, int tw, std::vector<float>* sum, std::vector<float>* sum2) {
    const size_t ntiles = static_cast<size_t>(th) * static_cast<size_t>(tw);
    Grow(win_.mStat, ntiles * sizeof(float));
    Grow(win_.mStat2, ntiles * sizeof(float));
    Run(K("k_tile_stats"), ntiles * kGroup, kGroup,
        {Arg(A), Arg(win_.H), Arg(win_.W), Arg(tile), Arg(th), Arg(tw), Arg(win_.mStat), Arg(win_.mStat2), ArgLocal(2 * kGroup * sizeof(float))});
    sum->resize(ntiles);
    sum2->resize(ntiles);
    Check(clEnqueueReadBuffer(queue_, win_.mStat.get(), CL_TRUE, 0, ntiles * sizeof(float), sum->data(), 0, nullptr, nullptr), "read of the sums of the tiles");
    Check(clEnqueueReadBuffer(queue_, win_.mStat2.get(), CL_TRUE, 0, ntiles * sizeof(float), sum2->data(), 0, nullptr, nullptr), "read of the sums of the tiles");
  }

  // EstimateMotion(y_ref = luma of field k, y_cur = luma of field j, tile, search)
  void EstimateMotionDevice(int k, int j, int tile, int search, MotionField* out) {
    const int H = win_.H;
    const int W = win_.W;
    const size_t global = ((win_.N + kGroup - 1) / kGroup) * kGroup;
    Box(win_.field.at(static_cast<size_t>(j))->y, win_.mA, 2);  // (the current field, blurred)
    Box(win_.field.at(static_cast<size_t>(k))->y, win_.mB, 2);  // (the reference)
    const int th = (H + tile - 1) / tile;
    const int tw = (W + tile - 1) / tile;
    const size_t ntiles = static_cast<size_t>(th) * static_cast<size_t>(tw);

    // level 0: the picture decimated by 4, every shift of a small range
    constexpr int kDecimation = 4;
    const int cs = std::max(1, (search + kDecimation - 1) / kDecimation);
    const int hh = H / kDecimation;
    const int ww = W / kDecimation;
    Run(K("k_decimate"), ((static_cast<size_t>(hh) * ww + kGroup - 1) / kGroup) * kGroup, kGroup, {Arg(win_.mA), Arg(win_.mAd), Arg(H), Arg(W), Arg(kDecimation)});
    Run(K("k_decimate"), ((static_cast<size_t>(hh) * ww + kGroup - 1) / kGroup) * kGroup, kGroup, {Arg(win_.mB), Arg(win_.mBd), Arg(H), Arg(W), Arg(kDecimation)});
    const int coarse_tile = std::max(4, tile / kDecimation);
    const int cth = (hh + coarse_tile - 1) / coarse_tile;
    const int ctw = (ww + coarse_tile - 1) / coarse_tile;
    const size_t cn = static_cast<size_t>(cth) * static_cast<size_t>(ctw);
    const std::vector<int> range = RangeCentre(-cs, cs);
    std::vector<std::pair<int, int>> candidates;
    for (int dy : range) {
      for (int dx : range) candidates.emplace_back(dy, dx);
    }
    std::vector<int> shifts;
    shifts.reserve(candidates.size() * cn * 2);
    for (const auto& c : candidates) {
      for (size_t t = 0; t < cn; ++t) {
        shifts.push_back(c.first);
        shifts.push_back(c.second);
      }
    }
    std::vector<float> costs = BlockCosts(win_.mAd, win_.mBd, hh, ww, coarse_tile, cth, ctw, shifts, static_cast<int>(candidates.size()));
    std::vector<float> cbest(cn, std::numeric_limits<float>::infinity());
    std::vector<float> cdy(cn, 0.0F), cdx(cn, 0.0F);
    for (size_t ci = 0; ci < candidates.size(); ++ci) {
      const float bias = 1.0F + 0.02F * static_cast<float>(std::abs(candidates[ci].first) + std::abs(candidates[ci].second));
      for (size_t t = 0; t < cn; ++t) {
        const float cost = costs[ci * cn + t] * bias;
        if (cost < cbest[t]) {
          cbest[t] = cost;
          cdy[t] = static_cast<float>(candidates[ci].first);
          cdx[t] = static_cast<float>(candidates[ci].second);
        }
      }
    }
    // the coarse vectors on the grid of the tiles of the full picture, in pixels
    Plane base_dy(th, tw), base_dx(th, tw);
    for (int ty = 0; ty < th; ++ty) {
      const int gy = std::min((ty * cth) / th, cth - 1);
      for (int tx = 0; tx < tw; ++tx) {
        const int gx = std::min((tx * ctw) / tw, ctw - 1);
        base_dy.at(ty, tx) = cdy[static_cast<size_t>(gy) * ctw + gx] * static_cast<float>(kDecimation);
        base_dx.at(ty, tx) = cdx[static_cast<size_t>(gy) * ctw + gx] * static_cast<float>(kDecimation);
      }
    }

    // level 1: a range of +-3 pixels around it, at full resolution
    const std::vector<int> fine = RangeCentre(-3, 3);
    candidates.clear();
    for (int dy : fine) {
      for (int dx : fine) candidates.emplace_back(dy, dx);
    }
    shifts.clear();
    for (const auto& c : candidates) {
      for (size_t t = 0; t < ntiles; ++t) {
        shifts.push_back(static_cast<int>(std::lround(base_dy[t])) + c.first);
        shifts.push_back(static_cast<int>(std::lround(base_dx[t])) + c.second);
      }
    }
    costs = BlockCosts(win_.mA, win_.mB, H, W, tile, th, tw, shifts, static_cast<int>(candidates.size()));
    Plane mdy(th, tw, 0.0F), mdx(th, tw, 0.0F);
    Plane best_refined(th, tw, std::numeric_limits<float>::infinity());
    for (size_t ci = 0; ci < candidates.size(); ++ci) {
      const float bias = 1.0F + 0.02F * static_cast<float>(std::abs(candidates[ci].first) + std::abs(candidates[ci].second));
      for (size_t t = 0; t < ntiles; ++t) {
        const float cost = costs[ci * ntiles + t] * bias;
        if (cost < best_refined[t]) {
          best_refined[t] = cost;
          mdy[t] = base_dy[t] + static_cast<float>(candidates[ci].first);
          mdx[t] = base_dx[t] + static_cast<float>(candidates[ci].second);
        }
      }
    }

    // the costs one pixel either side of the vector (for the sub-pixel fit), and at no motion: five candidates
    shifts.clear();
    const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& o : offsets) {
      for (size_t t = 0; t < ntiles; ++t) {
        shifts.push_back(static_cast<int>(std::lround(mdy[t] + static_cast<float>(o[0]))));
        shifts.push_back(static_cast<int>(std::lround(mdx[t] + static_cast<float>(o[1]))));
      }
    }
    for (size_t t = 0; t < ntiles; ++t) {
      shifts.push_back(0);
      shifts.push_back(0);
    }
    costs = BlockCosts(win_.mA, win_.mB, H, W, tile, th, tw, shifts, 5);
    auto parabolic = [](float cm, float c0, float cp) {
      const float den = cm - 2.0F * c0 + cp;
      float sub = 0.0F;
      if (den > 1e-9F) sub = 0.5F * (cm - cp) / std::max(den, 1e-9F);
      return std::clamp(sub, -0.5F, 0.5F);
    };
    Plane sdy(th, tw), sdx(th, tw);
    for (size_t t = 0; t < ntiles; ++t) {
      sdy[t] = parabolic(costs[0 * ntiles + t], best_refined[t], costs[1 * ntiles + t]);
      sdx[t] = parabolic(costs[2 * ntiles + t], best_refined[t], costs[3 * ntiles + t]);
    }
    // "prefer zero": the vector is dropped where no motion is as good, or not clearly worse
    Plane best = best_refined;
    for (size_t t = 0; t < ntiles; ++t) {
      const float best0 = costs[4 * ntiles + t];
      const float cost0 = costs[4 * ntiles + t];
      if (best0 < best[t]) {
        mdy[t] = 0.0F;
        mdx[t] = 0.0F;
        best[t] = best0;
      }
      if (best[t] > 0.85F * cost0) {
        mdy[t] = 0.0F;
        mdx[t] = 0.0F;
        best[t] = cost0;
        sdy[t] = 0.0F;
        sdx[t] = 0.0F;
      }
    }
    std::vector<float> sum, sum2;
    TileSums(win_.mA, tile, th, tw, &sum, &sum2);
    out->tile = tile;
    out->dy = Plane(th, tw);
    out->dx = Plane(th, tw);
    for (size_t t = 0; t < ntiles; ++t) {
      out->dy[t] = mdy[t] + sdy[t];
      out->dx[t] = mdx[t] + sdx[t];
    }
    out->confidence = ConfidenceOfTiles(sum, sum2, best, H, W, tile);
  }

  // VerifyMotion(y_ref = luma of field k, y_cur = luma of field j, the predicted vectors, tile)
  void VerifyMotionDevice(int k, int j, const Plane& pdy, const Plane& pdx, int tile, MotionField* out) {
    const int H = win_.H;
    const int W = win_.W;
    Box(win_.field.at(static_cast<size_t>(j))->y, win_.mA, 2);
    Box(win_.field.at(static_cast<size_t>(k))->y, win_.mB, 2);
    const int th = (H + tile - 1) / tile;
    const int tw = (W + tile - 1) / tile;
    const size_t ntiles = static_cast<size_t>(th) * static_cast<size_t>(tw);
    std::vector<int> shifts;
    for (size_t t = 0; t < ntiles; ++t) {  // the prediction, rounded
      shifts.push_back(static_cast<int>(std::lround(static_cast<float>(std::lround(pdy[t])))));
      shifts.push_back(static_cast<int>(std::lround(static_cast<float>(std::lround(pdx[t])))));
    }
    for (size_t t = 0; t < ntiles; ++t) {  // no motion
      shifts.push_back(0);
      shifts.push_back(0);
    }
    const std::vector<float> costs = BlockCosts(win_.mA, win_.mB, H, W, tile, th, tw, shifts, 2);
    out->tile = tile;
    out->dy = Plane(th, tw);
    out->dx = Plane(th, tw);
    Plane best(th, tw);
    for (size_t t = 0; t < ntiles; ++t) {
      const float pred = costs[t];
      const float zero = costs[ntiles + t];
      if (zero <= pred) {
        out->dy[t] = 0.0F;
        out->dx[t] = 0.0F;
        best[t] = zero;
      } else {
        out->dy[t] = pdy[t];
        out->dx[t] = pdx[t];
        best[t] = pred;
      }
    }
    std::vector<float> sum, sum2;
    TileSums(win_.mA, tile, th, tw, &sum, &sum2);
    out->confidence = ConfidenceOfTiles(sum, sum2, best, H, W, tile);
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
    if (!g_profile) {
      Check(clEnqueueNDRangeKernel(queue_, kernel, 1, nullptr, &global, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
      return;
    }
    cl_event event = nullptr;
    Check(clEnqueueNDRangeKernel(queue_, kernel, 1, nullptr, &global, &local, 0, nullptr, &event), "clEnqueueNDRangeKernel");
    char name[64] = {0};
    clGetKernelInfo(kernel, CL_KERNEL_FUNCTION_NAME, sizeof name - 1, name, nullptr);
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    g_profile_events.emplace_back(name, event);
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
    if (b_ready_ && n_ == N && nn_ >= nn) return;
    nn = std::max<size_t>(nn, 8);  // room for the neighbours of any field: the buffers are not made again from a field to the next
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

void PrintProfile() {
  std::map<std::string, std::pair<double, long>> total;
  for (auto& entry : g_profile_events) {
    clWaitForEvents(1, &entry.second);
    cl_ulong a = 0, b = 0;
    clGetEventProfilingInfo(entry.second, CL_PROFILING_COMMAND_START, sizeof a, &a, nullptr);
    clGetEventProfilingInfo(entry.second, CL_PROFILING_COMMAND_END, sizeof b, &b, nullptr);
    total[entry.first].first += static_cast<double>(b - a) / 1e6;
    ++total[entry.first].second;
  }
  std::vector<std::pair<double, std::string>> sorted;
  double all = 0.0;
  for (auto& t : total) {
    sorted.emplace_back(t.second.first, t.first);
    all += t.second.first;
  }
  std::sort(sorted.rbegin(), sorted.rend());
  std::fprintf(stderr, "HVD OpenCL: kernels, %.1f ms in all\n", all);
  for (auto& t : sorted) std::fprintf(stderr, "  %-18s %9.1f ms  %8ld calls  %8.4f ms each\n", t.second.c_str(), t.first, total[t.second].second, t.first / static_cast<double>(total[t.second].second));
}

void PrintStats() {
  std::fprintf(stderr, "HVD OpenCL: %lld solves, %.2f s held (uploads %.2f s, iterations %.2f s), %.2f s waited for a worker\n", g_stats.calls.load(),
               g_stats.heldNs.load() / 1e9, g_uploadNs.load() / 1e9, g_computeNs.load() / 1e9, g_stats.waitNs.load() / 1e9);
  std::fprintf(stderr, "HVD OpenCL: solves of fields %.2f s, %lld searches for the motion %.2f s\n", g_deviceNs.load() / 1e9, g_motionCalls.load(), g_copyNs.load() / 1e9);
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
      g_profile = std::getenv("HVD_OPENCL_PROFILE") != nullptr;
      g_device = new Device;
      if (std::getenv("HVD_OPENCL_STATS")) std::atexit(PrintStats);
      if (g_profile) std::atexit(PrintProfile);
      g_free.push_back(g_device);
      // A window holds a worker for as long as it lasts (a few hundred megabytes of the device: its fields and the planes of its solves): as many workers as the
      // memory of the device allows, up to eight
      long count = static_cast<long>(g_device->globalMemory() / (700u * 1024u * 1024u));
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


// ---- Window ----------------------------------------------------------------------------------------------------------------------------------------

struct Window::Impl {
  Device* worker = nullptr;
  std::mutex mutex;  // the threads of a window that solve fields at the same time wait for each other: the worker has one set of buffers
};

Window::Window() = default;

std::unique_ptr<Window> Window::Open(int height, int width, int fields) {
  const auto tw0 = std::chrono::steady_clock::now();
  Device* worker = Acquire();
  g_stats.waitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw0).count();
  if (!worker) return nullptr;
  try {
    worker->OpenWindow(height, width, fields);
  } catch (const std::exception&) {
    // no room for it (or no more): the CPU decodes this window, the GPU stays on for the others
    try {
      worker->CloseWindow();
    } catch (const std::exception&) {
    }
    Release(worker);
    return nullptr;
  }
  std::unique_ptr<Window> window(new Window);
  window->impl_ = std::make_unique<Impl>();
  window->impl_->worker = worker;
  return window;
}

Window::~Window() {
  if (impl_ && impl_->worker) {
    try {
      impl_->worker->CloseWindow();
    } catch (const std::exception&) {
    }
    Release(impl_->worker);
  }
}

bool Window::SetField(int j, const Plane& s, const ComplexPlane& carrier, const Plane& y, const ComplexPlane& chi) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    impl_->worker->SetField(j, s, carrier, y, chi);
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool Window::SolveField(int j, const FieldJob& job, const SolveParameters& p) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    const auto t0 = std::chrono::steady_clock::now();
    impl_->worker->SolveField(j, job, p);
    g_deviceNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    ++g_stats.calls;
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool Window::Debug(int j, const FieldJob& job, WindowDebug* out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    impl_->worker->DebugBuild(j, job, out);
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool Window::EstimateMotion(int k, int j, int tile, int search, MotionField* out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    const auto tm0 = std::chrono::steady_clock::now();
    impl_->worker->EstimateMotionDevice(k, j, tile, search, out);
    g_copyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tm0).count();
    ++g_motionCalls;
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool Window::VerifyMotion(int k, int j, const Plane& pdy, const Plane& pdx, int tile, MotionField* out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    const auto tm0 = std::chrono::steady_clock::now();
    impl_->worker->VerifyMotionDevice(k, j, pdy, pdx, tile, out);
    g_copyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tm0).count();
    ++g_motionCalls;
    return true;
  } catch (const std::exception& e) {
    Failed(e);
    return false;
  }
}

bool Window::GetField(int j, Plane* y, ComplexPlane* chi) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    impl_->worker->GetField(j, y, chi);
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

struct Window::Impl {};
Window::Window() = default;
std::unique_ptr<Window> Window::Open(int, int, int) { return nullptr; }
Window::~Window() = default;
bool Window::SetField(int, const Plane&, const ComplexPlane&, const Plane&, const ComplexPlane&) { return false; }
bool Window::SolveField(int, const FieldJob&, const SolveParameters&) { return false; }
bool Window::GetField(int, Plane*, ComplexPlane*) { return false; }
bool Window::EstimateMotion(int, int, int, int, MotionField*) { return false; }
bool Window::VerifyMotion(int, int, const Plane&, const Plane&, int, MotionField*) { return false; }
bool Window::Debug(int, const FieldJob&, WindowDebug*) { return false; }

}  // namespace gpu
}  // namespace hvd

#endif
