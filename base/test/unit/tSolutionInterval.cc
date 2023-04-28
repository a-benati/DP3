// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "../../SolutionInterval.h"
#include <dp3/base/DPBuffer.h>
#include "../../../common/Timer.h"
#include "../../../steps/test/unit/mock/MockInput.h"

using dp3::base::DPBuffer;
using dp3::base::SolutionInterval;
using dp3::common::NSTimer;
using dp3::steps::MockInput;

namespace {
const int kNBL = 2;
const int kNCorr = 1;
const int kNChan = 1;
const std::array<size_t, 3> kShape{kNBL, kNChan, kNCorr};

std::unique_ptr<DPBuffer> InitBuffer() {
  auto buffer = std::make_unique<DPBuffer>();

  buffer->ResizeUvw(kNBL);
  for (int i = 0; i < kNBL; ++i) {
    buffer->GetUvw()(i, 0) = i * kNBL + 1;
    buffer->GetUvw()(i, 1) = i * kNBL + 2;
    buffer->GetUvw()(i, 2) = i * kNBL + 3;
  }

  buffer->ResizeData(kShape);
  for (int i = 0; i < int(buffer->GetData().size()); ++i) {
    buffer->GetData().data()[i] =
        casacore::Complex(i + i * kNBL * 10, i - 1000 + i * kNBL * 6);
  }

  buffer->ResizeFlags(kShape);
  buffer->GetFlags().fill(false);

  buffer->ResizeWeights(kShape);
  buffer->GetWeights().fill(1.0f);

  return buffer;
}
}  // namespace

BOOST_AUTO_TEST_SUITE(solutioninterval)

/// Test if buffer inserted is the same
BOOST_AUTO_TEST_CASE(insertion) {
  size_t buffer_size = 1;
  std::unique_ptr<DPBuffer> buffer = InitBuffer();
  const DPBuffer* const buffer_pointer = buffer.get();

  SolutionInterval solInt(buffer_size);
  BOOST_TEST(solInt.Size() == 0U);

  solInt.PushBack(std::move(buffer));
  BOOST_TEST_REQUIRE(solInt.Size() == 1U);

  BOOST_TEST(&solInt[0] == buffer_pointer);
}

BOOST_AUTO_TEST_SUITE_END()
