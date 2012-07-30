//# Demixer.cc: DPPP step class to subtract A-team sources
//# Copyright (C) 2011
//# ASTRON (Netherlands Institute for Radio Astronomy)
//# P.O.Box 2, 7990 AA Dwingeloo, The Netherlands
//#
//# This file is part of the LOFAR software suite.
//# The LOFAR software suite is free software: you can redistribute it and/or
//# modify it under the terms of the GNU General Public License as published
//# by the Free Software Foundation, either version 3 of the License, or
//# (at your option) any later version.
//#
//# The LOFAR software suite is distributed in the hope that it will be useful,
//# but WITHOUT ANY WARRANTY; without even the implied warranty of
//# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//# GNU General Public License for more details.
//#
//# You should have received a copy of the GNU General Public License along
//# with the LOFAR software suite. If not, see <http://www.gnu.org/licenses/>.
//#
//# $Id$
//#
//# @author Ger van Diepen

#include <lofar_config.h>
#include <DPPP/Demixer.h>
#include <DPPP/Apply.h>
#include <DPPP/Averager.h>
#include <DPPP/CursorUtilCasa.h>
#include <DPPP/DPBuffer.h>
#include <DPPP/DPInfo.h>
#include <DPPP/EstimateMixed.h>
#include <DPPP/PhaseShift.h>
#include <DPPP/ParSet.h>
#include <DPPP/Simulate.h>
#include <DPPP/SourceDBUtil.h>
#include <DPPP/SubtractMixed.h>

#include <ParmDB/Axis.h>
#include <ParmDB/SourceDB.h>
#include <ParmDB/ParmDB.h>
#include <ParmDB/ParmSet.h>
#include <ParmDB/ParmCache.h>
#include <ParmDB/Parm.h>

#include <Common/LofarLogger.h>
#include <Common/OpenMP.h>
#include <Common/StreamUtil.h>
#include <Common/lofar_iomanip.h>
#include <Common/lofar_iostream.h>
#include <Common/lofar_fstream.h>

#include <casa/Quanta/MVAngle.h>
#include <casa/Arrays/Vector.h>
#include <casa/Arrays/Matrix.h>
#include <casa/Arrays/ArrayMath.h>
#include <casa/Arrays/MatrixMath.h>
#include <scimath/Mathematics/MatrixMathLA.h>

using namespace casa;

namespace LOFAR {
  namespace DPPP {

    using LOFAR::operator<<;

    namespace
    {
      string toString (double value);
      double getAngle (const String& value);
    } //# end unnamed namespace

