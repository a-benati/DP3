// Copyright (C) 2021 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BdaDdeCal.h"

#include "BDAResultStep.h"
#include "BdaPredict.h"
#include "Version.h"

#include "../base/DPInfo.h"
#include "../common/ParameterValue.h"
#include "../common/StreamUtil.h"
#include "../ddecal/gain_solvers/SolveData.h"
#include "../ddecal/SolverFactory.h"

#include <boost/make_unique.hpp>

#include <algorithm>

using dp3::base::BDABuffer;
using dp3::ddecal::BdaSolverBuffer;
using dp3::ddecal::SolveData;
using dp3::steps::BdaPredict;
using dp3::common::operator<<;
using dp3::base::DPInfo;

namespace dp3 {
namespace steps {

BdaDdeCal::BdaDdeCal(InputStep* input, const common::ParameterSet& parset,
                     const std::string& prefix)
    : settings_(parset, prefix),
      solution_writer_(),
      steps_(),
      result_steps_(),
      patches_(),
      data_buffers_(),
      model_buffers_(),
      solver_buffer_(),
      solver_(),
      solution_interval_(0.0),
      chan_block_start_freqs_(),
      antennas1_(),
      antennas2_(),
      solutions_(),
      iterations_(),
      approx_iterations_(),
      constraint_solutions_() {
  if (settings_.directions.empty()) {
    throw std::invalid_argument(
        "Invalid info in input parset: direction(s) must be specified");
  }
  InitializePredictSteps(input, parset, prefix);

  if (!settings_.only_predict) {
    solver_ = ddecal::CreateBdaSolver(settings_, parset, prefix);
    for (ddecal::SolverBase* constraint_solver : solver_->ConstraintSolvers()) {
      if (!constraint_solver->GetConstraints().empty()) {
        throw std::runtime_error(
            "BDA Solver constraints are not implemented in BdaDdeCal. "
            "Use a solver without constraints (scalar or diagonal).");
      }
    }
    solution_writer_ =
        boost::make_unique<ddecal::SolutionWriter>(settings_.h5parm_name);
  }
}

void BdaDdeCal::InitializePredictSteps(InputStep* input,
                                       const common::ParameterSet& parset,
                                       const string& prefix) {
  for (const std::string& direction : settings_.directions) {
    const std::vector<std::string> source_patterns =
        common::ParameterValue(direction).getStringVector();

    steps_.push_back(
        std::make_shared<BdaPredict>(*input, parset, prefix, source_patterns));
    result_steps_.push_back(std::make_shared<BDAResultStep>());
    steps_.back()->setNextStep(result_steps_.back());

    patches_.push_back(common::ParameterValue(direction).getStringVector());
  }
}

void BdaDdeCal::updateInfo(const DPInfo& _info) {
  Step::updateInfo(_info);

  // Update info for substeps
  for (unsigned int i = 0; i < settings_.directions.size(); i++) {
    steps_[i]->setInfo(_info);
  }

  if (!settings_.only_predict) {
    solution_interval_ = _info.timeInterval() * info().ntime();
    size_t n_solution_intervals = 1;
    if (settings_.solution_interval > 0) {
      solution_interval_ = _info.timeInterval() * settings_.solution_interval;
      n_solution_intervals =
          (info().ntime() + settings_.solution_interval - 1) /
          settings_.solution_interval;
    }

    solver_buffer_ = boost::make_unique<ddecal::BdaSolverBuffer>(
        settings_.directions.size(), _info.startTime(), solution_interval_);

    DetermineChannelBlocks();

    // Create lists with used antenna indices, similary to
    // DPInfo::removeUnusedAnt.
    antennas1_.resize(info().getAnt1().size());
    antennas2_.resize(info().getAnt2().size());
    for (unsigned int i = 0; i < antennas1_.size(); ++i) {
      antennas1_[i] = info().antennaMap()[info().getAnt1()[i]];
      antennas2_[i] = info().antennaMap()[info().getAnt2()[i]];
    }

    solver_->SetNThreads(info().nThreads());
    solver_->Initialize(info().antennaUsed().size(),
                        settings_.directions.size(),
                        chan_block_start_freqs_.size() - 1);

    // SolveCurrentInterval will add solution intervals to the solutions.
    solutions_.reserve(n_solution_intervals);
    constraint_solutions_.reserve(n_solution_intervals);
  }

  if (solution_writer_) {
    // Convert Casacore types to STL types and pass antenna info to the
    // SolutionWriter.
    const std::vector<std::string> antenna_names(info().antennaNames().begin(),
                                                 info().antennaNames().end());

    std::vector<std::array<double, 3>> antenna_pos(info().antennaPos().size());
    for (unsigned int i = 0; i < info().antennaNames().size(); ++i) {
      casacore::Quantum<casacore::Vector<double>> pos =
          info().antennaPos()[i].get("m");
      antenna_pos[i][0] = pos.getValue()[0];
      antenna_pos[i][1] = pos.getValue()[1];
      antenna_pos[i][2] = pos.getValue()[2];
    }

    solution_writer_->AddAntennas(antenna_names, antenna_pos);
  }
}

void BdaDdeCal::DetermineChannelBlocks() {
  // Combine channels into channel blocks. Since many baselines are not
  // averaged, use the same strategy as the regular DdeCal.
  size_t n_channel_blocks = 1;
  if (settings_.n_channels > 0) {
    n_channel_blocks =
        std::max(info().nchan() / settings_.n_channels, size_t(1));
  }

  // Although info().chanWidths(baseline) differs between baselines,
  // min_freq and max_freq should be equal for all baselines.
  const double min_freq =
      info().chanFreqs(0).front() - (info().chanWidths(0).front() / 2);
  const double max_freq =
      info().chanFreqs(0).back() + (info().chanWidths(0).back() / 2);
  const double chan_width = (max_freq - min_freq) / info().nchan();

  chan_block_start_freqs_.clear();
  chan_block_start_freqs_.reserve(n_channel_blocks + 1);
  chan_block_start_freqs_.push_back(min_freq);
  size_t start_index = 0;
  double start_freq = min_freq;

  for (size_t ch_block = 0; ch_block < n_channel_blocks; ++ch_block) {
    const size_t next_index =
        (ch_block + 1) * info().nchan() / n_channel_blocks;
    const size_t block_size = next_index - start_index;
    const double next_freq = start_freq + block_size * chan_width;
    chan_block_start_freqs_.push_back(next_freq);
    start_index = next_index;
    start_freq = next_freq;
  }
}

bool BdaDdeCal::process(std::unique_ptr<base::BDABuffer> buffer) {
  BDABuffer::Fields fields(true);
  BDABuffer::Fields copyfields(false);
  copyfields.full_res_flags = true;
  copyfields.flags = true;

  // Feed metadata-only copies of the buffer to the steps.
  for (std::shared_ptr<ModelDataStep>& step : steps_) {
    step->process(boost::make_unique<BDABuffer>(*buffer, fields, copyfields));
  }

  if (!settings_.only_predict) {
    // Store the input buffer. When all predict sub-steps have completed a model
    // buffer, give the input buffer and model buffers to the solver_buffer_.
    data_buffers_.push_back(std::move(buffer));
  }

  ExtractResults();

  ProcessCompleteDirections();

  return true;
}

void BdaDdeCal::ExtractResults() {
  // The BDABuffers from the sub-steps should have the same shape, however, a
  // step may delay outputting steps, e.g., due to internal buffering.
  for (size_t direction = 0; direction < result_steps_.size(); direction++) {
    std::vector<std::unique_ptr<BDABuffer>> results =
        result_steps_[direction]->Extract();
    if (!results.empty()) {
      // Find the queue index of the first free slot for this direction.
      size_t queue_index = 0;
      while (model_buffers_.size() > queue_index &&
             model_buffers_[queue_index][direction]) {
        ++queue_index;
      }

      for (std::unique_ptr<BDABuffer>& result : results) {
        // Extend the queue if it's not big enough.
        if (queue_index == model_buffers_.size()) {
          model_buffers_.emplace_back(result_steps_.size());
        }
        model_buffers_[queue_index][direction] = std::move(result);
        ++queue_index;
      }
    }
  }
}

void BdaDdeCal::ProcessCompleteDirections() {
  const auto pointer_is_set = [](const std::unique_ptr<BDABuffer>& pointer) {
    return !!pointer;  // Convert the pointer to a boolean.
  };
  while (!model_buffers_.empty() &&
         std::all_of(model_buffers_.front().begin(),
                     model_buffers_.front().end(), pointer_is_set)) {
    std::vector<std::unique_ptr<base::BDABuffer>>& direction_buffers =
        model_buffers_.front();
    if (settings_.only_predict) {
      // Add all model buffers and use that as the result.
      std::complex<float> restrict* data = direction_buffers.front()->GetData();
      const size_t data_size = direction_buffers.front()->GetNumberOfElements();

      for (size_t dir = 1; dir < direction_buffers.size(); dir++) {
        assert(direction_buffers[dir]->GetNumberOfElements() == data_size);
        const std::complex<float> restrict* other_data =
            direction_buffers[dir]->GetData();
        for (size_t j = 0; j < data_size; ++j) data[j] += other_data[j];
        direction_buffers[dir].reset();
      }

      getNextStep()->process(std::move(direction_buffers.front()));
    } else {
      // Send data buffer and model_buffers to solver_buffer_.
      assert(!data_buffers_.empty() && data_buffers_.front());
      solver_buffer_->AppendAndWeight(std::move(data_buffers_.front()),
                                      std::move(direction_buffers));
      data_buffers_.pop_front();
    }
    model_buffers_.pop_front();
  }

  if (!settings_.only_predict) {
    while (solver_buffer_->IntervalIsComplete()) {
      SolveCurrentInterval();
      solver_buffer_->AdvanceInterval();
    }

    for (std::unique_ptr<BDABuffer>& done_buffer : solver_buffer_->GetDone()) {
      getNextStep()->process(std::move(done_buffer));
    }
  }
}

void BdaDdeCal::SolveCurrentInterval() {
  const size_t n_channel_blocks = chan_block_start_freqs_.size() - 1;
  const size_t n_antennas = info().antennaUsed().size();

  dp3::ddecal::SolveData data(*solver_buffer_, n_channel_blocks,
                              settings_.directions.size(), n_antennas,
                              antennas1_, antennas2_);

  const int current_interval = solutions_.size();
  assert(current_interval == solver_buffer_->GetCurrentInterval());
  const double current_center =
      info().startTime() + (current_interval + 0.5) * solution_interval_;

  solutions_.emplace_back(n_channel_blocks);
  const size_t block_solution_size = settings_.directions.size() * n_antennas *
                                     solver_->NSolutionPolarizations();
  for (std::vector<std::complex<double>>& block_solution : solutions_.back()) {
    block_solution.assign(block_solution_size, 1.0);
  }

  InitializeCurrentSolutions();

  ddecal::SolverBase::SolveResult result =
      solver_->Solve(data, solutions_.back(), current_center, nullptr);

  assert(iterations_.size() == solutions_.size() - 1);
  assert(approx_iterations_.size() == solutions_.size() - 1);
  iterations_.push_back(result.iterations);
  approx_iterations_.push_back(result.constraint_iterations);

  if (settings_.subtract) {
    solver_buffer_->SubtractCorrectedModel(
        solutions_.back(), chan_block_start_freqs_,
        solver_->NSolutionPolarizations(), antennas1_, antennas2_,
        info().BdaChanFreqs());
  }

  // Check for nonconvergence and flag if desired. Unconverged solutions are
  // identified by the number of iterations being one more than the max
  // allowed number.
  if (settings_.flag_unconverged &&
      result.iterations > solver_->GetMaxIterations()) {
    if (settings_.flag_diverged_only) {
      // Set negative weights (indicating unconverged solutions that diverged)
      // to zero. All other unconverged solutions remain unflagged.
      for (auto& constraint_results : result.results) {
        for (auto& constraint_result : constraint_results) {
          for (double& weight : constraint_result.weights) {
            weight = std::max(weight, 0.0);
          }
        }
      }
    } else {
      // Set all weights to zero
      for (auto& constraint_results : result.results) {
        for (auto& constraint_result : constraint_results) {
          std::fill(constraint_result.weights.begin(),
                    constraint_result.weights.end(), 0.0);
        }
      }
    }
  } else {
    // Set negative weights (indicating unconverged solutions that diverged)
    // to one. All other unconverged solutions are unflagged already.
    for (auto& constraint_results : result.results) {
      for (auto& constraint_result : constraint_results) {
        for (double& weight : constraint_result.weights) {
          if (weight < 0.0) weight = 1.0;
        }
      }
    }
  }

  // Store constraint solutions if any constaint has a non-empty result.
  if (std::any_of(
          result.results.begin(), result.results.end(),
          [](const std::vector<ddecal::Constraint::Result>&
                 constraint_results) { return !constraint_results.empty(); })) {
    constraint_solutions_.push_back(std::move(result.results));
  } else {  // Add an empty constraint solution for the solution interval.
    constraint_solutions_.emplace_back();
  }
  assert(solutions_.size() == constraint_solutions_.size());
}

void BdaDdeCal::InitializeCurrentSolutions() {
  std::vector<std::vector<std::complex<double>>>& current_solution =
      solutions_.back();

  bool propagate = solutions_.size() > 1 && settings_.propagate_solutions;
  if (propagate && settings_.propagate_converged_only &&
      iterations_[solutions_.size() - 2] > solver_->GetMaxIterations()) {
    propagate = false;
  }

  if (propagate) {
    // Copy the data from the previous solution, without reallocating
    // data structures.
    const std::vector<std::vector<std::complex<double>>>& prev_solution =
        *(solutions_.end() - 2);
    assert(current_solution.size() == prev_solution.size());

    for (size_t block = 0; block < current_solution.size(); ++block) {
      auto& current_block = current_solution[block];
      const auto& prev_block = prev_solution[block];
      assert(current_block.size() == prev_block.size());
      std::copy(prev_block.begin(), prev_block.end(), current_block.begin());
    }
  } else if (solver_->NSolutionPolarizations() == 4) {
    // Initialize full jones solutions with unity matrix.
    for (auto& current_block : current_solution) {
      for (size_t i = 0; i < current_block.size(); i += 4) {
        current_block[i + 0] = 1.0;
        current_block[i + 1] = 0.0;
        current_block[i + 2] = 0.0;
        current_block[i + 3] = 1.0;
      }
    }
  } else {
    // Initial scalar and diagonal solutions with 1.0.
    for (auto& block_solution : current_solution) {
      std::fill(block_solution.begin(), block_solution.end(), 1.0);
    }
  }
}

void BdaDdeCal::finish() {
  for (std::shared_ptr<ModelDataStep>& step : steps_) {
    step->finish();
  }

  ExtractResults();
  ProcessCompleteDirections();
  assert(data_buffers_.empty());

  if (!settings_.only_predict) {
    while (solver_buffer_->BufferCount() > 0) {
      SolveCurrentInterval();
      solver_buffer_->AdvanceInterval();
    }
    for (std::unique_ptr<BDABuffer>& done_buffer : solver_buffer_->GetDone()) {
      getNextStep()->process(std::move(done_buffer));
    }
    if (solution_writer_) WriteSolutions();
  }

  getNextStep()->finish();
}

void BdaDdeCal::WriteSolutions() {
  // Create antenna info for H5Parm, used antennas only.
  std::vector<std::string> used_antenna_names;
  used_antenna_names.reserve(info().antennaUsed().size());
  for (size_t used_antenna : info().antennaUsed()) {
    used_antenna_names.emplace_back(info().antennaNames()[used_antenna]);
  }

  std::vector<std::pair<double, double>> source_positions;
  source_positions.reserve(steps_.size());
  for (const std::shared_ptr<ModelDataStep>& s : steps_) {
    source_positions.push_back(s->GetFirstDirection());
  }

  std::vector<double> chan_block_freqs;
  chan_block_freqs.reserve(chan_block_start_freqs_.size() - 1);
  for (size_t i = 0; i < chan_block_start_freqs_.size() - 1; ++i) {
    chan_block_freqs.push_back(
        (chan_block_start_freqs_[i] + chan_block_start_freqs_[i + 1]) * 0.5);
  }

  const std::string history = "CREATE by " + DP3Version::AsString() + "\n" +
                              "step " + settings_.name + " in parset: \n" +
                              settings_.parset_string;

  solution_writer_->Write(solutions_, constraint_solutions_, info().startTime(),
                          solution_interval_, settings_.mode,
                          used_antenna_names, source_positions, patches_,
                          info().chanFreqs(), chan_block_freqs, history);
}

void BdaDdeCal::show(std::ostream& stream) const {
  stream << "BdaDdeCal " << settings_.name << '\n'
         << "  mode (constraints):  " << ToString(settings_.mode) << '\n'
         << "  directions:          " << patches_ << '\n';
  if (solver_) {
    const size_t nchan = settings_.n_channels == 0 ? size_t(getInfo().nchan())
                                                   : settings_.n_channels;
    stream << "  solver algorithm:    "
           << ddecal::ToString(settings_.solver_algorithm) << '\n'
           << "  H5Parm:              " << settings_.h5parm_name << '\n'
           << "  subtract model:      " << std::boolalpha << settings_.subtract
           << '\n'
           << "  solution interval:   " << solution_interval_ << " s\n"
           << "  #channels/block:     " << nchan << '\n'
           << "  #channel blocks:     " << chan_block_start_freqs_.size() - 1
           << '\n'
           << "  tolerance:           " << solver_->GetAccuracy() << '\n'
           << "  max iter:            " << solver_->GetMaxIterations() << '\n'
           << "  flag unconverged:    " << std::boolalpha
           << settings_.flag_unconverged << '\n'
           << "     diverged only:    " << std::boolalpha
           << settings_.flag_diverged_only << '\n'
           << "  propagate solutions: " << std::boolalpha
           << settings_.propagate_solutions << '\n'
           << "       converged only: " << std::boolalpha
           << settings_.propagate_converged_only << '\n'
           << "  detect stalling:     " << std::boolalpha
           << solver_->GetDetectStalling() << '\n'
           << "  step size:           " << solver_->GetStepSize() << '\n';
  }

  for (size_t dir = 0; dir < steps_.size(); ++dir) {
    stream << "Model steps for direction " << patches_[dir] << '\n';
    for (std::shared_ptr<Step> step = steps_[dir]; step;
         step = step->getNextStep()) {
      step->show(stream);
    }
    stream << '\n';
  }
}

}  // namespace steps
}  // namespace dp3
