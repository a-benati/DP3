// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DDE_SOLVER_BASE_H
#define DDE_SOLVER_BASE_H

#include "../constraints/Constraint.h"

#include "SolverBuffer.h"

#include "../linear_solvers/LLSSolver.h"

#include <boost/algorithm/string.hpp>

#include <complex>
#include <vector>
#include <memory>

namespace dp3 {
namespace base {

class DPBuffer;

class SolverBase {
 public:
  typedef std::complex<double> DComplex;
  typedef std::complex<float> Complex;

  class Matrix : public std::vector<Complex> {
   public:
    Matrix() : columns_(0) {}
    Matrix(size_t columns, size_t rows)
        : std::vector<Complex>(columns * rows, 0.0), columns_(columns) {}
    void SetZero() { assign(size(), Complex(0.0, 0.0)); }
    Complex& operator()(size_t column, size_t row) {
      return (*this)[column + row * columns_];
    }

   private:
    size_t columns_;
  };

  struct SolveResult {
    size_t iterations;
    size_t constraint_iterations;
    std::vector<std::vector<Constraint::Result>> results;
  };

  SolverBase();

  virtual ~SolverBase() {}

  /**
   * Prepares the solver with the given dimensionality info
   * and antenna mapping.
   * The antenna arrays map the data provided in @Solve to the antennas.
   */
  void Initialize(size_t nAntennas, size_t nDirections, size_t nChannels,
                  size_t nChannelBlocks, const std::vector<int>& ant1,
                  const std::vector<int>& ant2);

  /**
   * Solves multi-directional Jones matrices. Takes the (single) measured data
   * and the (multi-directional) model data, and solves the optimization
   * problem that minimizes the norm of the differences.
   *
   * @param unweighted_data_buffers The measured data.
   * unweighted_data_buffers[i] holds the data for timestep i.
   * @param model_buffers The model data. model_buffers[i] is a vector for
   * timestep i with ndir buffers with model data. These
   * buffers have in the same structure as the data. Because the model data is
   * large* (e.g. tens of GB in extensive slow gain solves), the data is not
   * copied but weighted in place.
   * @param solutions The per-channel and per-antenna solutions.
   * solutions[ch] is a pointer for channelblock ch to antenna x directions x
   * pol solutions.
   */
  virtual SolveResult Solve(
      const std::vector<DPBuffer>& unweighted_data_buffers,
      const std::vector<std::vector<DPBuffer*>>& model_buffers,
      std::vector<std::vector<DComplex>>& solutions, double time,
      std::ostream* statStream) = 0;

  void AddConstraint(Constraint& constraint) {
    constraints_.push_back(&constraint);
  }

  /**
   * If enabled, the solver will perform steps along the complex
   * circle, instead of moving freely through complex space.
   * See the implementation of @ref MakeStep().
   */
  void SetPhaseOnly(bool phase_only) { phase_only_ = phase_only; }

  /**
   * Max nr of iterations (stopping criterion).
   * @{
   */
  size_t GetMaxIterations() const { return max_iterations_; }
  void SetMaxIterations(size_t max_iterations) {
    max_iterations_ = max_iterations;
  }
  /** @} */

  /**
   * Min nr of iterations before stopping.
   * @{
   */
  size_t GetMinIterations() const { return min_iterations_; }
  void SetMinIterations(size_t min_iterations) {
    min_iterations_ = min_iterations;
  }
  /** @} */

  /**
   * Required relative accuracy.
   * @{
   */
  void SetAccuracy(double accuracy) { accuracy_ = accuracy; }
  double GetAccuracy() const { return accuracy_; }
  /** @} */

  /**
   * Required relative accuracy for the constraints to finish.
   */
  void SetConstraintAccuracy(double constraint_accuracy) {
    constraint_accuracy_ = constraint_accuracy;
  }

  /**
   * The step size taken each iteration. Higher values might
   * make convergence faster, but may cause instability.
   * @{
   */
  void SetStepSize(double step_size) { step_size_ = step_size; }
  double GetStepSize() const { return step_size_; }
  /** @} */

  /**
   * Whether stalling of the solutions should abort the solving.
   * @{
   */
  void SetDetectStalling(bool detect_stalling) {
    detect_stalling_ = detect_stalling;
  }
  bool GetDetectStalling() const { return detect_stalling_; }
  /** @} */

  /**
   * Number of threads to use in parts that can be parallelized.
   * The solving is parallelized over channel blocks.
   */
  void SetNThreads(size_t n_threads) { n_threads_ = n_threads; }

  /**
   * Output timing information to a stream.
   */
  void GetTimings(std::ostream& os, double duration) const;

  void SetLLSSolverType(LLSSolverType solver,
                        std::pair<double, double> tolerances);

 protected:
  void Step(const std::vector<std::vector<DComplex>>& solutions,
            std::vector<std::vector<DComplex>>& next_solutions) const;

  bool DetectStall(size_t iteration,
                   const std::vector<double>& step_magnitudes) const;

  static void MakeSolutionsFinite1Pol(
      std::vector<std::vector<DComplex>>& solutions);

  static void MakeSolutionsFinite2Pol(
      std::vector<std::vector<DComplex>>& solutions);

  static void MakeSolutionsFinite4Pol(
      std::vector<std::vector<DComplex>>& solutions);

  /**
   * Assign the solutions in nextSolutions to the solutions.
   * @returns whether the solutions have converged. Appends the current step
   * magnitude to step_magnitudes
   */
  bool AssignSolutions(std::vector<std::vector<DComplex>>& solutions,
                       const std::vector<std::vector<DComplex>>& new_solutions,
                       bool use_constraint_accuracy, double& avg_abs_diff,
                       std::vector<double>& step_magnitudes, size_t nPol) const;

  template <typename T>
  static bool Isfinite(const std::complex<T>& val) {
    return std::isfinite(val.real()) && std::isfinite(val.imag());
  }

  double calculateLLSTolerance(double iteration_fraction,
                               double solver_precision) const;

  bool ReachedStoppingCriterion(
      size_t iteration, bool has_converged, bool constraints_satisfied,
      const std::vector<double>& step_magnitudes) const {
    bool has_stalled = false;
    if (detect_stalling_ && constraints_satisfied)
      has_stalled = DetectStall(iteration, step_magnitudes);

    const bool is_ready = iteration >= max_iterations_ ||
                          (has_converged && constraints_satisfied) ||
                          has_stalled;
    return iteration >= min_iterations_ && is_ready;
  }

  size_t n_antennas_;
  size_t n_directions_;
  size_t n_channels_;
  size_t n_channel_blocks_;
  std::vector<int> ant1_, ant2_;
  SolverBuffer buffer_;

  /**
   * Calibration setup
   * @{
   */
  size_t min_iterations_;
  size_t max_iterations_;
  size_t n_threads_;
  double accuracy_;
  double constraint_accuracy_;
  double step_size_;
  bool detect_stalling_;
  bool phase_only_;
  std::vector<Constraint*>
      constraints_;  // Does not own the Constraint objects.
  LLSSolverType lls_solver_type_;
  double lls_min_tolerance_;
  double lls_max_tolerance_;
  /** @} */
};

}  // namespace base
}  // namespace dp3

#endif