    Demixer::Demixer (DPInput* input,
                      const ParSet& parset, const string& prefix)
      : itsInput          (input),
        itsName           (prefix),
        itsSkyName        (parset.getString(prefix+"skymodel", "sky")),
        itsInstrumentName (parset.getString(prefix+"instrumentmodel",
                                           "instrument")),
        itsAvgResultSubtr (0),
        itsTargetSource   (parset.getString(prefix+"targetsource", string())),
        itsSubtrSources   (parset.getStringVector (prefix+"subtractsources")),
        itsModelSources   (parset.getStringVector (prefix+"modelsources",
                                                   vector<string>())),
        itsExtraSources   (parset.getStringVector (prefix+"othersources",
                                                   vector<string>())),
//        itsCutOffs        (parset.getDoubleVector (prefix+"elevationcutoffs",
//                                                   vector<double>())),
//        itsJointSolve     (parset.getBool  (prefix+"jointsolve", true)),
        itsNDir           (0),
        itsNModel         (0),
        itsNStation       (0),
        itsNBl            (0),
        itsNCorr          (0),
        itsNChanIn        (0),
        itsNTimeIn        (0),
        itsNTimeDemix     (0),
        itsNChanAvgSubtr  (parset.getUint  (prefix+"freqstep", 1)),
        itsNTimeAvgSubtr  (parset.getUint  (prefix+"timestep", 1)),
        itsNChanOutSubtr  (0),
        itsNTimeOutSubtr  (0),
        itsNTimeChunk     (parset.getUint  (prefix+"ntimechunk", 0)),
        itsNTimeChunkSubtr(0),
        itsNChanAvg       (parset.getUint  (prefix+"demixfreqstep",
                                            itsNChanAvgSubtr)),
        itsNTimeAvg       (parset.getUint  (prefix+"demixtimestep",
                                            itsNTimeAvgSubtr)),
        itsNChanOut       (0),
        itsNTimeOut       (0),
        itsTimeIntervalAvg(0),
        itsTimeIndex      (0),
        itsNConverged     (0)
    {
      // Get and set solver options.
//      itsSolveOpt.maxIter =
//        parset.getUint  (prefix+"Solve.Options.MaxIter", 300);
//      itsSolveOpt.epsValue =
//        parset.getDouble(prefix+"Solve.Options.EpsValue", 1e-9);
//      itsSolveOpt.epsDerivative =
//        parset.getDouble(prefix+"Solve.Options.EpsDerivative", 1e-9);
//      itsSolveOpt.colFactor =
//        parset.getDouble(prefix+"Solve.Options.ColFactor", 1e-9);
//      itsSolveOpt.lmFactor  =
//        parset.getDouble(prefix+"Solve.Options.LMFactor", 1.0);
//      itsSolveOpt.balancedEq =
//        parset.getBool  (prefix+"Solve.Options.BalancedEqs", false);
//      itsSolveOpt.useSVD  =
//        parset.getBool  (prefix+"Solve.Options.UseSVD", true);

      ASSERTSTR (!(itsSkyName.empty() || itsInstrumentName.empty()),
                 "An empty name is given for the sky and/or instrument model");
      // Default nr of time chunks is maximum number of threads.
      if (itsNTimeChunk == 0) {
        itsNTimeChunk = OpenMP::maxThreads();
      }
      // Check that time windows fit integrally.
      ASSERTSTR ((itsNTimeChunk * itsNTimeAvg) % itsNTimeAvgSubtr == 0,
                 "time window should fit final averaging integrally");
      itsNTimeChunkSubtr = (itsNTimeChunk * itsNTimeAvg) / itsNTimeAvgSubtr;
      // Collect all source names.
      itsNModel = itsSubtrSources.size() + itsModelSources.size();
      itsNDir   = itsNModel + itsExtraSources.size() + 1;
      itsAllSources.reserve (itsNDir);
      itsAllSources.insert (itsAllSources.end(),
                            itsSubtrSources.begin(), itsSubtrSources.end());
      itsAllSources.insert (itsAllSources.end(),
                            itsModelSources.begin(), itsModelSources.end());
      itsAllSources.insert (itsAllSources.end(),
                            itsExtraSources.begin(), itsExtraSources.end());
      itsAllSources.push_back (itsTargetSource);

      // Get the source info of all patches from the SourceDB table.
      BBS::SourceDB sourceDB(BBS::ParmDBMeta("", itsSkyName), false);
      vector<string> patchNames(itsAllSources);
      // If the target source is given, add it to the model.
      // Because the target source has to be the last direction, it means
      // that (for the time being) no extra sources can be given.
      if (! itsTargetSource.empty()) {
        patchNames[itsNModel++] = itsTargetSource;
        // The target has to be the last demix direction.
        // If it has a source model, there cannot be any extra source
        // because the sources to be predicted have to be a consecutive vector.
        ASSERTSTR (itsExtraSources.empty(), "Currently no extrasources can "
                   "be given if the targetsource is given");
      }
      itsPatchList = makePatches (sourceDB, patchNames, itsNModel);
      ASSERT(itsPatchList.size() == itsNModel);

      // Size buffers.
      itsFactors.resize      (itsNTimeChunk);
      itsFactorsSubtr.resize (itsNTimeChunkSubtr);
      itsPhaseShifts.reserve (itsNDir-1);
      itsFirstSteps.reserve  (itsNDir+1);   // one extra for itsAvgSubtr
      itsAvgResults.reserve  (itsNDir);

      // Create the steps for the sources to be removed.
      // Demixing consists of the following steps:
      // - phaseshift data to each demix source
      // - average data in each direction, also for original phasecenter.
      // - determine demix factors for all directions
      // - use BBS to predict and solve in each direction. It is possible to
      //   predict more directions than to solve (for strong sources in field).
      // - use BBS to subtract the solved sources using the demix factors.
      //   The averaging used here can be smaller than used when solving.
      for (uint i=0; i<itsNDir-1; ++i) {
        // First make the phaseshift and average steps for each demix source.
        // The resultstep gets the result.
        // The phasecenter can be given in a parameter. Its name is the default.
        // Look up the source direction in the patch table.
        // If found, turn it into a vector of strings.
        vector<string> sourceVec (1, itsAllSources[i]);
        if(i < itsNModel) {
          sourceVec[0] = toString(itsPatchList[i]->position()[0]);
          sourceVec.push_back(toString(itsPatchList[i]->position()[1]));
        }
        PhaseShift* step1 = new PhaseShift (input, parset,
                                            prefix + itsAllSources[i] + '.',
                                            sourceVec);
        itsFirstSteps.push_back (DPStep::ShPtr(step1));
        itsPhaseShifts.push_back (step1);
        DPStep::ShPtr step2 (new Averager(input, prefix, itsNChanAvg,
                                          itsNTimeAvg));
        step1->setNextStep (step2);
        MultiResultStep* step3 = new MultiResultStep(itsNTimeChunk);
        step2->setNextStep (DPStep::ShPtr(step3));
        // There is a single demix factor step which needs to get all results.
        itsAvgResults.push_back (step3);
      }

      // Now create the step to average the data themselves.
      DPStep::ShPtr targetAvg(new Averager(input, prefix,
                                           itsNChanAvg, itsNTimeAvg));
      itsFirstSteps.push_back (targetAvg);
      MultiResultStep* targetAvgRes = new MultiResultStep(itsNTimeChunk);
      targetAvg->setNextStep (DPStep::ShPtr(targetAvgRes));
      itsAvgResults.push_back (targetAvgRes);

      // Create the data average step for the subtract.
      DPStep::ShPtr targetAvgSubtr(new Averager(input, prefix,
                                                itsNChanAvgSubtr,
                                                itsNTimeAvgSubtr));
      itsAvgResultSubtr = new MultiResultStep(itsNTimeChunkSubtr);
      targetAvgSubtr->setNextStep (DPStep::ShPtr(itsAvgResultSubtr));
      itsFirstSteps.push_back (targetAvgSubtr);

//      while(itsCutOffs.size() < itsNModel) {
//        itsCutOffs.push_back(0.0);
//      }
//      itsCutOffs.resize(itsNModel);
    }

