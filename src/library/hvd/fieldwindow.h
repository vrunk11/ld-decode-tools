/******************************************************************************
 * fieldwindow.h
 * lddecode-hvd - the HVD decoder on a window of fields
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 vrunk11
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDDECODE_HVD_FIELDWINDOW_H_
#define LDDECODE_HVD_FIELDWINDOW_H_

#include <vector>

#include "engine/engine.h"
#include "engine/hvd_config.h"
#include "engine/ntsc_geometry.h"
#include "engine/sequence.h"

namespace hvd {

// The fields of a window, decoded: `decoded[i]` goes with `fields[i]` (the field's carrier and its true parity)
struct FieldWindow {
  std::vector<FieldObs> fields;
  std::vector<DecodedField> decoded;
  float gain = 1.0F;  // chroma_gain times the automatic colour control gain of the window
};

// Decode consecutive fields (IRE referenced to blanking, `is_first_field` set from the source) through the sequence pipeline
// of engine/sequence.h. The fields stand in time order; the caller keeps the fields of the frames it asked for and takes the
// others as context. Not safe to call at the same time on one engine: each thread has its own.
FieldWindow DecodeFieldInputs(const std::vector<FieldInput>& inputs, const FieldGeometry& g,
                              const HvdConfig& cfg, HvdEngine& engine);

}  // namespace hvd

#endif  // LDDECODE_HVD_FIELDWINDOW_H_
