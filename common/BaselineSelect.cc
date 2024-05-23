// BaselineSelect.cc: Convert MSSelection baseline string to a Matrix
//
// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BaselineSelect.h"

#include <cassert>

#include <casacore/ms/MeasurementSets/MeasurementSet.h>
#include <casacore/ms/MeasurementSets/MSAntenna.h>
#include <casacore/ms/MeasurementSets/MSAntennaColumns.h>
#include <casacore/ms/MSSel/MSSelection.h>
#include <casacore/ms/MSSel/MSAntennaParse.h>
#include <casacore/ms/MSSel/MSSelectionErrorHandler.h>
#include <casacore/ms/MSSel/MSAntennaGram.h>
#include <casacore/tables/Tables/Table.h>
#include <casacore/tables/Tables/SetupNewTab.h>
#include <casacore/tables/Tables/TableRecord.h>
#include <casacore/tables/TaQL/TableParse.h>
#include <casacore/tables/Tables/ScalarColumn.h>
#include <casacore/tables/Tables/ScaColDesc.h>
#include <casacore/measures/Measures/MPosition.h>
#include <casacore/casa/Arrays/Matrix.h>
#include <casacore/casa/Arrays/Vector.h>
#include <casacore/casa/version.h>

#include <dp3/common/Types.h>

using casacore::Matrix;
using casacore::MPosition;
using casacore::MSAntenna;
using casacore::MSAntennaColumns;
using casacore::MSAntennaParse;
using casacore::MSSelectionErrorHandler;
using casacore::ScalarColumn;
using casacore::ScalarColumnDesc;
using casacore::SetupNewTable;
using casacore::Table;
using casacore::TableDesc;
using casacore::TableExprNode;

namespace dp3 {
namespace common {

casacore::Matrix<bool> BaselineSelect::convert(
    const std::vector<std::string>& names, const std::vector<MPosition>& pos,
    const std::vector<int>& antenna1, const std::vector<int>& antenna2,
    const std::string& baseline_selection, std::ostream& os) {
  assert(names.size() == pos.size());
  assert(antenna1.size() == antenna2.size());

  // Create a temporary MSAntenna table in memory for parsing purposes.
  SetupNewTable antNew(casacore::String(), MSAntenna::requiredTableDesc(),
                       Table::New);
  Table anttab(antNew, Table::Memory, names.size());
  MSAntenna msant(anttab);
  MSAntennaColumns antcol(msant);
  antcol.name().putColumn(casacore::Vector<casacore::String>(names));
  for (size_t i = 0; i < pos.size(); ++i) {
    antcol.positionMeas().put(i, pos[i]);
  }
  // Create a temporary table holding the antenna numbers of the baselines.
  TableDesc td;
  td.addColumn(ScalarColumnDesc<int>("ANTENNA1"));
  td.addColumn(ScalarColumnDesc<int>("ANTENNA2"));
  SetupNewTable tabNew(casacore::String(), td, Table::New);
  Table tab(tabNew, Table::Memory, antenna1.size());
  ScalarColumn<int> ac1(tab, "ANTENNA1");
  ScalarColumn<int> ac2(tab, "ANTENNA2");
  ac1.putColumn(casacore::Vector<casacore::Int>(antenna1));
  ac2.putColumn(casacore::Vector<casacore::Int>(antenna2));

  // Do the selection using the temporary tables.
  TableExprNode a1(tab.col("ANTENNA1"));
  TableExprNode a2(tab.col("ANTENNA2"));
  return convert(anttab, a1, a2, baseline_selection, os);
}

Matrix<bool> BaselineSelect::convert(Table& anttab, TableExprNode& a1Node,
                                     TableExprNode& a2Node,
                                     const string& baselineSelection,
                                     std::ostream& os) {
  // Overwrite the error handler to ignore errors for unknown antennas.
  // First construct MSSelection, because it resets the error handler.
  casacore::Vector<int> selectedAnts1;
  casacore::Vector<int> selectedAnts2;
  Matrix<int> selectedBaselines;
  auto curHandler = MSAntennaParse::thisMSAErrorHandler;
#if CASACORE_MAJOR_VERSION < 3 ||    \
    (CASACORE_MAJOR_VERSION == 3 &&  \
     (CASACORE_MINOR_VERSION == 0 || \
      (CASACORE_MINOR_VERSION == 1 && CASACORE_PATCH_VERSION < 2)))
  // In casacore < 3.1.2 thisMSAErrorHandler is a raw pointer,
  // From casacore 3.1.2. it's a CountedPtr
  BaselineSelectErrorHandler errorHandler(os);
  MSAntennaParse::thisMSAErrorHandler = &errorHandler;
#else
  // After casacore 3.5.0, the type of MSAntennaParse::thisMSAErrorHandler
  // changed again from a CountedPtr to a unique_ptr, so derive the appropriate
  // type:
  using CasacorePointerType = decltype(MSAntennaParse::thisMSAErrorHandler);
  CasacorePointerType errorHandler(new common::BaselineSelectErrorHandler(os));
  MSAntennaParse::thisMSAErrorHandler = std::move(errorHandler);
#endif
  try {
    // Create a table expression representing the selection.
    TableExprNode node = msAntennaGramParseCommand(
        anttab, a1Node, a2Node, baselineSelection, selectedAnts1, selectedAnts2,
        selectedBaselines);
    // Get the antenna numbers.
    Table seltab = a1Node.table()(node);
    casacore::Vector<int> a1 =
        ScalarColumn<int>(seltab, "ANTENNA1").getColumn();
    casacore::Vector<int> a2 =
        ScalarColumn<int>(seltab, "ANTENNA2").getColumn();
    int nant = anttab.nrow();
    Matrix<bool> bl(nant, nant, false);
    for (unsigned int i = 0; i < a1.size(); ++i) {
      bl(a1[i], a2[i]) = true;
      bl(a2[i], a1[i]) = true;
    }
    MSAntennaParse::thisMSAErrorHandler = curHandler;
    return bl;
  } catch (const std::exception&) {
    MSAntennaParse::thisMSAErrorHandler = curHandler;
    throw;
  }
}

BaselineSelectErrorHandler::~BaselineSelectErrorHandler() {}

void BaselineSelectErrorHandler::reportError(const char* token,
                                             const casacore::String message) {
  itsStream << message << token << '\n';
}

}  // namespace common
}  // namespace dp3