    void Demixer::updateInfo (const DPInfo& infoIn)
    {
      info() = infoIn;

      // Get size info.
      itsNStation = infoIn.antennaNames().size();
      itsNChanIn  = infoIn.nchan();
      itsNBl      = infoIn.nbaselines();
      itsNCorr    = infoIn.ncorr();
      ASSERTSTR (itsNCorr==4, "Demixing requires data with 4 polarizations");
      itsFactorBuf.resize (IPosition(4, itsNCorr, itsNChanIn, itsNBl,
                                     itsNDir*(itsNDir-1)/2));
      itsFactorBufSubtr.resize (IPosition(4, itsNCorr, itsNChanIn, itsNBl,
                                     itsNDir*(itsNDir-1)/2));

      // Adapt averaging to available nr of channels and times.
      // Use a copy of the DPInfo, otherwise it is updated multiple times.
      DPInfo infoDemix(infoIn);
      itsNTimeAvg = std::min (itsNTimeAvg, infoIn.ntime());
      itsNChanAvg = infoDemix.update (itsNChanAvg, itsNTimeAvg);
      itsNChanOut = infoDemix.nchan();
      itsTimeIntervalAvg = infoDemix.timeInterval();
      itsNTimeDemix      = infoDemix.ntime();
      for (size_t i=0; i<infoIn.getAnt1().size(); ++i) {
        itsBaselines.push_back (Baseline(infoIn.getAnt1()[i],
                                         infoIn.getAnt2()[i]));
      }

      // Let the internal steps update their data.
      for (uint i=0; i<itsFirstSteps.size(); ++i) {
        itsFirstSteps[i]->setInfo (infoIn);
      }
      // Update the info of this object.
      info().setNeedVisData();
      info().setNeedWrite();
      itsNTimeAvgSubtr = std::min (itsNTimeAvgSubtr, infoIn.ntime());
      itsNChanAvgSubtr = info().update (itsNChanAvgSubtr, itsNTimeAvgSubtr);
      itsNChanOutSubtr = info().nchan();
      ASSERTSTR (itsNChanAvg % itsNChanAvgSubtr == 0,
        "Demix averaging " << itsNChanAvg
        << " must be multiple of output averaging "
        << itsNChanAvgSubtr);
      ASSERTSTR (itsNTimeAvg % itsNTimeAvgSubtr == 0,
        "Demix averaging " << itsNTimeAvg
        << " must be multiple of output averaging "
        << itsNTimeAvgSubtr);
      // Store channel frequencies for the demix and subtract resolutions.
      itsFreqDemix = infoDemix.chanFreqs();
      itsFreqSubtr = getInfo().chanFreqs();

      // Store phase center position in J2000.
      MDirection dirJ2000(MDirection::Convert(infoIn.phaseCenter(),
                                              MDirection::J2000)());
      Quantum<Vector<Double> > angles = dirJ2000.getAngle();
      itsPhaseRef = Position(angles.getBaseValue()[0],
                             angles.getBaseValue()[1]);

      // Intialize the unknowns.
      itsUnknowns.resize(itsNTimeDemix * itsNModel * itsNStation * 8);
      itsLastKnowns.resize(itsNModel * itsNStation * 8);
      vector<double>::iterator it = itsLastKnowns.begin();
      vector<double>::iterator it_end = itsLastKnowns.end();
      while(it != it_end)
      {
        *it++ = 1.0;
        *it++ = 0.0;
        *it++ = 0.0;
        *it++ = 0.0;
        *it++ = 0.0;
        *it++ = 0.0;
        *it++ = 1.0;
        *it++ = 0.0;
      }
    }

    void Demixer::show (std::ostream& os) const
    {
      os << "Demixer " << itsName << std::endl;
      os << "  skymodel:         " << itsSkyName << std::endl;
      os << "  instrumentmodel:  " << itsInstrumentName << std::endl;
      os << "  targetsource:     " << itsTargetSource << std::endl;
      os << "  subtractsources:  " << itsSubtrSources << std::endl;
      os << "  modelsources:     " << itsModelSources << std::endl;
      os << "  extrasources:     " << itsExtraSources << std::endl;
//      os << "  elevationcutoffs: " << itsCutOffs << std::endl;
//      os << "  jointsolve:     " << itsJointSolve << std::endl;
      os << "  freqstep:         " << itsNChanAvgSubtr << std::endl;
      os << "  timestep:         " << itsNTimeAvgSubtr << std::endl;
      os << "  demixfreqstep:    " << itsNChanAvg << std::endl;
      os << "  demixtimestep:    " << itsNTimeAvg << std::endl;
      os << "  ntimechunk:       " << itsNTimeChunk << std::endl;
//      os << "  Solve.Options.MaxIter:       " << itsSolveOpt.maxIter << endl;
//      os << "  Solve.Options.EpsValue:      " << itsSolveOpt.epsValue << endl;
//      os << "  Solve.Options.EpsDerivative: " << itsSolveOpt.epsDerivative << endl;
//      os << "  Solve.Options.ColFactor:     " << itsSolveOpt.colFactor << endl;
//      os << "  Solve.Options.LMFactor:      " << itsSolveOpt.lmFactor << endl;
//      os << "  Solve.Options.BalancedEqs:   " << itsSolveOpt.balancedEq << endl;
//      os << "  Solve.Options.UseSVD:        " << itsSolveOpt.useSVD <<endl;
    }

    void Demixer::showCounts (std::ostream& os) const
    {
      os << endl << "Statistics for Demixer " << itsName;
      os << endl << "======================" << endl;
      os << endl << "Converged: " << itsNConverged << "/" << itsNTimeDemix
        << " cells" << endl;
    }

    void Demixer::showTimings (std::ostream& os, double duration) const
    {
      const double self = itsTimer.getElapsed();

      os << "  ";
      FlagCounter::showPerc1 (os, self, duration);
      os << " Demixer " << itsName << endl;

      os << "          ";
      FlagCounter::showPerc1 (os, itsTimerPhaseShift.getElapsed(), self);
      os << " of it spent in phase shifting/averaging data" << endl;
      os << "          ";
      FlagCounter::showPerc1 (os, itsTimerDemix.getElapsed(), self);
      os << " of it spent in calculating decorrelation factors" << endl;
      os << "          ";
      FlagCounter::showPerc1 (os, itsTimerSolve.getElapsed(), self);
      os << " of it spent in estimating gains and computing residuals" << endl;
      os << "          ";
      FlagCounter::showPerc1 (os, itsTimerDump.getElapsed(), self);
      os << " of it spent in writing gain solutions to disk" << endl;
    }

