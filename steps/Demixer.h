// Demixer.h: DP3 step class to subtract A-team sources
// Copyright (C) 2023 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DP3_STEPS_DEMIXER_H_
#define DP3_STEPS_DEMIXER_H_

#include <casacore/measures/Measures/MeasFrame.h>

#include "Filter.h"
#include "MultiResultStep.h"
#include "PhaseShift.h"

#include "../base/Baseline.h"
#include "../base/FlagCounter.h"
#include "../base/Patch.h"
#include "../common/ParameterSet.h"
#include "../common/Timer.h"

namespace dp3 {
namespace steps {

/// @brief DP3 step class to subtract A-team sources
/// This class is a Step class to subtract the strong A-team sources.
/// It is based on the demixing.py script made by Bas vd Tol and operates
/// per time chunk as follows:
/// <ul>
///  <li> The data are phase-shifted and averaged for each source.
///  <li> Demixing is done using the combined results.
///  <li> For each source, a BBS solve, smooth, and predict is done.
///  <li> The predicted results are subtracted from the averaged data.
/// </ul>

class Demixer : public Step {
 public:
  /// Construct the object.
  /// Parameters are obtained from the parset using the given prefix.
  Demixer(const common::ParameterSet&, const std::string& prefix);

  /// Process the data.
  /// It keeps the data.
  /// When processed, it invokes the process function of the next step.
  bool process(const base::DPBuffer&) override;

  common::Fields getRequiredFields() const override;

  common::Fields getProvidedFields() const override;

  /// Finish the processing of this step and subsequent steps.
  void finish() override;

  /// Update the general info.
  void updateInfo(const base::DPInfo&) override;

  /// Show the step parameters.
  void show(std::ostream&) const override;

  /// Show the counts.
  void showCounts(std::ostream&) const override;

  /// Show the timings.
  void showTimings(std::ostream&, double duration) const override;

 private:
  /// Add the decorrelation factor contribution for each time slot.
  void addFactors(const base::DPBuffer& newBuf,
                  casacore::Array<casacore::DComplex>& factorBuf);

  /// Calculate the decorrelation factors by averaging them.
  /// Apply the P matrix to deproject the sources without a model.
  void makeFactors(const casacore::Array<casacore::DComplex>& bufIn,
                   casacore::Array<casacore::DComplex>& bufOut,
                   const casacore::Cube<float>& weightSums,
                   unsigned int nChanOut, unsigned int nChanAvg);

  /// Do the demixing.
  void handleDemix();

  /// Deproject the sources without a model.
  void deproject(casacore::Array<casacore::DComplex>& factors,
                 unsigned int resultIndex);

  /// Solve gains and subtract sources.
  void demix();

  /// Export the solutions to a ParmDB.
  void dumpSolutions();

  /// Merge the data of the selected baselines from the subtract buffer
  /// (itsAvgResultSubtr) into the full buffer (itsAvgResultFull).
  void mergeSubtractResult();

