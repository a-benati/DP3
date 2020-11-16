// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// @author Lars Krombeen

#include "MS.h"

namespace DP3 {
namespace DPPP {
namespace DP3MS {

/// BDA_TIME_AXIS table.
const std::string kBDATimeAxisTable = "BDA_TIME_AXIS";
const std::string kTimeAxisId = "BDA_TIME_AXIS_ID";
const std::string kIsBdaApplied = "IS_BDA_APPLIED";
const std::string kMaxTimeInterval = "MAX_TIME_INTERVAL";
const std::string kMinTimeInterval = "MIN_TIME_INTERVAL";
const std::string kUnitTimeInterval = "UNIT_TIME_INTERVAL";
const std::string kIntervalFactors = "INTEGER_INTERVAL_FACTORS";
const std::string kHasBDAOrdering = "HAS_BDA_ORDERING";
const std::string kFieldId = "FIELD_ID";
const std::string kSingleFactorPerBL = "SINGLE_FACTOR_PER_BASELINE";

/// BDA_FACTORS table.
const std::string kBDAFactorsTable = "BDA_FACTORS";
const std::string kFactor = "FACTOR";

/// SPECTRAL_WINDOW table.
const std::string kSpectralWindowTable = "SPECTRAL_WINDOW";
const std::string kSpectralWindowId = "SPECTRAL_WINDOW_ID";
const std::string kDataDescTable = "DATA_DESCRIPTION";
const std::string kBDAFreqAxisId = "BDA_FREQ_AXIS_ID";
const std::string kBDASetId = "BDA_SET_ID";
const std::string kChanFreq = "CHAN_FREQ";
const std::string kChanWidth = "CHAN_WIDTH";
const std::string kEffectiveBW = "EFFECTIVE_BW";
const std::string kResolution = "RESOLUTION";
const std::string kNumChan = "NUM_CHAN";
const std::string kTotalBandWidth = "TOTAL_BANDWIDTH";
const std::string kRefFrequency = "REF_FREQUENCY";

const std::string kLofarAntennaSet = "LOFAR_ANTENNA_SET";

const std::string kAntennaTable = "ANTENNA";
const std::string kName = "NAME";
const std::string kDishDiameter = "DISH_DIAMETER";
const std::string kPosition = "POSITION";
const std::string kMeasFreqRef = "MEAS_FREQ_REF";

const std::string kObservationTable = "OBSERVATION";

}  // namespace DP3MS
}  // namespace DPPP
}  // namespace DP3