    bool Demixer::process (const DPBuffer& buf)
    {
      itsTimer.start();
      // Update the count.
      itsNTimeIn++;
      // Make sure all required data arrays are filled in.
      DPBuffer newBuf(buf);
      RefRows refRows(newBuf.getRowNrs());
      if (newBuf.getUVW().empty()) {
        newBuf.setUVW(itsInput->fetchUVW(newBuf, refRows, itsTimer));
      }
      if (newBuf.getWeights().empty()) {
        newBuf.setWeights(itsInput->fetchWeights(newBuf, refRows, itsTimer));
      }
      if (newBuf.getFullResFlags().empty()) {
        newBuf.setFullResFlags(itsInput->fetchFullResFlags(newBuf, refRows,
                                                           itsTimer));
      }

      // Do the initial steps (phaseshift and average).
      itsTimerPhaseShift.start();
      for (int i=0; i<int(itsFirstSteps.size()); ++i) {
        itsFirstSteps[i]->process(newBuf);
      }
      itsTimerPhaseShift.stop();

      // For each itsNTimeAvg times, calculate the phase rotation per direction.
      itsTimerDemix.start();
      addFactors (newBuf, itsFactorBuf);
      if (itsNTimeIn % itsNTimeAvg == 0) {
        makeFactors (itsFactorBuf, itsFactors[itsNTimeOut],
                     itsAvgResults[0]->get()[itsNTimeOut].getWeights(),
                     itsNChanOut,
                     itsNChanAvg);
        // Deproject sources without a model.
        deproject (itsFactors[itsNTimeOut], itsAvgResults, itsNTimeOut);
        itsFactorBuf = Complex();       // Clear summation buffer
        itsNTimeOut++;
      }
      // Subtract is done with different averaging parameters, so calculate the
      // factors for it.
      addFactors (newBuf, itsFactorBufSubtr);
      if (itsNTimeIn % itsNTimeAvgSubtr == 0) {
        makeFactors (itsFactorBufSubtr, itsFactorsSubtr[itsNTimeOutSubtr],
                     itsAvgResultSubtr->get()[itsNTimeOutSubtr].getWeights(),
                     itsNChanOutSubtr,
                     itsNChanAvgSubtr);
        itsFactorBufSubtr = Complex();  // Clear summation buffer
        itsNTimeOutSubtr++;
      }
      itsTimerDemix.stop();

      // Estimate gains and subtract source contributions when sufficient time
      // slots have been collected.
      if (itsNTimeOut == itsNTimeChunk) {
        if(itsNModel > 0) {
          itsTimerSolve.start();
          demix();
          itsTimerSolve.stop();
        }

        // Clear the input buffers.
        for (size_t i=0; i<itsAvgResults.size(); ++i) {
          itsAvgResults[i]->clear();
        }

        // Let the next step process the data.
        for (uint i=0; i<itsNTimeOutSubtr; ++i) {
          itsTimer.stop();
          getNextStep()->process (itsAvgResultSubtr->get()[i]);
          itsTimer.start();
        }

        // Clear the output buffer.
        itsAvgResultSubtr->clear();

        // Reset counters.
        itsNTimeIn       = 0;
        itsNTimeOut      = 0;
        itsNTimeOutSubtr = 0;
        itsTimeIndex += itsNTimeChunk;
      }

      itsTimer.stop();
      return true;
    }

    void Demixer::finish()
    {
      itsTimer.start();

      // Process remaining entries.
      if (itsNTimeIn > 0) {
        // Finish the initial steps (phaseshift and average).
        itsTimerPhaseShift.start();
        for (int i=0; i<int(itsFirstSteps.size()); ++i) {
          itsFirstSteps[i]->finish();
        }
        itsTimerPhaseShift.stop();
        // Only average if there is some unaveraged data.
        itsTimerDemix.start();
        if (itsNTimeIn % itsNTimeAvg != 0) {
          makeFactors (itsFactorBuf, itsFactors[itsNTimeOut],
                       itsAvgResults[0]->get()[itsNTimeOut].getWeights(),
                       itsNChanOut,
                       itsNChanAvg);
          // Deproject sources without a model.
          deproject (itsFactors[itsNTimeOut], itsAvgResults, itsNTimeOut);
          itsNTimeOut++;
        }
        if (itsNTimeIn % itsNTimeAvgSubtr != 0) {
          makeFactors (itsFactorBufSubtr, itsFactorsSubtr[itsNTimeOutSubtr],
                       itsAvgResultSubtr->get()[itsNTimeOutSubtr].getWeights(),
                       itsNChanOutSubtr,
                       itsNChanAvgSubtr);
          itsNTimeOutSubtr++;
        }
        itsTimerDemix.stop();
        // Resize lists of mixing factors to the number of valid entries.
        itsFactors.resize(itsNTimeOut);
        itsFactorsSubtr.resize(itsNTimeOutSubtr);

        // Demix the source directions.
        if(itsNModel > 0) {
          itsTimerSolve.start();
          demix();
          itsTimerSolve.stop();
        }

        // Clear the input buffers.
        for (size_t i=0; i<itsAvgResults.size(); ++i) {
          itsAvgResults[i]->clear();
        }

        // Let the next step process the data.
        for (uint i=0; i<itsNTimeOutSubtr; ++i) {
          itsTimer.stop();
          getNextStep()->process (itsAvgResultSubtr->get()[i]);
          itsTimer.start();
        }

        // Clear the output buffer.
        itsAvgResultSubtr->clear();
      }

      // Write solutions to disk in ParmDB format.
      itsTimerDump.start();
      dumpSolutions();
      itsTimerDump.stop();

      itsTimer.stop();

      // Let the next steps finish.
      getNextStep()->finish();
    }

