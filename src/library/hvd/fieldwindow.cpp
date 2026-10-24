/******************************************************************************
 * fieldwindow.cpp
 * lddecode-hvd - the HVD decoder on a window of fields
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 vrunk11
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "fieldwindow.h"

#include <algorithm>

#include "engine/colour.h"
#include "engine/lockin.h"
#include "frame_bridge.h"

namespace hvd {

FieldWindow DecodeFieldInputs(const std::vector<FieldInput>& inputs, const FieldGeometry& g,
                              const HvdConfig& cfg, HvdEngine& engine) {
  FieldWindow window;
  window.fields.reserve(inputs.size());
  std::vector<float> burstAmplitudes;
  burstAmplitudes.reserve(inputs.size());
  for (const FieldInput& input : inputs) {
    window.fields.push_back(PrepareFieldObs(input, g, cfg, input.is_first_field ? 0 : 1));
    burstAmplitudes.push_back(BurstAmplitudeIre(input.samples, g));
  }

  float acc = 1.0F;
  if (cfg.acc && !burstAmplitudes.empty()) {
    // The median burst of the window, against the nominal one of the standard
    const size_t middle = burstAmplitudes.size() / 2;
    std::nth_element(burstAmplitudes.begin(), burstAmplitudes.begin() + middle, burstAmplitudes.end());
    acc = AccGain(burstAmplitudes[middle], g.nominal_burst_ire());
  }
  window.gain = cfg.chroma_gain * acc;

  if (cfg.selective_3d && cfg.cg_iterations > 0 && cfg.temporal_strength >= 0.0F) {
    window.decoded = DecodeFieldsSelective(window.fields, g, cfg, engine, nullptr);
  } else {
    window.decoded = engine.DecodeSequenceWindow(window.fields, g, cfg);
  }
  return window;
}

}  // namespace hvd
