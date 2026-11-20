// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu_solver.h — the solver of the HVD decoder on a GPU (OpenCL).
//
// VariationalRefine (variational.h) is a few hundred passes over the planes of a frame, each pass a difference, a product or a sum of a plane:
// work for a GPU. This file is the same maths in OpenCL kernels. The planes go to the device once for each solve, the iterations of the
// IRLS and of the conjugate gradient stay there (the host reads two sums for each iteration, to choose the step), and the chroma comes back.
//
// Without OpenCL in the build, or without a device, or when anything fails at the first use, the functions say so and the solver runs on the CPU as
// before: the GPU is an option, never a requirement.

#ifndef LDDECODE_HVD_GPU_GPU_SOLVER_H_
#define LDDECODE_HVD_GPU_GPU_SOLVER_H_

#include <string>
#include <vector>

#include "engine/plane.h"
#include "engine/variational.h"

namespace hvd {
namespace gpu {

// What the solver of one frame needs, already resolved by VariationalRefine (the anisotropy, the renormalisation of the priors and the
// budget of iterations come out of the configuration there, once, for the CPU and the GPU).
struct SolveParameters {
  float eps = 0.0F;     // Charbonnier of the luma
  float eps_c = 0.0F;   // Charbonnier of the chroma
  float eps_t = 0.0F;   // robust scale of the temporal residual
  float mu_h = 0.0F;    // chroma prior, horizontal
  float mu_v = 0.0F;    // chroma prior, vertical
  float mu_d = 0.0F;    // chroma prior, oriented (0 = none)
  float k = 0.0F;       // coupling of the chroma diffusivity to the luma gradients
  float nu = 0.0F;      // weight of the temporal neighbours (0 = none)
  int irls_outer = 1;
  int n_inner = 1;      // CG iterations for each IRLS round
  float cg_tol = 0.0F;
};

// Whether the build has OpenCL and a device was found (the first call looks; it can take a moment)
bool Available();

// A line about the device that is used ("NVIDIA GeForce RTX 5080 Laptop GPU (OpenCL 3.0)"), empty if none
std::string DeviceDescription();

// The same as the iterations of VariationalRefine, on the device. Returns false, and leaves `chi` as it was given, if it could not (the caller
// goes on with the CPU); the first failure turns the GPU off for the rest of the run, and says why once on the standard error.
bool Solve(const Plane& s, const ComplexPlane& carrier, const SolveParameters& p, const std::vector<NeighborTerm>& neighbors, ComplexPlane* chi);

// The joint solve of the passes with the anchor (VariationalRefineJoint): the luma `y` is an unknown as well as `chi`, and nu_a the weight of the anchor (0 = none)
bool SolveJoint(const Plane& s, const ComplexPlane& carrier, const SolveParameters& p, float nu_a, const std::vector<NeighborTerm>& neighbors, const AnchorTerm* anchor,
                Plane* y, ComplexPlane* chi);

}  // namespace gpu
}  // namespace hvd

#endif  // LDDECODE_HVD_GPU_GPU_SOLVER_H_