    void Demixer::addFactors (const DPBuffer& newBuf,
                              Array<DComplex>& factorBuf)
    {
      // Nothing to do if only target direction.
      if (itsNDir <= 1) return;
      int ncorr  = newBuf.getData().shape()[0];
      int nchan  = newBuf.getData().shape()[1];
      int nbl    = newBuf.getData().shape()[2];
      int ncc    = ncorr*nchan;
      //# If ever in the future a time dependent phase center is used,
      //# the machine must be reset for each new time, thus each new call
      //# to process.
      // Add the weighted factors for each pair of directions.
      // The input factor is the phaseshift from target direction to
      // source direction. By combining them you get the shift from one
      // source direction to another.
      int dirnr = 0;
      for (uint i1=0; i1<itsNDir-1; ++i1) {
        for (uint i0=i1+1; i0<itsNDir; ++i0) {
          if (i0 == itsNDir-1) {
            // The last direction is the target direction, so no need to
            // combine the factors. Take conj to get shift source to target.
#pragma omp parallel for
            for (int i=0; i<nbl; ++i) {
              const bool*   flagPtr   = newBuf.getFlags().data() + i*ncc;
              const float*  weightPtr = newBuf.getWeights().data() + i*ncc;
              DComplex* factorPtr     = factorBuf.data() + (dirnr*nbl + i)*ncc;
              const DComplex* phasor1 = itsPhaseShifts[i1]->getPhasors().data()
                                        + i*nchan;
              for (int j=0; j<nchan; ++j) {
                DComplex factor = conj(*phasor1++);
                for (int k=0; k<ncorr; ++k) {
                  if (! *flagPtr) {
                    *factorPtr += factor * double(*weightPtr);
                  }
                  flagPtr++;
                  weightPtr++;
                  factorPtr++;
                }
              }
            } // end omp parallel for
          } else {
            // Different source directions; take both phase terms into account.
#pragma omp parallel for
            for (int i=0; i<nbl; ++i) {
              const bool*   flagPtr   = newBuf.getFlags().data() + i*ncc;
              const float*  weightPtr = newBuf.getWeights().data() + i*ncc;
              DComplex* factorPtr     = factorBuf.data() + (dirnr*nbl + i)*ncc;
              const DComplex* phasor0 = itsPhaseShifts[i0]->getPhasors().data()
                                        + i*nchan;
              const DComplex* phasor1 = itsPhaseShifts[i1]->getPhasors().data()
                                        + i*nchan;
              for (int j=0; j<nchan; ++j) {
                DComplex factor = *phasor0++ * conj(*phasor1++);
                for (int k=0; k<ncorr; ++k) {
                  if (! *flagPtr) {
                    *factorPtr += factor * double(*weightPtr);
                  }
                  flagPtr++;
                  weightPtr++;
                  factorPtr++;
                }
              }
            } // end omp parallel for
          }

          // Next direction pair.
          dirnr++;
        }
      }
    }

    void Demixer::makeFactors (const Array<DComplex>& bufIn,
                               Array<DComplex>& bufOut,
                               const Cube<float>& weightSums,
                               uint nChanOut,
                               uint nChanAvg)
    {
      // Nothing to do if only target direction.
      if (itsNDir <= 1) return;
      ASSERT (! weightSums.empty());
      bufOut.resize (IPosition(5, itsNDir, itsNDir,
                               itsNCorr, nChanOut, itsNBl));
      bufOut = DComplex(1,0);
      int ncc = itsNCorr*nChanOut;
      int nccdd = ncc*itsNDir*itsNDir;
      int nccin = itsNCorr*itsNChanIn;
      // Fill the factors for each combination of different directions.
      uint dirnr = 0;
      for (uint d0=0; d0<itsNDir; ++d0) {
        for (uint d1=d0+1; d1<itsNDir; ++d1) {
#pragma omp parallel for
          // Average factors by summing channels.
          // Note that summing in time is done in addFactors.
          // The sum per output channel is divided by the summed weight.
          // Note there is a summed weight per baseline,outchan,corr.
          for (int k=0; k<int(itsNBl); ++k) {
            const DComplex* phin = bufIn.data() + (dirnr*itsNBl + k)*nccin;
            DComplex* ph1 = bufOut.data() + k*nccdd + (d0*itsNDir + d1);
            DComplex* ph2 = bufOut.data() + k*nccdd + (d1*itsNDir + d0);
            const float* weightPtr = weightSums.data() + k*ncc;
            for (uint c0=0; c0<nChanOut; ++c0) {
              // Sum the factors for the input channels to average.
              DComplex sum[4];
              // In theory the last output channel could consist of fewer
              // input channels, so take care of that.
              uint nch = std::min(nChanAvg, itsNChanIn-c0*nChanAvg);
              for (uint c1=0; c1<nch; ++c1) {
                for (uint j=0; j<itsNCorr; ++j) {
                  sum[j] += *phin++;
                }
              }
              for (uint j=0; j<itsNCorr; ++j) {
                *ph1 = sum[j] / double(*weightPtr++);
                *ph2 = conj(*ph1);
                ph1 += itsNDir*itsNDir;
                ph2 += itsNDir*itsNDir;
              }
            }
          } // end omp parallel for
          // Next input direction pair.
          dirnr++;
        }
      }
    }

