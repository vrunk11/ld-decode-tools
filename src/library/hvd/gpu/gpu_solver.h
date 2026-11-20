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

#include <memory>
#include <string>
#include <vector>

#include "engine/motion.h"
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

// The parameters of a solve resolved from the configuration, as VariationalRefine and VariationalRefineJoint do (defined in engine/variational.cpp, with them)
SolveParameters ResolveSolveParameters(const ComplexPlane& chi0, const HvdConfig& cfg, bool has_neighbors);

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

// ---- A window of fields that stays on the device ----------------------------------------------------------------------------------------------
// The sequence decoder (engine/sequence.cpp) decodes a window of fields in passes. With a Window the fields are sent to the device once, the neighbours of a
// field are made there (the warps and the gates of their confidence), and so is the anchor of the later passes; each solve leaves its luma and chroma there.
// The host asks for a field only when it needs it (the search for the motion, the end of the window).

// A neighbour of the field: the tile grids of its motion (cleaned of their outliers by the host: SnapTileVectors), those of its confidence squared and
// cleaned, and whether its offset is odd (the neighbour is of the other parity)
// What the host knows of the init of a field of this geometry: the rows that its mirror padding adds above and below, and the two crops (a curve along the lines
// and a curve along the columns each, for the padded size)
struct InitAxes {
  int pad = 0;
  std::vector<float> kx[2], ky[2];
};

struct NeighbourSpec {
  int k = 0;
  Plane mdy, mdx, conf;
  bool odd = false;
};

// A neighbour of the anchor: the same grids, the confidence being the one of the motion as it is, and the offset of the line between the parities
struct AnchorSpec {
  int k = 0;
  Plane mdy, mdx, conf;
  float rowOffset = 0.0F;
};

// What a solve of one field needs besides the field itself
struct FieldJob {
  int tile = 32;
  std::vector<NeighbourSpec> neighbours;
  bool anyOdd = false;         // the vertical envelope and the floor of the gate of the odd neighbours are wanted
  // The confidence of a neighbour is the one of the slow mode: the squared confidence of the tile that the pixel is in, blurred (the confidence of the motion is in
  // the grids as it is); otherwise the fast one (the squared confidence of the tiles, cleaned of its outliers and interpolated between them)
  bool slowConfidence = false;
  bool gateOn = false;         // the coherence gate of the confidence
  float coherence = 0.0F;      // its strength
  float temporalEps = 0.0F;    // the robust scale of the neighbours
  float oddFloorMax = 0.0F;    // the ceiling of the floor of the gate of the odd neighbours
  bool joint = false;          // the passes with the anchor
  float anchorStrength = 0.0F;
  float nrEps = 0.0F;
  std::vector<AnchorSpec> anchor;
};

// What the device made of the neighbours and the anchor of a field, read back (for the tests and for the check of the decoder against the CPU)
struct WindowDebug {
  std::vector<float> comp, conf, vgrad, lapsm, ay, aw;
  std::vector<Complex> car, ac;
};

class Window {
 public:
  // The fields of a window, as many as `fields` with `height` x `width` planes; null when there is no device or no room on it (the CPU decodes then).
  // It holds a worker of the device until it is destroyed: the threads that open one when they are all taken wait for it.
  static std::unique_ptr<Window> Open(int height, int width, int fields);
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // The field j: the composite, its carrier, and the luma and chroma it starts from
  bool SetField(int j, const Plane& s, const ComplexPlane& carrier, const Plane& y, const ComplexPlane& chi);
  // The neighbours of the field j, the anchor if the job is joint, and the solve; the new luma and chroma stay on the device
  bool SolveField(int j, const FieldJob& job, const SolveParameters& p);
  // The luma and the chroma of the field j, from the device
  bool GetField(int j, Plane* y, ComplexPlane* chi);
  // The field j made from its signal and its carrier: the holographic init (its chroma and its luma) is made on the device; false when it cannot be (the host makes it)
  bool InitField(int j, const Plane& s, const ComplexPlane& carrier, const InitAxes& axes);
  // The search for the motion between the luma of two fields that are on the device (EstimateMotion and VerifyMotion of engine/motion.h, the reference being the field k and the
  // current one the field j)
  bool EstimateMotion(int k, int j, int tile, int search, MotionField* out);
  bool VerifyMotion(int k, int j, const Plane& pdy, const Plane& pdx, int tile, MotionField* out);
  bool Debug(int j, const FieldJob& job, WindowDebug* out);

 private:
  Window();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
}  // namespace hvd

#endif  // LDDECODE_HVD_GPU_GPU_SOLVER_H_
