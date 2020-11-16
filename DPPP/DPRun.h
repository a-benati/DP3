// DPRun.h: Class to run steps like averaging and flagging on an MS
// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// @brief Class to run steps like averaging and flagging on an MS
/// @author Ger van Diepen

#ifndef DPPP_DPRUN_H
#define DPPP_DPRUN_H

#include "DPStep.h"
#include "MSReader.h"

#include <map>

namespace DP3 {
namespace DPPP {

/// @brief Class to run steps like averaging and flagging on an MS

/// This class contains a single static function that creates and executes
/// the steps defined in the parset file.
/// The parset file is documented on the LOFAR wiki.

class DPRun {
 public:
  /// Define the function to create a step from the given parameterset.
  typedef DPStep::ShPtr StepCtor(DPInput*, const class ParameterSet&,
                                 const std::string& prefix);

  /// Add a function creating a DPStep to the map.
  static void registerStepCtor(const std::string&, StepCtor*);

  /// Create a step object from the given parameters.
  /// It looks up the step type in theirStepMap. If not found, it will
  /// try to load a shared library with that name and execute the
  /// register function in it.
  static StepCtor* findStepCtor(const std::string& type);

  /// Execute the steps defined in the parset file.
  /// Possible parameters given at the command line are taken into account.
  static void execute(const std::string& parsetName, int argc = 0,
                      char* argv[] = 0);

  /// Create the step objects.
  /// If optionalWriter=false, no undefined writer will be added to the steps
  static DPStep::ShPtr makeSteps(const ParameterSet& parset,
                                 const std::string& prefix, DPInput* reader,
                                 bool optionalWriter = true);

 private:
  /// Create an output step, either an MSWriter, MSUpdater or an MSBDAWriter
  /// If no data are modified (for example if only count was done),
  /// still an MSUpdater is created, but it will not write anything.
  /// It reads the output name from the parset. If the prefix is "", it
  /// reads msout or msout.name, otherwise it reads name from the output step
  /// Create an updater step if an input MS was given; otherwise a writer.
  /// Create an updater step only if needed (e.g. not if only count is done).
  /// If the user specified an output MS name, a writer or updater is always
  /// created If there is a writer, the reader needs to read the visibility
  /// data. reader should be the original reader
  static DPStep::ShPtr makeOutputStep(DPInput* reader,
                                      const ParameterSet& parset,
                                      const string& prefix,
                                      casacore::String& currentMSName,
                                      const bool& isBDA);

  /// The map to create a step object from its type name.
  static std::map<std::string, StepCtor*> theirStepMap;
};

}  // namespace DPPP
}  // namespace DP3

#endif
