// PointSource.h: Point source model component with optional spectral index and
// rotation measure.
//
// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DPPP_POINTSOURCE_H
#define DPPP_POINTSOURCE_H

#include <vector>

#include "ModelComponent.h"
#include "Position.h"
#include "Stokes.h"

#include <memory>

namespace DP3 {
namespace DPPP {

/// \brief Point source model component with optional spectral index and
/// rotation measure.

/// @{

class PointSource : public ModelComponent {
 public:
  typedef std::shared_ptr<PointSource> Ptr;
  typedef std::shared_ptr<const PointSource> ConstPtr;

  PointSource(const Position &position);
  PointSource(const Position &position, const Stokes &stokes);

  virtual const Position &position() const;
  void setPosition(const Position &position);

  void setStokes(const Stokes &stokes);

  template <typename T>
  void setSpectralTerms(double refFreq, bool isLogarithmic, T first, T last);

  void setRotationMeasure(double fraction, double angle, double rm);

  Stokes stokes(double freq) const;

  virtual void accept(ModelComponentVisitor &visitor) const;

 private:
  bool hasSpectralTerms() const;
  bool hasRotationMeasure() const;

  Position itsPosition;
  Stokes itsStokes;
  double itsRefFreq;
  std::vector<double> itsSpectralTerms;
  double itsPolarizedFraction;
  double itsPolarizationAngle;
  double itsRotationMeasure;
  bool itsHasRotationMeasure;
  bool itsHasLogarithmicSI;
};

/// @}

// -------------------------------------------------------------------------- //
// - Implementation: PointSource                                            - //
// -------------------------------------------------------------------------- //

template <typename T>
void PointSource::setSpectralTerms(double refFreq, bool isLogarithmic, T first,
                                   T last) {
  itsRefFreq = refFreq;
  itsHasLogarithmicSI = isLogarithmic;
  itsSpectralTerms.clear();
  itsSpectralTerms.insert(itsSpectralTerms.begin(), first, last);
}

inline const Position &PointSource::position() const { return itsPosition; }

}  // namespace DPPP
}  // namespace DP3

#endif
