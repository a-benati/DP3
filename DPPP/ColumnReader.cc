// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ColumnReader.h"

#include <casacore/tables/Tables/ArrayColumn.h>

using casacore::ArrayColumn;

namespace DP3 {
namespace DPPP {

ColumnReader::ColumnReader(DPInput& input, const ParameterSet& parset,
                           const string& prefix, const string& column)
    : input_(input),
      name_(prefix),
      column_name_(parset.getString(prefix + "column", column)),
      operation_(parset.getString(prefix + "operation", "replace")),
      buffer_() {
  if (operation_ != "subtract" && operation_ != "add" &&
      operation_ != "replace") {
    throw std::invalid_argument("Invalid ColumnReader operation " + operation_);
  }
}

bool ColumnReader::process(const DPBuffer& buffer) {
  buffer_.copy(buffer);
  ArrayColumn<casacore::Complex> model_col(input_.table(), column_name_);
  model_col.getColumnCells(buffer.getRowNrs(), buffer_.getData());

  if (operation_ == "add") {
    buffer_.setData(buffer.getData() + buffer_.getData());
  } else if (operation_ == "subtract") {
    buffer_.setData(buffer.getData() - buffer_.getData());
  }

  getNextStep()->process(buffer_);

  return false;
}

void ColumnReader::updateInfo(const DPInfo& _info) {
  DPStep::updateInfo(_info);
  info().setNeedVisData();
  info().setWriteData();
}

void ColumnReader::finish() { getNextStep()->finish(); }

void ColumnReader::show(std::ostream& os) const {
  os << "ColumnReader " << name_ << '\n';
  os << "  column:      " << column_name_ << '\n';
  os << "  operation:   " << operation_ << '\n';
}

void ColumnReader::showTimings(std::ostream& os, double duration) const {
  os << " ColumnReader " << name_ << std::endl;
}

}  // namespace DPPP
}  // namespace DP3
