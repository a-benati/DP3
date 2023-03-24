// OnePredict.cc: DPPP step class that predicts visibilities.
// Copyright (C) 2021 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// @author Tammo Jan Dijkema

#include "OnePredict.h"
#include "ApplyBeam.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include <xtensor/xview.hpp>

#include "../common/ParameterSet.h"
#include "../common/Timer.h"
#include "../common/StreamUtil.h"

#include "../parmdb/ParmDBMeta.h"
#include "../parmdb/PatchInfo.h"
#include "../parmdb/SkymodelToSourceDB.h"

#include <dp3/base/DPInfo.h>
#include "../base/FlagCounter.h"
#include "../base/Simulate.h"
#include "../base/Simulator.h"
#include "../base/Stokes.h"
#include "../base/PointSource.h"
#include "../base/GaussianSource.h"
#include "../base/Telescope.h"

#include "../parmdb/SourceDB.h"

#include <aocommon/threadpool.h>
#include <aocommon/parallelfor.h>
#include <aocommon/barrier.h>

#include <casacore/casa/Arrays/Array.h>
#include <casacore/casa/Arrays/Vector.h>
#include <casacore/casa/OS/File.h>
#include <casacore/measures/Measures/MDirection.h>
#include <casacore/measures/Measures/MeasConvert.h>
#include <casacore/measures/Measures/MEpoch.h>
#include <casacore/tables/Tables/RefRows.h>

#include <stddef.h>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include <numeric>
#include <algorithm>

#include <boost/algorithm/string/case_conv.hpp>

using casacore::Cube;
using casacore::MDirection;
using casacore::MEpoch;
using casacore::MVDirection;
using casacore::MVEpoch;
using casacore::Quantum;

using dp3::base::DPBuffer;
using dp3::base::DPInfo;
using dp3::common::operator<<;

