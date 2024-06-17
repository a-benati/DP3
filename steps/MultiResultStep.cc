// Copyright (C) 2022 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MultiResultStep.h"

#include <cassert>

#include "NullStep.h"

namespace dp3 {
namespace steps {

MultiResultStep::MultiResultStep(unsigned int size) : buffers_(size), size_(0) {
  setNextStep(std::make_shared<NullStep>());
}

bool MultiResultStep::process(std::unique_ptr<base::DPBuffer> buffer) {
  if (size_ >= buffers_.size()) {
      std::cerr << "Error: size_ exceeds buffers_.size()" << std::endl;
      return false;  // Prevent the loop from continuing
  }
  assert(size_ < buffers_.size());

  // If the next step is not a NullStep, one copy of the buffer is saved into
  // buffers_[size_] and the other is moved to the next step's process()
  // function.
  if (dynamic_cast<NullStep*>(getNextStep().get()) == nullptr) {
    buffers_[size_] = std::make_unique<base::DPBuffer>(*buffer);
    ++size_;
    getNextStep()->process(std::move(buffer));
  } else {
    buffers_[size_] = std::move(buffer);
    ++size_;
  }
  return true;
}

}  // namespace steps
}  // namespace dp3
