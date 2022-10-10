// Split.h: DP3 step class to Split visibilities from a source model
// Copyright (C) 2022 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// @brief DP3 step class to Split visibilities from a source model
/// @author Tammo Jan Dijkema

#ifndef DP3_STEPS_SPLIT_H_
#define DP3_STEPS_SPLIT_H_

#include <utility>

#include <dp3/base/DPBuffer.h>

#include "../common/ParameterSet.h"

#include "InputStep.h"

namespace dp3 {
namespace steps {

/// @brief DP3 step class to Split visibilities from a source model
class Split : public Step {
 public:
  /// Construct the object.
  /// Parameters are obtained from the parset using the given prefix.
  Split(InputStep*, const common::ParameterSet&, const std::string& prefix);

  common::Fields getRequiredFields() const override;

  common::Fields getProvidedFields() const override { return {}; }

  /// Process the data.
  /// It keeps the data.
  /// When processed, it invokes the process function of the next step.
  bool process(const base::DPBuffer&) override;

  /// Finish the processing of this step and subsequent steps.
  void finish() override;

  void addToMS(const string&) override;

  /// Update the general info.
  void updateInfo(const base::DPInfo&) override;

  /// Show the step parameters.
  void show(std::ostream&) const override;

  /// Show the timings.
  void showTimings(std::ostream&, double duration) const override;

 private:
  std::string name_;

  /// The names of the parameters that differ along the substeps.
  std::vector<std::string> replace_parameters_;

  /// The first step in each chain of sub steps.
  std::vector<std::shared_ptr<Step>> sub_steps_;

  bool added_to_ms_;  ///< Used in addToMS to prevent recursion
};

}  // namespace steps
}  // namespace dp3

#endif