namespace dp3 {
namespace steps {

OnePredict::OnePredict(const common::ParameterSet& parset, const string& prefix,
                       const std::vector<string>& source_patterns)
    : thread_pool_(nullptr), measures_mutex_(nullptr) {
  if (!source_patterns.empty()) {
    init(parset, prefix, source_patterns);
  } else {
    const std::vector<std::string> parset_patterns =
        parset.getStringVector(prefix + "sources", std::vector<std::string>());
    init(parset, prefix, parset_patterns);
  }
}

void OnePredict::init(const common::ParameterSet& parset, const string& prefix,
                      const std::vector<string>& sourcePatterns) {
  name_ = prefix;
  source_db_name_ = parset.getString(prefix + "sourcedb");
  correct_freq_smearing_ =
      parset.getBool(prefix + "correctfreqsmearing", false);
  SetOperation(parset.getString(prefix + "operation", "replace"));
  apply_beam_ = parset.getBool(prefix + "usebeammodel", false);
  thread_over_baselines_ = parset.getBool(prefix + "parallelbaselines", false);
  debug_level_ = parset.getInt(prefix + "debuglevel", 0);
  patch_list_.clear();

  // Save directions specifications to pass to applycal
  std::stringstream ss;
  ss << sourcePatterns;
  direction_str_ = ss.str();

  base::SourceDB source_db{source_db_name_, sourcePatterns,
                           base::SourceDB::FilterMode::kPattern};
  try {
    patch_list_ = source_db.MakePatchList();
    if (patch_list_.empty()) {
      throw std::runtime_error("Couldn't find patch for direction " +
                               direction_str_);
    }
  } catch (std::exception& exception) {
    throw std::runtime_error(std::string("Something went wrong while reading "
                                         "the source model. The error was: ") +
                             exception.what());
  }

  if (apply_beam_) {
    use_channel_freq_ = parset.getBool(prefix + "usechannelfreq", true);
    one_beam_per_patch_ = parset.getBool(prefix + "onebeamperpatch", false);
    beam_proximity_limit_ =
        parset.getDouble(prefix + "beamproximitylimit", 60.0) *
        (M_PI / (180.0 * 60.0 * 60.0));

    beam_mode_ = everybeam::ParseCorrectionMode(
        parset.getString(prefix + "beammode", "default"));

    string element_model = boost::to_lower_copy(
        parset.getString(prefix + "elementmodel", "hamaker"));
    if (element_model == "hamaker") {
      element_response_model_ = everybeam::ElementResponseModel::kHamaker;
    } else if (element_model == "lobes") {
      element_response_model_ = everybeam::ElementResponseModel::kLOBES;
    } else if (element_model == "oskar") {
      element_response_model_ =
          everybeam::ElementResponseModel::kOSKARSphericalWave;
    } else if (element_model == "oskardipole") {
      element_response_model_ = everybeam::ElementResponseModel::kOSKARDipole;
    } else {
      throw std::runtime_error(
          "Elementmodel should be HAMAKER, LOBES, OSKAR or OSKARDIPOLE");
    }

    // By default, a source model has each direction in one patch. Therefore,
    // if one-beam-per-patch is requested, we don't have to do anything.
    if (!one_beam_per_patch_) {
      if (beam_proximity_limit_ > 0.0) {
        // Rework patch list to cluster proximate sources
        patch_list_ =
            clusterProximateSources(patch_list_, beam_proximity_limit_);
      } else {
        // Rework patch list to contain a patch for every source
        patch_list_ = makeOnePatchPerComponent(patch_list_);
      }
    }
  }

  // If called from h5parmpredict, applycal gets set by that step,
  // so must not be read from parset
  if (parset.isDefined(prefix + "applycal.parmdb") ||
      parset.isDefined(prefix + "applycal.steps")) {
    SetApplyCal(parset, prefix + "applycal.");
  }

  source_list_ = makeSourceList(patch_list_);

  // Determine whether any sources are polarized. If not, enable Stokes-I-
  // only mode (note that this mode cannot be used with apply_beam_)
  if (apply_beam_ && beam_mode_ != everybeam::CorrectionMode::kArrayFactor) {
    stokes_i_only_ = false;
  } else {
    stokes_i_only_ = !source_db.CheckPolarized();
  }
  any_orientation_is_absolute_ = source_db.CheckAnyOrientationIsAbsolute();
}

void OnePredict::SetApplyCal(const common::ParameterSet& parset,
                             const string& prefix) {
  apply_cal_step_ =
      std::make_shared<ApplyCal>(parset, prefix, true, direction_str_);
  if (operation_ != Operation::kReplace &&
      parset.getBool(prefix + "applycal.updateweights", false))
    throw std::invalid_argument(
        "Weights cannot be updated when operation is not replace");
  result_step_ = std::make_shared<ResultStep>();
  apply_cal_step_->setNextStep(result_step_);
}

OnePredict::~OnePredict() {}

void OnePredict::initializeThreadData() {
  const size_t nBl = info().nbaselines();
  const size_t nSt = info().nantenna();
  const size_t nCh = info().nchan();
  const size_t nCr = stokes_i_only_ ? 1 : info().ncorr();
  const size_t nThreads = getInfo().nThreads();

  station_uvw_.resize({nSt, 3});

  std::vector<std::array<double, 3>> antenna_pos(info().antennaPos().size());
  for (unsigned int i = 0; i < info().antennaPos().size(); ++i) {
    casacore::Quantum<casacore::Vector<double>> pos =
        info().antennaPos()[i].get("m");
    antenna_pos[i][0] = pos.getValue()[0];
    antenna_pos[i][1] = pos.getValue()[1];
    antenna_pos[i][2] = pos.getValue()[2];
  }

  uvw_split_index_ = base::nsetupSplitUVW(info().nantenna(), info().getAnt1(),
                                          info().getAnt2(), antenna_pos);

  if (!predict_buffer_) {
    predict_buffer_ = std::make_shared<base::PredictBuffer>();
  }
  if (apply_beam_ && predict_buffer_->GetStationList().empty()) {
    telescope_ = base::GetTelescope(info().msName(), element_response_model_,
                                    use_channel_freq_);
  }
  predict_buffer_->resize(nThreads, nCr, nCh, nBl, nSt, apply_beam_);
  // Create the Measure ITRF conversion info given the array position.
  // The time and direction are filled in later.
  meas_convertors_.resize(nThreads);
  meas_frame_.resize(nThreads);

  for (size_t thread = 0; thread < nThreads; ++thread) {
    const bool need_meas_converters = moving_phase_ref_ || apply_beam_;
    if (need_meas_converters) {
      // Prepare measures converters
      meas_frame_[thread].set(info().arrayPosCopy());
      meas_frame_[thread].set(
          MEpoch(MVEpoch(info().startTime() / 86400), MEpoch::UTC));
      meas_convertors_[thread].set(
          MDirection::J2000,
          MDirection::Ref(MDirection::ITRF, meas_frame_[thread]));
    }
  }
}

void OnePredict::updateInfo(const DPInfo& infoIn) {
  Step::updateInfo(infoIn);
  if (operation_ == Operation::kReplace)
    info().setBeamCorrectionMode(
        static_cast<int>(everybeam::CorrectionMode::kNone));

  const size_t nBl = info().nbaselines();
  for (size_t i = 0; i != nBl; ++i) {
    baselines_.emplace_back(info().getAnt1()[i], info().getAnt2()[i]);
  }

  try {
    MDirection dirJ2000(
        MDirection::Convert(infoIn.phaseCenter(), MDirection::J2000)());
    Quantum<casacore::Vector<double>> angles = dirJ2000.getAngle();
    moving_phase_ref_ = false;
    phase_ref_ =
        base::Direction(angles.getBaseValue()[0], angles.getBaseValue()[1]);
  } catch (casacore::AipsError&) {
    // Phase direction (in J2000) is time dependent
    moving_phase_ref_ = true;
  }

  initializeThreadData();

  if (apply_cal_step_) {
    info() = apply_cal_step_->setInfo(info());
  }
}

base::Direction OnePredict::GetFirstDirection() const {
  return patch_list_.front()->direction();
}

void OnePredict::SetOperation(const std::string& operation) {
  if (operation == "replace") {
    operation_ = Operation::kReplace;
  } else if (operation == "add") {
    operation_ = Operation::kAdd;
  } else if (operation == "subtract") {
    operation_ = Operation::kSubtract;
  } else {
    throw std::invalid_argument(
        "Operation must be 'replace', 'add' or 'subtract'.");
  }
}

void OnePredict::show(std::ostream& os) const {
  os << "OnePredict " << name_ << '\n';
  os << "  sourcedb:                " << source_db_name_ << '\n';
  os << "   number of patches:      " << patch_list_.size() << '\n';
  os << "   patches clustered:      " << std::boolalpha
     << (!one_beam_per_patch_ && (beam_proximity_limit_ > 0.0)) << '\n';
  os << "   number of components:   " << source_list_.size() << '\n';
  os << "   absolute orientation:   " << std::boolalpha
     << any_orientation_is_absolute_ << '\n';
  os << "   all unpolarized:        " << std::boolalpha << stokes_i_only_
     << '\n';
  os << "   correct freq smearing:  " << std::boolalpha
     << correct_freq_smearing_ << '\n';
  os << "  apply beam:              " << std::boolalpha << apply_beam_ << '\n';
  if (apply_beam_) {
    os << "   mode:                   " << everybeam::ToString(beam_mode_);
    os << '\n';
    os << "   use channelfreq:        " << std::boolalpha << use_channel_freq_
       << '\n';
    os << "   one beam per patch:     " << std::boolalpha << one_beam_per_patch_
       << '\n';
    os << "   beam proximity limit:   "
       << (beam_proximity_limit_ * (180.0 * 60.0 * 60.0) / M_PI) << " arcsec\n";
  }
  os << "  operation:               ";
  switch (operation_) {
    case Operation::kReplace:
      os << "replace\n";
      break;
    case Operation::kAdd:
      os << "add\n";
      break;
    case Operation::kSubtract:
      os << "subtract\n";
      break;
  }
  os << "  threads:                 " << getInfo().nThreads() << '\n';
  if (apply_cal_step_) {
    apply_cal_step_->show(os);
  }
}

void OnePredict::showTimings(std::ostream& os, double duration) const {
  os << "  ";
  base::FlagCounter::showPerc1(os, timer_.getElapsed(), duration);
  os << " OnePredict " << name_ << '\n';

  /*
   * The timer_ measures the time in a single thread. Both predict_time_ and
   * apply_beam_time_ are the sum of time in multiple threads. This makes it
   * hard to determine the exact time spent in these phases. Instead it shows
   * the percentage spent in these two parts.
   */
  const int64_t time{predict_time_ + apply_beam_time_};
  os << "          ";
  base::FlagCounter::showPerc1(os, predict_time_, time);
  os << " of it spent in predict" << '\n';

  os << "          ";
  base::FlagCounter::showPerc1(os, apply_beam_time_, time);
  os << " of it spent in apply beam" << '\n';
}

bool OnePredict::process(std::unique_ptr<DPBuffer> buffer) {
  timer_.start();

  // Determine the various sizes.
  const size_t nSt = info().nantenna();
  const size_t nBl = info().nbaselines();
  const size_t nCh = info().nchan();
  const size_t nCr = info().ncorr();

  base::nsplitUVW(uvw_split_index_, baselines_, buffer->GetUvw(), station_uvw_);

  double time = buffer->getTime();
  // Set up directions for beam evaluation
  everybeam::vector3r_t refdir, tiledir;

  const bool need_meas_converters = moving_phase_ref_ || apply_beam_;
  if (need_meas_converters) {
    // Because multiple predict steps might be predicting simultaneously, and
    // Casacore is not thread safe, this needs synchronization.
    std::unique_lock<std::mutex> lock;
    if (measures_mutex_ != nullptr)
      lock = std::unique_lock<std::mutex>(*measures_mutex_);
    for (size_t thread = 0; thread != getInfo().nThreads(); ++thread) {
      meas_frame_[thread].resetEpoch(
          MEpoch(MVEpoch(time / 86400), MEpoch::UTC));
      // Do a conversion on all threads
      refdir = dir2Itrf(info().delayCenter(), meas_convertors_[thread]);
      tiledir = dir2Itrf(info().tileBeamDir(), meas_convertors_[thread]);
    }
  }

  if (moving_phase_ref_) {
    // Convert phase reference to J2000
    MDirection dirJ2000(MDirection::Convert(
        info().phaseCenter(),
        MDirection::Ref(MDirection::J2000, meas_frame_[0]))());
    Quantum<casacore::Vector<double>> angles = dirJ2000.getAngle();
    phase_ref_ =
        base::Direction(angles.getBaseValue()[0], angles.getBaseValue()[1]);
  }

  std::unique_ptr<aocommon::ThreadPool> localThreadPool;
  aocommon::ThreadPool* pool = thread_pool_;
  if (pool == nullptr) {
    // If no ThreadPool was specified, we create a temporary one just
    // for executation of this part.
    localThreadPool = std::make_unique<aocommon::ThreadPool>(info().nThreads());
    pool = localThreadPool.get();
  } else {
    if (pool->NThreads() != info().nThreads())
      throw std::runtime_error(
          "Thread pool has inconsistent number of threads!");
  }
  std::vector<base::Simulator> simulators;
  simulators.reserve(pool->NThreads());

  std::vector<std::pair<size_t, size_t>> baseline_range;
  std::vector<xt::xtensor<std::complex<double>, 3>> sim_buffer;
  std::vector<std::vector<std::pair<size_t, size_t>>> baselines_split;
  std::vector<std::pair<size_t, size_t>> station_range;
  const size_t actual_nCr = (stokes_i_only_ ? 1 : nCr);
  size_t n_threads = pool->NThreads();
  if (thread_over_baselines_) {
    // Reduce the number of threads if there are not enough baselines.
    n_threads = std::min(n_threads, nBl);

    // All threads process 'baselines_per_thread' baselines.
    // The first 'remaining_baselines' threads process an extra baseline.
    const size_t baselines_per_thread = nBl / n_threads;
    const size_t remaining_baselines = nBl % n_threads;

    baseline_range.resize(n_threads);
    sim_buffer.resize(n_threads);
    baselines_split.resize(n_threads);
    if (apply_beam_) {
      station_range.resize(n_threads);
    }

    // Index of the first baseline for the current thread. The loop below
    // updates this variable in each iteration.
    size_t first_baseline = 0;
    for (size_t thread_index = 0; thread_index != n_threads; ++thread_index) {
      const size_t chunk_size =
          baselines_per_thread + ((thread_index < remaining_baselines) ? 1 : 0);

      baseline_range[thread_index] =
          std::make_pair(first_baseline, first_baseline + chunk_size);
      sim_buffer[thread_index].resize({chunk_size, nCh, actual_nCr});

      baselines_split[thread_index].resize(chunk_size);
      std::copy_n(std::next(baselines_.cbegin(), first_baseline), chunk_size,
                  baselines_split[thread_index].begin());

      first_baseline += chunk_size;  // Update for the next loop iteration.
    }
    // Verify that all baselines are assigned to threads.
    assert(first_baseline == nBl);

    // find min,max station indices for this thread
    if (apply_beam_) {
      const size_t stations_thread = (nSt + n_threads - 1) / n_threads;
      for (size_t thread_index = 0; thread_index != n_threads; ++thread_index) {
        const size_t station_start = thread_index * stations_thread;
        const size_t station_end = station_start + stations_thread < nSt
                                       ? station_start + stations_thread
                                       : nSt;
        if (station_start < nSt) {
          station_range[thread_index] =
              std::make_pair(station_start, station_end);
        } else {
          // fill an invalid station range
          // so that station_start<nSt for valid range
          station_range[thread_index] = std::make_pair(nSt + 1, nSt + 1);
        }
      }
    }

    aocommon::ParallelFor<size_t> loop(n_threads);
    loop.Run(0, n_threads, [&](size_t thread_index) {
      const std::complex<double> zero(0.0, 0.0);
      predict_buffer_->GetModel(thread_index).fill(zero);
      if (apply_beam_) predict_buffer_->GetPatchModel(thread_index).fill(zero);
      sim_buffer[thread_index].fill(zero);
    });

    // Keep this loop single threaded, I'm not sure if Simulator constructor
    // is thread safe.
    for (size_t thread_index = 0; thread_index != n_threads; ++thread_index) {
      // When applying beam, simulate into patch vector
      // Create a Casacore view since the Simulator still uses Casacore.
      xt::xtensor<std::complex<double>, 3>& thread_buffer =
          sim_buffer[thread_index];
      const casacore::IPosition shape(3, thread_buffer.shape(2),
                                      thread_buffer.shape(1),
                                      thread_buffer.shape(0));
      Cube<dcomplex> simulatedest(shape, thread_buffer.data(), casacore::SHARE);

      simulators.emplace_back(phase_ref_, nSt, baselines_split[thread_index],
                              casacore::Vector<double>(info().chanFreqs()),
                              casacore::Vector<double>(info().chanWidths()),
                              station_uvw_, simulatedest,
                              correct_freq_smearing_, stokes_i_only_);
    }
  } else {
    for (size_t thread_index = 0; thread_index != pool->NThreads();
         ++thread_index) {
      xt::xtensor<std::complex<double>, 3>& model =
          predict_buffer_->GetModel(thread_index);
      model.fill(std::complex<double>(0.0, 0.0));
      std::complex<double>* model_data = model.data();

      if (apply_beam_) {
        xt::xtensor<std::complex<double>, 3>& patch_model =
            predict_buffer_->GetPatchModel(thread_index);
        patch_model.fill(std::complex<double>(0.0, 0.0));
        // When applying beam, simulate into patch vector
        model_data = patch_model.data();
      }

      // Create a Casacore view since the Simulator still uses Casacore.
      // Always use model.shape(), since it's equal to the patch model shape.
      const casacore::IPosition shape(3, model.shape(2), model.shape(1),
                                      model.shape(0));
      Cube<dcomplex> simulatedest(shape, model_data, casacore::SHARE);

      simulators.emplace_back(phase_ref_, nSt, baselines_,
                              casacore::Vector<double>(info().chanFreqs()),
                              casacore::Vector<double>(info().chanWidths()),
                              station_uvw_, simulatedest,
                              correct_freq_smearing_, stokes_i_only_);
    }
  }
  std::vector<std::shared_ptr<const base::Patch>> curPatches(pool->NThreads());

  if (thread_over_baselines_) {
    aocommon::Barrier barrier(n_threads);
    aocommon::ParallelFor<size_t> loop(n_threads);
    loop.Run(0, n_threads, [&](size_t thread_index) {
      const common::ScopedMicroSecondAccumulator<decltype(predict_time_)>
          scoped_time{predict_time_};
      // OnePredict the source model and apply beam when an entire patch is
      // done
      std::shared_ptr<const base::Patch>& curPatch = curPatches[thread_index];

      for (size_t source_index = 0; source_index < source_list_.size();
           ++source_index) {
        const bool patchIsFinished =
            curPatch != source_list_[source_index].second &&
            curPatch != nullptr;

        if (apply_beam_ && patchIsFinished) {
          // PatchModel <- SimulBuffer
          xt::xtensor<std::complex<double>, 3>& patch_model =
              predict_buffer_->GetPatchModel(thread_index);
          xt::view(patch_model,
                   xt::range(baseline_range[thread_index].first,
                             baseline_range[thread_index].second),
                   xt::all(), xt::all()) = sim_buffer[thread_index];

          // Apply the beam and add PatchModel to Model
          addBeamToData(curPatch, time, thread_index, patch_model,
                        baseline_range[thread_index],
                        station_range[thread_index], barrier, stokes_i_only_);
          // Initialize patchmodel to zero for the next patch
          sim_buffer[thread_index].fill(std::complex<double>(0.0, 0.0));
        }
        // Depending on apply_beam_, the following call will add to either
        // the Model or the PatchModel of the predict buffer
        simulators[thread_index].simulate(source_list_[source_index].first);

        curPatch = source_list_[source_index].second;
      }
      // catch last source
      if (apply_beam_ && curPatch != nullptr) {
        // PatchModel <- SimulBuffer
        xt::xtensor<std::complex<double>, 3>& patch_model =
            predict_buffer_->GetPatchModel(thread_index);
        xt::view(patch_model,
                 xt::range(baseline_range[thread_index].first,
                           baseline_range[thread_index].second),
                 xt::all(), xt::all()) = sim_buffer[thread_index];

        addBeamToData(curPatch, time, thread_index, patch_model,
                      baseline_range[thread_index], station_range[thread_index],
                      barrier, stokes_i_only_);
      }
      if (!apply_beam_) {
        xt::xtensor<std::complex<double>, 3>& model =
            predict_buffer_->GetModel(thread_index);
        xt::view(model,
                 xt::range(baseline_range[thread_index].first,
                           baseline_range[thread_index].second),
                 xt::all(), xt::all()) = sim_buffer[thread_index];
      }
    });
  } else {
    pool->For(0, source_list_.size(), [&](size_t source_index, size_t thread) {
      const common::ScopedMicroSecondAccumulator<decltype(predict_time_)>
          scoped_time{predict_time_};
      // OnePredict the source model and apply beam when an entire patch is
      // done
      std::shared_ptr<const base::Patch>& curPatch = curPatches[thread];
      const bool patchIsFinished =
          curPatch != source_list_[source_index].second && curPatch != nullptr;
      if (apply_beam_ && patchIsFinished) {
        // Apply the beam and add PatchModel to Model
        addBeamToData(curPatch, time, thread,
                      predict_buffer_->GetPatchModel(thread), stokes_i_only_);
        // Initialize patchmodel to zero for the next patch
        predict_buffer_->GetPatchModel(thread).fill(
            std::complex<double>(0.0, 0.0));
      }
      // Depending on apply_beam_, the following call will add to either
      // the Model or the PatchModel of the predict buffer
      simulators[thread].simulate(source_list_[source_index].first);

      curPatch = source_list_[source_index].second;
    });
    // Apply beam to the last patch
    if (apply_beam_) {
      pool->For(0, pool->NThreads(), [&](size_t thread, size_t) {
        const common::ScopedMicroSecondAccumulator<decltype(predict_time_)>
            scoped_time{predict_time_};
        if (curPatches[thread] != nullptr) {
          addBeamToData(curPatches[thread], time, thread,
                        predict_buffer_->GetPatchModel(thread), stokes_i_only_);
        }
      });
    }
  }

  // Copy the input visibilities if we need them later.
  if (operation_ == Operation::kAdd || operation_ == Operation::kSubtract) {
    input_data_ = buffer->GetData();
  }

  // Add all thread model data to one buffer
  buffer->ResizeData(nBl, nCh, nCr);
  buffer->MakeIndependent(kDataField);
  buffer->GetData().fill(std::complex<float>());
  for (size_t thread = 0; thread < std::min(pool->NThreads(), n_threads);
       ++thread) {
    if (stokes_i_only_) {
      // Add the predicted model to the first and last correlation.
      auto data_view = xt::view(buffer->GetData(), xt::all(), xt::all(),
                                xt::keep(0, nCr - 1));
      // Without explicit casts, XTensor does not know what to do.
      data_view = xt::cast<std::complex<float>>(
          xt::cast<std::complex<double>>(data_view) +
          predict_buffer_->GetModel(thread));
    } else {
      // Without explicit casts, XTensor does not know what to do.
      buffer->GetData() = xt::cast<std::complex<float>>(
          xt::cast<std::complex<double>>(buffer->GetData()) +
          predict_buffer_->GetModel(thread));
    }
  }

  if (apply_cal_step_) {
    apply_cal_step_->process(std::move(buffer));
    buffer = result_step_->extract();
  }

  if (operation_ == Operation::kAdd) {
    buffer->GetData() += input_data_;
  } else if (operation_ == Operation::kSubtract) {
    buffer->GetData() = input_data_ - buffer->GetData();
  }

  timer_.stop();
  getNextStep()->process(std::move(buffer));
  return false;
}

everybeam::vector3r_t OnePredict::dir2Itrf(const MDirection& dir,
                                           MDirection::Convert& measConverter) {
  const MDirection& itrfDir = measConverter(dir);
  const casacore::Vector<double>& itrf = itrfDir.getValue().getValue();
  everybeam::vector3r_t vec;
  vec[0] = itrf[0];
  vec[1] = itrf[1];
  vec[2] = itrf[2];
  return vec;
}

void OnePredict::addBeamToData(std::shared_ptr<const base::Patch> patch,
                               double time, size_t thread,
                               xt::xtensor<std::complex<double>, 3>& data,
                               bool stokesIOnly) {
  // Apply beam for a patch, add result to Model
  MDirection dir(MVDirection(patch->direction().ra, patch->direction().dec),
                 MDirection::J2000);
  everybeam::vector3r_t srcdir = dir2Itrf(dir, meas_convertors_[thread]);

  if (stokesIOnly) {
    const common::ScopedMicroSecondAccumulator<decltype(apply_beam_time_)>
        scoped_time{apply_beam_time_};
    ApplyBeam::applyBeamStokesIArrayFactor(
        info(), time, data.data(), srcdir, telescope_.get(),
        predict_buffer_->GetScalarBeamValues(thread), false, beam_mode_,
        &mutex_);
  } else {
    {
      const common::ScopedMicroSecondAccumulator<decltype(apply_beam_time_)>
          scoped_time{apply_beam_time_};
      float* dummyweight = nullptr;
      ApplyBeam::applyBeam(info(), time, data.data(), dummyweight, srcdir,
                           telescope_.get(),
                           predict_buffer_->GetFullBeamValues(thread), false,
                           beam_mode_, false, &mutex_);
    }
  }

  // Add temporary buffer to Model
  predict_buffer_->GetModel(thread) += data;
}

void OnePredict::addBeamToData(std::shared_ptr<const base::Patch> patch,
                               double time, size_t thread,
                               xt::xtensor<std::complex<double>, 3>& data,
                               const std::pair<size_t, size_t>& baseline_range,
                               const std::pair<size_t, size_t>& station_range,
                               aocommon::Barrier& barrier, bool stokesIOnly) {
  // Apply beam for a patch, add result to Model
  MDirection dir(MVDirection(patch->direction().ra, patch->direction().dec),
                 MDirection::J2000);
  everybeam::vector3r_t srcdir = dir2Itrf(dir, meas_convertors_[thread]);

  // We use a common buffer to calculate beam values
  const size_t common_thread = 0;
  if (stokesIOnly) {
    const common::ScopedMicroSecondAccumulator<decltype(apply_beam_time_)>
        scoped_time{apply_beam_time_};
    ApplyBeam::applyBeamStokesIArrayFactor(
        info(), time, data.data(), srcdir, telescope_.get(),
        predict_buffer_->GetScalarBeamValues(common_thread), baseline_range,
        station_range, barrier, false, beam_mode_, &mutex_);
  } else {
    const common::ScopedMicroSecondAccumulator<decltype(apply_beam_time_)>
        scoped_time{apply_beam_time_};
    float* dummyweight = nullptr;
    ApplyBeam::applyBeam(
        info(), time, data.data(), dummyweight, srcdir, telescope_.get(),
        predict_buffer_->GetFullBeamValues(common_thread), baseline_range,
        station_range, barrier, false, beam_mode_, false, &mutex_);
  }

  // Add temporary buffer to Model
  predict_buffer_->GetModel(thread) += data;
}

void OnePredict::finish() {
  // Let the next steps finish.
  getNextStep()->finish();
}
}  // namespace steps
}  // namespace dp3
