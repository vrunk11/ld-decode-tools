// SPDX-License-Identifier: GPL-3.0-or-later
//
// variational.h — the Y/C arbitration solver (the heart of the decoder).
//
// Ports the 2-D (no-temporal-neighbour) path of `variational_refine` from
// `reference/hvd/decoder.py`.
//
// Structural fact that makes this different from a comb filter: the data term
// ||S - Y - Re[chi e^{i phi}]||^2 is *invariant* to transfers between luma and
// modulated chroma — the holographic init already fits the composite exactly.
// So Y/C separation is not a filtering problem, it is an *arbitration* problem.
// We enforce exact data fidelity by eliminating Y (Y := S - Re[chi e^{i phi}])
// and solve, over chi alone,
//
//     argmin_chi  sum rho( grad Y(chi) )
//               + mu_h sum rho_c( Dx chi ) + mu_v sum rho_c( Dy chi )
//
// with rho, rho_c the edge-preserving Charbonnier penalty, solved by IRLS
// (lagged diffusivity) + conjugate gradient. Dot crawl is carrier-frequency
// ripple in Y (huge grad-Y penalty) so it migrates into chi; twin-image /
// cross-colour is a 2fsc oscillation in chi (huge grad-chi penalty) so it
// migrates back into Y. The two classic NTSC artefacts police each other.
//
// The chroma diffusivity is coupled to the luma gradients (parallel level sets)
// so a luma edge opens a matching chroma edge — this removes hanging dots at
// vertical chroma transitions.
//
// NOTE (3-D seam): the reference's variational_refine ALSO accepts
// motion-compensated temporal neighbours — see VariationalRefine below,
// which now ports that too (an earlier version of this file had it
// missing; variational_refine_joint's independent-Y formulation is a
// DIFFERENT, anchor-specific path — see the note on VariationalRefineJoint
// for why conflating the two would have been wrong).

#ifndef LDDECODE_HVD_ENGINE_VARIATIONAL_H_
#define LDDECODE_HVD_ENGINE_VARIATIONAL_H_

#include <vector>

#include "engine/hvd_config.h"
#include "engine/plane.h"

namespace hvd {

struct RefineResult {
  Plane luma;           // Y = S - Re[chi * carrier]  (IRE)
  ComplexPlane chroma;  // chi = V - iU  (IRE)
};

// One motion-compensated neighbour's raw data term — the reference's
// `neighbors` list entries, built by temporal.h's MotionCompensatePrev (the
// function actually used by decode_sequence — MotionCompensateEnvelope
// exists in the reference but was tried and reverted, see temporal.h).
struct NeighborTerm {
  Plane composite;       // S_w: the neighbour's motion-compensated composite
  ComplexPlane carrier;  // c_w: the carrier S_w is expressed against
  Plane confidence;      // conf: per-pixel trust in this neighbour's match
};

// Refine `chi0` by arbitration, optionally against motion-compensated
// temporal neighbours ("3D mode" in the reference). `s` is the active
// composite (IRE), `carrier` is exp(i*phi), `chi0` the holographic init.
//
// Y stays EXACTLY eliminated (Y = S - Re[chi*carrier]) even with neighbors
// present — chi is the sole unknown throughout, so the lossless split
// invariant luma + Re[chroma*carrier] == s always holds on return. This
// works because each neighbour's raw residual substitutes that same
// elimination and reformulates algebraically into a term depending only on
// chi and a precomputed per-neighbour delta dc = carrier - c_w:
//     r_t = S_w - Y - Re[chi * c_w] = (S_w - S) + Re[chi * dc]
// (see reference/hvd/decoder.py's variational_refine + temporal_residual —
// this is the formulation decode_sequence's default/no-anchor path
// actually calls; VariationalRefineJoint below is a DIFFERENT, separate
// path only used together with an NR anchor).
// `neighbors` empty (the default) reduces to the plain 2-D solve.
RefineResult VariationalRefine(const Plane& s, const ComplexPlane& carrier,
                               const ComplexPlane& chi0, const HvdConfig& cfg,
                               const std::vector<NeighborTerm>& neighbors = {});

// The re-encode-loop anchor term (Y_hat, chi_hat, w_a): a synthesized,
// motion-compensated-temporal-NR reference and a composite-domain "does
// this explain the raw data" weight. Comes from synth_reference in the
// reference, which needs already-decoded neighbour output (the
// Gauss-Seidel-within-a-chunk dependency) — not wired up yet, this struct
// just gives VariationalRefineJoint's signature a stable home for it.
struct AnchorTerm {
  Plane luma;           // Y_hat
  ComplexPlane chroma;  // chi_hat
  Plane weight;         // w_a
};

// Pass-2+ solver (variational_refine_joint in the reference), used ONLY
// together with an NR anchor (decode_sequence's `_pass >= 1 and nr_anchor
// > 0` branch) — NOT the general 3-D path, which is VariationalRefine
// above. Unlike VariationalRefine, BOTH Y and chi are free unknowns here
// (relaxed data fidelity — Y is no longer forced to equal
// S - Re[chi*carrier] exactly): that relaxation is only safe with a
// trustworthy prior, which is exactly what the anchor provides.
// `neighbors` here use the (S_w, c_w, conf) triples directly (no dc
// reformulation — Y isn't eliminated, so there's nothing to substitute
// into), unlike VariationalRefine's use of the same NeighborTerm struct.
// Do NOT use this as a drop-in replacement for VariationalRefine even with
// neighbors/anchor both absent: without the exact-fidelity elimination of
// Y, the lossless luma+Re[chroma*carrier]==s guarantee does not hold here.
RefineResult VariationalRefineJoint(const Plane& s, const ComplexPlane& carrier,
                                    const Plane& y0, const ComplexPlane& chi0,
                                    const HvdConfig& cfg,
                                    const std::vector<NeighborTerm>& neighbors,
                                    const AnchorTerm* anchor);

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_VARIATIONAL_H_