  std::string itsName;
  base::DPBuffer itsBufTmp;
  std::string itsSkyName;
  std::string itsInstrumentName;
  double itsDefaultGain;
  size_t itsMaxIter;
  base::BaselineSelection itsSelBL;
  Filter itsFilter;
  std::vector<std::shared_ptr<PhaseShift>> itsPhaseShifts;
  bool itsMovingPhaseRef;
  casacore::MeasFrame itsMeasFrame;
  /// Phase shift and average steps for demix.
  std::vector<std::shared_ptr<Step>> itsFirstSteps;
  /// Result of phase shifting and averaging the directions of interest
  /// at the demix resolution.
  std::vector<std::shared_ptr<MultiResultStep>> itsAvgResults;
  std::shared_ptr<Step> itsAvgStepSubtr;
  std::shared_ptr<Filter> itsFilterSubtr;
  /// Result of averaging the target at the subtract resolution.
  std::shared_ptr<MultiResultStep> itsAvgResultFull;
  std::shared_ptr<MultiResultStep> itsAvgResultSubtr;
  /// Ignore target in demixing?
  bool itsIgnoreTarget;
  /// Name of the target. Empty if no model is available for the target.
  std::string itsTargetSource;
  std::vector<std::string> itsSubtrSources;
  std::vector<std::string> itsModelSources;
  std::vector<std::string> itsExtraSources;
  std::vector<std::string> itsAllSources;
  bool itsPropagateSolutions;
  unsigned int itsNDir;
  unsigned int itsNModel;
  unsigned int itsNStation;
  unsigned int itsNBl;
  unsigned int itsNCorr;
  unsigned int itsNChanIn;
  unsigned int itsNTimeIn;
  unsigned int itsNTimeDemix;
  unsigned int itsNChanAvgSubtr;
  unsigned int itsNTimeAvgSubtr;
  unsigned int itsNChanOutSubtr;
  unsigned int itsNTimeOutSubtr;
  unsigned int itsNTimeChunk;
  unsigned int itsNTimeChunkSubtr;
  unsigned int itsNChanAvg;
  unsigned int itsNTimeAvg;
  double itsFreqResolution;
  double itsTimeResolution;
  unsigned int itsNChanOut;
  unsigned int itsNTimeOut;
  double itsTimeIntervalAvg;

  bool itsUseLBFGS;  ///< if this is not false, use LBFGS solver instead of
                     ///< LSQfit.
  unsigned int
      itsLBFGShistory;       ///< the size of LBFGS memory(history), specified
                             ///< as a multiple of the size of parameter vector.
  double itsLBFGSrobustdof;  ///< the degrees of freedom used in robust noise
                             ///< model.

  /// Accumulator used for computing the demixing weights at the demix
  /// resolution. The shape of this buffer is
  ///     #direction-pairs x #baselines x #channels x #correlations,
  /// where #direction-pairs equals: #directions x (#directions - 1)/2.
  casacore::Array<casacore::DComplex> itsFactorBuf;
  /// Buffer of demixing weights at the demix resolution. The shape of each
  /// Array is
  ///     #baselines x #channels x #correlations x #directions x #directions.
  /// Conceptually, for every pair of source directions (i.e. xt::view(
  /// itsFactors[i], xt::all(), xt::all(), xt::all(), dir1, dir2)),
  /// this Array thus provides a 3D cube of demixing weights.
  std::vector<casacore::Array<casacore::DComplex>> itsFactors;

  /// Accumulator used for computing the demixing weights at the subtract
  /// resolution. The shape of this buffer is
  ///     #direction-pairs x #baselines x #channels x #correlations,
  /// where #direction-pairs equals: #directions x (#directions - 1)/2.
  casacore::Array<casacore::DComplex> itsFactorBufSubtr;
  /// Buffer of demixing weights at the subtract resolution. The shape of each
  /// Array is
  ///     #baselines x #channels x #correlations x #directions x #directions.
  /// Conceptually, for every pair of source directions (i.e. xt::view(
  /// itsFactorsSubtr[i], xt::all(), xt::all(), xt::all(), dir1, dir2)),
  /// this Array thus provides a 3D cube of demixing weights.
  std::vector<casacore::Array<casacore::DComplex>> itsFactorsSubtr;

  std::vector<std::shared_ptr<base::Patch>> itsPatchList;
  base::Direction itsPhaseRef;
  std::vector<base::Baseline> itsBaselines;
  std::vector<int> itsUVWSplitIndex;
  std::vector<double> itsFreqDemix;
  std::vector<double> itsFreqSubtr;
  std::vector<double> itsUnknowns;
  std::vector<double> itsPrevSolution;
  unsigned int itsTimeIndex;
  unsigned int itsNConverged;
  base::FlagCounter itsFlagCounter;

  common::NSTimer itsTimer;
  common::NSTimer itsTimerPhaseShift;
  common::NSTimer itsTimerDemix;
  common::NSTimer itsTimerSolve;
  common::NSTimer itsTimerDump;
};

}  // namespace steps
}  // namespace dp3

#endif
