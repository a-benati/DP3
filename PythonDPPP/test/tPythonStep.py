# tPythonStep.py: Test python DPPP class
#
# Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import print_function

from lofar.pythondppp import DPStep
from lofar.parameterset import parameterset

class tPythonStep(DPStep):
    def __init__(self, parsetDict):
        # The constructor gets the subset of the NDPPP parset containing
        # all keys-value pairs for this step.
        # Note: the superclass constructor MUST be called.
        DPStep.__init__(self, parsetDict)
        parset = parameterset(parsetDict)
        self.itsIncr = parset.getDouble('incr', 1)

    def updateInfo(self, dpinfo):
        # This function must be implemented.
        self.itsInfo = dpinfo
        # Make the arrays that will get the input buffer data from
        # the getData, etc. calls in the process function.
        self.itsData = self.makeArrayDataIn()
        self.itsFlags = self.makeArrayFlagsIn()
        self.itsWeights = self.makeArrayWeightsIn()
        self.itsUVW = self.makeArrayUVWIn()
        # Return the dict with info fields that change in this step.
        return {};

    def process(self, time, exposure):
        # This function must be implemented.
        # First get the data arrays needed by this step.
        self.getData (self.itsData);
        self.getFlags (self.itsFlags);
        self.getWeights (self.itsWeights);
        self.getUVW (self.itsUVW);
        # Process the data.
        print("process tPythonStep", time-4.47203e9, exposure, self.itsData.sum(), self.itsFlags.sum(), self.itsWeights.sum(), self.itsUVW.sum())
        # Execute the next step in the DPPP pipeline. TIME,UVW are changed.
        return self.processNext ({'TIME': time+self.itsIncr, 'UVW': self.itsUVW+self.itsIncr})

    def finish(self):
        # Finish the step as needed.
        # This function does not need to be implemented.
        # Note: finish of the next step is called by the C++ layer.
        print("finish tPythonStep")

    def showCounts(self):
        # Show the counts of this test.
        # This function does not need to be implemented.
        return "   **showcounttest**"

    def addToMS(self, msname):
        # Add some info the the output MeasurementSet.
        # This function does not need to be implemented.
        print("addToMS tPythonStep", msname)