    void Demixer::deproject (Array<DComplex>& factors,
                             vector<MultiResultStep*> avgResults,
                             uint resultIndex)
    {
      // Nothing to do if only target direction or if all sources are modeled.
      if (itsNDir <= 1 || itsNDir == itsNModel) return;
      // Get pointers to the data for the various directions.
      vector<Complex*> resultPtr(itsNDir);
      for (uint j=0; j<itsNDir; ++j) {
        resultPtr[j] = avgResults[j]->get()[resultIndex].getData().data();
      }
      // Sources without a model have to be deprojected.
      uint nrDeproject = itsNDir - itsNModel;
      // The projection matrix is given by
      //     P = I - A * inv(A.T.conj * A) * A.T.conj
      // where A is the last column of the demixing matrix M.
      // The BBS equations get:
      //     P * M' * v_predict = P * v_averaged
      // where M' is obtained by removing the last column of demixing matrix M.
      // The dimensions of the matrices/vectors are:
      //     P : NxN
      //     M' : Nx(N-1)
      //     v_predict : (N-1) x 1
      //     v_averaged: N x 1
      // where N is the number of modeled sources to use in demixing.
      // In the general case S sources might not have a source model.
      // In that case A is the NxS matrix containing all these columns
      // from M and M' is the Nx(N-S) matrix without all these columns.

      // Calculate P for all baselines,channels,correlations.
      IPosition shape = factors.shape();
      int nvis = shape[2] * shape[3] * shape[4];
      shape[1] = itsNModel;
      Array<DComplex> newFactors (shape);
      IPosition inShape (2, itsNDir, itsNDir);
      IPosition outShape(2, itsNDir, itsNModel);
///#pragma omp parallel
      {
        Matrix<DComplex> a(itsNDir, nrDeproject);
        Matrix<DComplex> ma(itsNDir, itsNModel);
        vector<DComplex> vec(itsNDir);
        ///#pragma omp for
        for (int i=0; i<nvis; ++i) {
          // Split the matrix into the modeled and deprojected sources.
          // Copy the columns to the individual matrices.
          const DComplex* inptr  = factors.data() + i*itsNDir*itsNDir;
          DComplex* outptr = newFactors.data() + i*itsNDir*itsNModel;
          Matrix<DComplex> out (outShape, outptr, SHARE);
          // Copying a bit of data is probably faster than taking a matrix
          // subset.
          objcopy (ma.data(), inptr, itsNDir*itsNModel);
          objcopy (a.data(), inptr + itsNDir*itsNModel, itsNDir*nrDeproject);
          // Calculate conjugated transpose of A, multiply with A, and invert.
          Matrix<DComplex> at(adjoint(a));
          Matrix<DComplex> ata(invert(product(at, a)));
          if (ata.empty()) {
            ata.resize (nrDeproject, nrDeproject);
          }
          DBGASSERT(ata.ncolumn()==nrDeproject && ata.nrow()==nrDeproject);
          // Calculate P = I - A * ata * A.T.conj
          Matrix<DComplex> aata(product(a,ata));
          Matrix<DComplex> p (-product(product(a, ata), at));
          Vector<DComplex> diag(p.diagonal());
          diag += DComplex(1,0);
          // Multiply the demixing factors with P (get stored in newFactors).
          out = product(p, ma);
          // Multiply the averaged data point with P.
          std::fill (vec.begin(), vec.end(), DComplex());
          for (uint j=0; j<itsNDir; ++j) {
            for (uint k=0; k<itsNDir; ++k) {
              vec[k] += DComplex(resultPtr[j][i]) * p(k,j);
            }
          }
          // Put result back in averaged data for those sources.
          for (uint j=0; j<itsNDir; ++j) {
            resultPtr[j][i] = vec[j];
          }
        }
      }
      // Set the new demixing factors.
      factors.reference (newFactors);
    }

    namespace {
      struct ThreadPrivateStorage
      {
        vector<double>    unknowns;
        vector<double>    uvw;
        vector<dcomplex>  model;
        vector<dcomplex>  model_subtr;
        size_t            count_converged;
      };

      void initThreadPrivateStorage(ThreadPrivateStorage &storage,
        size_t nDirection, size_t nStation, size_t nBaseline, size_t nChannel,
        size_t nChannelSubtr)
      {
        storage.unknowns.resize(nDirection * nStation * 8);
        storage.uvw.resize(nStation * 3);
        storage.model.resize(nDirection * nBaseline * nChannel * 4);
        storage.model_subtr.resize(nBaseline * nChannelSubtr * 4);
        storage.count_converged = 0;
      }
    } //# end unnamed namespace

