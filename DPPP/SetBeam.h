// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// @brief DPPP step class to set the beam keywords in a ms

#ifndef DPPP_SETBEAM_H
#define DPPP_SETBEAM_H

#include "DPInput.h"
#include "DPBuffer.h"
#include "Position.h"

#include <casacore/measures/Measures/MDirection.h>

namespace DP3 {

class ParameterSet;

namespace DPPP {

/// @brief DPPP step class to set the beam keywords in a ms
class SetBeam final : public DPStep {
 public:
  /// Parameters are obtained from the parset using the given prefix.
  SetBeam(DPInput* input, const ParameterSet& parameters, const string& prefix);

  bool process(const DPBuffer& buffer) override;

  void finish() override{};

  void updateInfo(const DPInfo& info) override;

  void show(std::ostream&) const override;

 private:
  DPInput* _input;
  string _name;
  std::vector<string> _directionStr;
  casacore::MDirection _direction;
  BeamCorrectionMode _mode;
};

}  // namespace DPPP
}  // namespace DP3

#endif