    void Demixer::demix()
    {
      const size_t nThread = OpenMP::maxThreads();
      const size_t nTime = itsAvgResults[0]->get().size();
      const size_t nTimeSubtr = itsAvgResultSubtr->get().size();
      const size_t multiplier = itsNTimeAvg / itsNTimeAvgSubtr;
      const size_t nDr = itsNModel;
      const size_t nDrSubtr = itsSubtrSources.size();
      const size_t nSt = itsNStation;
      const size_t nBl = itsBaselines.size();
      const size_t nCh = itsFreqDemix.size();
      const size_t nChSubtr = itsFreqSubtr.size();
      const size_t nCr = 4;
      const size_t nSamples = nBl * nCh * nCr;

      vector<ThreadPrivateStorage> threadStorage(nThread);
      for(vector<ThreadPrivateStorage>::iterator it = threadStorage.begin(),
        end = threadStorage.end(); it != end; ++it)
      {
        initThreadPrivateStorage(*it, nDr, nSt, nBl, nCh, nChSubtr);

        // Copy solutions from global solution array to thread private solution
        // array (solution propagation between chunks).
        copy(itsLastKnowns.begin(), itsLastKnowns.end(), it->unknowns.begin());
      }

      const_cursor<double> cr_freq = casa_const_cursor(itsFreqDemix);
      const_cursor<double> cr_freqSubtr = casa_const_cursor(itsFreqSubtr);
      const_cursor<Baseline> cr_baseline(&(itsBaselines[0]));

#pragma omp parallel for
      for(size_t ts = 0; ts < nTime; ++ts)
      {
        const size_t thread = OpenMP::threadNum();
        ThreadPrivateStorage &storage = threadStorage[thread];

        // Simulate.
        //
        // Model visibilities for each direction of interest will be computed
        // and stored.
        size_t stride_uvw[2] = {1, 3};
        cursor<double> cr_uvw_split(&(storage.uvw[0]), 2, stride_uvw);

        size_t stride_model[3] = {1, nCr, nCr * nCh};
        fill(storage.model.begin(), storage.model.end(), dcomplex());
        for(size_t dr = 0; dr < nDr; ++dr)
        {
          const_cursor<double> cr_uvw =
            casa_const_cursor(itsAvgResults[dr]->get()[ts].getUVW());
          splitUVW(nSt, nBl, cr_baseline, cr_uvw, cr_uvw_split);

          cursor<dcomplex> cr_model(&(storage.model[dr * nSamples]), 3,
            stride_model);
          simulate(itsPatchList[dr]->position(), itsPatchList[dr], nSt,
            nBl, nCh, cr_baseline, cr_freq, cr_uvw_split, cr_model);
        }

        // Estimate Jones matrices.
        //
        // A Jones matrix will be estimated for each pair of station and
        // direction.
        //
        // A single (overdetermined) non-linear set of equations for all
        // stations and directions is solved iteratively. The influence of
        // each direction on each other direction is given by the mixing
        // matrix.
        const_cursor<bool> cr_flag =
          casa_const_cursor(itsAvgResults[0]->get()[ts].getFlags());
        const_cursor<float> cr_weight =
          casa_const_cursor(itsAvgResults[0]->get()[ts].getWeights());
        const_cursor<dcomplex> cr_mix = casa_const_cursor(itsFactors[ts]);

        vector<const_cursor<fcomplex> > cr_data(nDr);
        vector<const_cursor<dcomplex> > cr_model(nDr);
        for(size_t dr = 0; dr < nDr; ++dr)
        {
          cr_data[dr] =
            casa_const_cursor(itsAvgResults[dr]->get()[ts].getData());
          cr_model[dr] =
            const_cursor<dcomplex>(&(storage.model[dr * nSamples]), 3,
            stride_model);
        }

        bool converged = estimate(nDr, nSt, nBl, nCh, cr_baseline, cr_data,
          cr_model, cr_flag, cr_weight, cr_mix, &(storage.unknowns[0]));
        if(converged)
        {
          ++storage.count_converged;
        }

        // Compute the residual.
        //
        // All the so-called "subtract sources" are subtracted from the
        // observed data. The previously estimated Jones matrices, as well as
        // the appropriate mixing weight are applied before subtraction.
        //
        // Note that the resolution of the residual can differ from the
        // resolution at which the Jones matrices were estimated.
        for(size_t ts_subtr = multiplier * ts, ts_subtr_end = min(ts_subtr
          + multiplier, nTimeSubtr); ts_subtr != ts_subtr_end; ++ts_subtr)
        {
          for(size_t dr = 0; dr < nDrSubtr; ++dr)
          {
            // Re-use simulation used for estimating Jones matrices if possible.
            cursor<dcomplex> cr_model_subtr(&(storage.model[dr * nSamples]),
              3, stride_model);

            // Re-simulate if required.
            if(multiplier != 1 || nCh != nChSubtr)
            {
              const_cursor<double> cr_uvw =
                casa_const_cursor(itsAvgResultSubtr->get()[ts_subtr].getUVW());
              splitUVW(nSt, nBl, cr_baseline, cr_uvw, cr_uvw_split);

              // Rotate the UVW coordinates for the target direction to the
              // direction of source to subtract. This is required because at
              // the resolution of the residual the UVW coordinates for
              // directions other than the target are unavailable (unless the
              // resolution of the residual is equal to the resolution at which
              // the Jones matrices were estimated, of course).
              rotateUVW(itsPhaseRef, itsPatchList[dr]->position(), nSt,
                cr_uvw_split);

              // Zero the visibility buffer.
              fill(storage.model_subtr.begin(), storage.model_subtr.end(),
                dcomplex());

              // Simulate visibilities at the resolution of the residual.
              size_t stride_model_subtr[3] = {1, nCr, nCr * nChSubtr};
              cr_model_subtr = cursor<dcomplex>(&(storage.model_subtr[0]), 3,
                stride_model_subtr);
              simulate(itsPatchList[dr]->position(), itsPatchList[dr], nSt, nBl,
                nChSubtr, cr_baseline, cr_freqSubtr, cr_uvw_split,
                cr_model_subtr);
            }

            // Apply Jones matrices.
            size_t stride_unknowns[2] = {1, 8};
            const_cursor<double> cr_unknowns(&(storage.unknowns[dr * nSt * 8]),
              2, stride_unknowns);
            apply(nBl, nChSubtr, cr_baseline, cr_unknowns, cr_model_subtr);

            // Subtract the source contribution from the data.
            cursor<fcomplex> cr_residual =
              casa_cursor(itsAvgResultSubtr->get()[ts_subtr].getData());

            // Construct a cursor to iterate over a slice of the mixing matrix
            // at the resolution of the residual. The "to" and "from" direction
            // are fixed. Since the full mixing matrix is 5-D, the slice is
            // therefore 3-D. Each individual value in the slice quantifies the
            // influence of the source to subtract on the target direction for
            // a particular correlation, channel, and baseline.
            //
            // The target direction is the direction with the highest index by
            // convention, i.e. index itsNDir - 1. The directions to subtract
            // have the lowest indices by convention, i.e. indices
            // [0, nDrSubtr).
            const IPosition &stride_mix_subtr =
              itsFactorsSubtr[ts_subtr].steps();
            size_t stride_mix_subtr_slice[3] = {stride_mix_subtr[2],
              stride_mix_subtr[3], stride_mix_subtr[4]};
            ASSERT(stride_mix_subtr_slice[0] == itsNDir * itsNDir
              && stride_mix_subtr_slice[1] == itsNDir * itsNDir * nCr
              && stride_mix_subtr_slice[2] == itsNDir * itsNDir * nCr * nChSubtr);

            IPosition offset(5, itsNDir - 1, dr, 0, 0, 0);
            const_cursor<dcomplex> cr_mix_subtr =
              const_cursor<dcomplex>(&(itsFactorsSubtr[ts_subtr](offset)), 3,
              stride_mix_subtr_slice);

            // Subtract the source.
            subtract(nBl, nChSubtr, cr_baseline, cr_residual, cr_model_subtr,
              cr_mix_subtr);
          }
        }

        // Copy solutions to global solution array.
        copy(storage.unknowns.begin(), storage.unknowns.end(),
          &(itsUnknowns[(itsTimeIndex + ts) * nDr * nSt * 8]));
      }

      // Store last known solutions.
      if(nTime > 0)
      {
        copy(&(itsUnknowns[(itsTimeIndex + nTime - 1) * nDr * nSt * 8]),
          &(itsUnknowns[(itsTimeIndex + nTime) * nDr * nSt * 8]),
          itsLastKnowns.begin());
      }

      // Update convergence count.
      for(size_t i = 0; i < nThread; ++i)
      {
        itsNConverged += threadStorage[i].count_converged;
      }
    }

    void Demixer::dumpSolutions()
    {
      // Construct solution grid.
      const Vector<double>& freq      = getInfo().chanFreqs();
      const Vector<double>& freqWidth = getInfo().chanWidths();
      BBS::Axis::ShPtr freqAxis(new BBS::RegularAxis(freq[0] - freqWidth[0]
        * 0.5, freqWidth[0], 1));
      BBS::Axis::ShPtr timeAxis(new BBS::RegularAxis(getInfo().startTime()
        - getInfo().timeInterval() * 0.5, itsTimeIntervalAvg, itsNTimeDemix));
      BBS::Grid solGrid(freqAxis, timeAxis);

      // Create and initialize ParmDB.
      BBS::ParmDB parmDB(BBS::ParmDBMeta("casa", itsInstrumentName), true);
      BBS::ParmSet parmSet;
      BBS::ParmCache parmCache(parmSet, solGrid.getBoundingBox());

      // Store the (freq, time) resolution of the solutions.
      vector<double> resolution(2);
      resolution[0] = freqWidth[0];
      resolution[1] = itsTimeIntervalAvg;
      parmDB.setDefaultSteps(resolution);

      // Convert station names from casa::String to std::string.
      ASSERT(getInfo().antennaNames().size() == itsNStation);
      vector<string> stations(itsNStation);
      copy(getInfo().antennaNames().begin(), getInfo().antennaNames().end(),
          stations.begin());

      vector<BBS::Parm> parms;
      for(size_t dr = 0; dr < itsNModel; ++dr) {
        for(size_t st = 0; st < itsNStation; ++st) {
          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:0:0:Real:" + stations[st] + ":"
            + itsAllSources[dr])));
          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:0:0:Imag:" + stations[st] + ":"
            + itsAllSources[dr])));

          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:0:1:Real:" + stations[st] + ":"
            + itsAllSources[dr])));
          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:0:1:Imag:" + stations[st] + ":"
            + itsAllSources[dr])));

          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:1:0:Real:" + stations[st] + ":"
            + itsAllSources[dr])));
          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:1:0:Imag:" + stations[st] + ":"
            + itsAllSources[dr])));

          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:1:1:Real:" + stations[st] + ":"
            + itsAllSources[dr])));
          parms.push_back(BBS::Parm(parmCache, parmSet.addParm(parmDB,
            "DirectionalGain:1:1:Imag:" + stations[st] + ":"
            + itsAllSources[dr])));
        }
      }

      // Cache parameter values.
      parmCache.cacheValues();

      // Assign solution grid to parameters.
      for(size_t i = 0; i < parms.size(); ++i) {
        parms[i].setSolveGrid(solGrid);
      }

      // Write solutions.
      for(size_t ts = 0; ts < itsNTimeDemix; ++ts) {
        double *unknowns = &(itsUnknowns[ts * itsNModel * itsNStation * 8]);
        for(size_t i = 0; i < parms.size(); ++i) {
          parms[i].setCoeff(BBS::Location(0, ts), unknowns + i, 1);
        }
      }

      // Flush solutions to disk.
      parmCache.flush();
    }

    namespace
    {
      string toString (double value)
      {
        ostringstream os;
        os << setprecision(16) << value;
        return os.str();
      }

      double getAngle (const String& value)
      {
        double angle;
        Quantity q;
        ASSERTSTR (Quantity::read (q, value),
                   "Demixer: " + value + " is not a proper angle");
        if (q.getUnit().empty()) {
          angle = q.getValue() / 180. * C::pi;
        } else {
          ASSERTSTR (q.getFullUnit().getValue() == UnitVal::ANGLE,
                     "Demixer: " + value + " is not a proper angle");
          angle = q.getValue("rad");
        }
        return angle;
      }
    } //# end unnamed namespace

  } //# end namespace DPPP
} //# end namespace LOFAR
