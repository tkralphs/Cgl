// Copyright (C) 2005, International Business Machines
// Corporation and others.  All Rights Reserved.
#if defined(_MSC_VER)
// Turn off compiler warning about long names
#  pragma warning(disable:4786)
#endif
#include <string>
#include <cassert>
#include <cmath>
#include <cfloat>

#include "CglPreProcess.hpp"
#include "CglMessage.hpp"
#include "OsiRowCut.hpp"
#include "OsiColCut.hpp"
#include "OsiRowCutDebugger.hpp"
#include "OsiCuts.hpp"
#include "CglCutGenerator.hpp"
#include "CoinTime.hpp"
#include "CoinSort.hpp"
#include "CoinBuild.hpp"
#include "CoinHelperFunctions.hpp"

#include "CglProbing.hpp"
#include "CglDuplicateRow.hpp"

OsiSolverInterface *
CglPreProcess::preProcess(OsiSolverInterface & model, 
                       bool makeEquality, int numberPasses)
{
  // Tell solver we are in Branch and Cut
  model.setHintParam(OsiDoInBranchAndCut,true,OsiHintDo) ;
  // Default set of cut generators
  CglProbing generator1;
  generator1.setUsingObjective(true);
  generator1.setMaxPass(3);
  generator1.setMaxProbe(100);
  generator1.setMaxLook(50);
  generator1.setRowCuts(3);
  // Add in generators
  addCutGenerator(&generator1);
  OsiSolverInterface * newSolver = preProcessNonDefault(model,makeEquality,numberPasses);
  // Tell solver we are in Branch and Cut
  model.setHintParam(OsiDoInBranchAndCut,false,OsiHintDo) ;
  if (newSolver)
    newSolver->setHintParam(OsiDoInBranchAndCut,false,OsiHintDo) ;
  return newSolver;
}
OsiSolverInterface *
CglPreProcess::preProcessNonDefault(OsiSolverInterface & model, 
                       bool makeEquality, int numberPasses)
{
  originalModel_ = & model;
  numberSolvers_ = numberPasses;
  model_ = new OsiSolverInterface * [numberSolvers_];
  modifiedModel_ = new OsiSolverInterface * [numberSolvers_];
  presolve_ = new OsiPresolve * [numberSolvers_];
  for (int i=0;i<numberSolvers_;i++) {
    model_[i]=NULL;
    modifiedModel_[i]=NULL;
    presolve_[i]=NULL;
  }
  // clear original
  delete [] originalColumn_;
  delete [] originalRow_;
  originalColumn_=NULL;
  originalRow_=NULL;
  startModel_=&model;
  CoinPackedMatrix matrixByRow(*originalModel_->getMatrixByRow());
  int numberRows = originalModel_->getNumRows();
  int numberColumns = originalModel_->getNumCols();
  
  // We want to add columns
  int numberSlacks=0;
  int * rows = new int[numberRows];
  double * element =new double[numberRows];
  
  int iRow;
  
  int numberCliques=0;
  int * which = new int[numberColumns];
  
  // Statistics
  int totalP1=0,totalM1=0;
  int numberFixed=0;
  // May just find it is infeasible
  bool feasible=true;
  
  // Row copy
  const double * elementByRow = matrixByRow.getElements();
  const int * column = matrixByRow.getIndices();
  const CoinBigIndex * rowStart = matrixByRow.getVectorStarts();
  const int * rowLength = matrixByRow.getVectorLengths();
  
  const double * lower = originalModel_->getColLower();
  const double * upper = originalModel_->getColUpper();
  const double * rowLower = originalModel_->getRowLower();
  const double * rowUpper = originalModel_->getRowUpper();

  // See if all + 1
  bool allPlusOnes=true;
  for (iRow=0;iRow<numberRows;iRow++) {
    int numberP1=0, numberM1=0;
    int j;
    double upperValue=rowUpper[iRow];
    double lowerValue=rowLower[iRow];
    bool good=true;
    for (j=rowStart[iRow];j<rowStart[iRow]+rowLength[iRow];j++) {
      int iColumn = column[j];
      if (upper[iColumn]-lower[iColumn]<1.0e-8) {
        // fixed
        upperValue -= lower[iColumn]*elementByRow[j];
        lowerValue -= lower[iColumn]*elementByRow[j];
        continue;
      } else if (!originalModel_->isBinary(iColumn)) {
        good = false;
        break;
      }
      if (fabs(elementByRow[j])!=1.0) {
        good=false;
        allPlusOnes=false;
        break;
      } else if (elementByRow[j]>0.0) {
        which[numberP1++]=iColumn;
      } else {
        allPlusOnes=false;
        numberM1++;
        which[numberColumns-numberM1]=iColumn;
      }
    }
    int iUpper = (int) floor(upperValue+1.0e-5);
    int iLower = (int) ceil(lowerValue-1.0e-5);
    int state=0;
    if (upperValue<1.0e6) {
      if (iUpper==1-numberM1)
        state=1;
      else if (iUpper==-numberM1)
        state=2;
      else if (iUpper<-numberM1)
        state=3;
    }
    if (!state&&lowerValue>-1.0e6) {
      if (-iLower==1-numberP1)
        state=-1;
      else if (-iLower==-numberP1)
        state=-2;
      else if (-iLower<-numberP1)
        state=-3;
    }
    if (good&&state) {
      if (abs(state)==3) {
        // infeasible
        feasible=false;
        break;
      } else if (abs(state)==2) {
        // we can fix all
        numberFixed += numberP1+numberM1;
        int i;
        if (state>0) {
          // fix all +1 at 0, -1 at 1
          for (i=0;i<numberP1;i++)
            originalModel_->setColUpper(which[i],0.0);
          for (i=0;i<numberM1;i++)
            originalModel_->setColLower(which[numberColumns-i-1],1.0);
        } else {
          // fix all +1 at 1, -1 at 0
          for (i=0;i<numberP1;i++)
            originalModel_->setColLower(which[i],1.0);
          for (i=0;i<numberM1;i++)
            originalModel_->setColUpper(which[numberColumns-i-1],0.0);
        }
      } else {
        numberCliques++;
        if (iLower!=iUpper) {
          element[numberSlacks]=state;
          rows[numberSlacks++]=iRow;
        }
        if (state>0) {
          totalP1 += numberP1;
          totalM1 += numberM1;
        } else {
          totalP1 += numberM1;
          totalM1 += numberP1;
        }
      }
    }
  }
  delete [] which;
  if (!feasible) {
    handler_->message(CGL_INFEASIBLE,messages_)
      <<CoinMessageEol;
    delete [] rows;
    delete [] element;
    return NULL;
  } else {
    if (numberCliques) {
      handler_->message(CGL_CLIQUES,messages_)
        <<numberCliques
        << ((double)(totalP1+totalM1))/((double) numberCliques)
        <<CoinMessageEol;
      //printf("%d of these could be converted to equality constraints\n",
      //     numberSlacks);
    }
    if (numberFixed)
      handler_->message(CGL_FIXED,messages_)
        <<numberFixed
        <<CoinMessageEol;
  }
  if (numberSlacks&&makeEquality) {
    // add variables to make equality rows
    // Get new model
    startModel_ = originalModel_->clone();
    for (int i=0;i<numberSlacks;i++) {
      int iRow = rows[i];
      double value = element[i];
      double lowerValue = 0.0;
      double upperValue = 1.0;
      double objValue  = 0.0;
      CoinPackedVector column(1,&iRow,&value);
      startModel_->addCol(column,lowerValue,upperValue,objValue);
      // set integer
      startModel_->setInteger(numberColumns+i);
      if (value >0)
	startModel_->setRowLower(iRow,rowUpper[iRow]);
      else
	startModel_->setRowUpper(iRow,rowLower[iRow]);
    }
  }
  delete [] rows;
  delete [] element;
   
  OsiSolverInterface * returnModel=NULL;
  int numberChanges;
  if (!numberSolvers_) {
    // just fix
    startModel_->initialSolve();
    if (!startModel_->isProvenOptimal()) {
      handler_->message(CGL_INFEASIBLE,messages_)
        <<CoinMessageEol;
      return NULL;
    }
    OsiSolverInterface * newModel = modified(startModel_,false,numberChanges);
    if (startModel_!=originalModel_)
      delete startModel_;
    startModel_=newModel;
    returnModel=startModel_;
  } else {
    OsiSolverInterface * presolvedModel;
    OsiSolverInterface * oldModel = startModel_;
    CglDuplicateRow dupCuts(startModel_);
    //dupCuts.setLogLevel(1);
    // If +1 try duplicate rows
    if (allPlusOnes)
      addCutGenerator(&dupCuts);
    for (int iPass=0;iPass<numberSolvers_;iPass++) {
      // Look at Vubs
      {
        const double * columnLower = oldModel->getColLower();
        const double * columnUpper = oldModel->getColUpper();
        const CoinPackedMatrix * rowCopy = oldModel->getMatrixByRow();
        const int * column = rowCopy->getIndices();
        const CoinBigIndex * rowStart = rowCopy->getVectorStarts();
        const int * rowLength = rowCopy->getVectorLengths(); 
        const double * rowElements = rowCopy->getElements();
        const CoinPackedMatrix * columnCopy = oldModel->getMatrixByCol();
        //const int * row = columnCopy->getIndices();
        //const CoinBigIndex * columnStart = columnCopy->getVectorStarts();
        const int * columnLength = columnCopy->getVectorLengths(); 
        //const double * columnElements = columnCopy->getElements();
        const double * rowLower = oldModel->getRowLower();
        const double * rowUpper = oldModel->getRowUpper();
        const double * objective = oldModel->getObjCoefficients();
        double direction = oldModel->getObjSense();
        int numberRows = oldModel->getNumRows();
        for (int iRow=0;iRow<numberRows;iRow++) {
          if (rowLength[iRow]==2&&(rowLower[iRow]<-1.0e20||rowUpper[iRow]>1.0e20)) {
            CoinBigIndex start = rowStart[iRow];
            int iColumn1 = column[start];
            int iColumn2 = column[start+1];
            double value1 = rowElements[start];
            double value2 = rowElements[start+1];
            double upper;
            if (rowLower[iRow]<-1.0e20) {
              if (rowUpper[iRow]<1.0e20)
                upper = rowUpper[iRow];
              else
                continue; // free row
            } else {
              upper = - rowLower[iRow];
              value1=-value1;
              value2=-value2;
            }
            //for now just singletons
            bool integer1 = oldModel->isInteger(iColumn1);
            bool integer2 = oldModel->isInteger(iColumn2);
            int debug=0;
            if (columnLength[iColumn1]==1) {
              if (integer1) {
                debug=0;// no good
              } else if (integer2) {
                // possible
                debug=1;
              }
            } else if (columnLength[iColumn2]==1) {
              if (integer2) {
                debug=-1; // print and skip
              } else if (integer1) {
                // possible
                debug=1;
                double valueD = value1;
                value1 = value2;
                value2 = valueD;
                int valueI = iColumn1;
                iColumn1 = iColumn2;
                iColumn2 = valueI;
                bool valueB = integer1;
                integer1 = integer2;
                integer2 = valueB;
              }
            }
            if (debug&&0) {
              printf("%d %d elements%selement %g and %d %d elements%selement %g <= %g\n",
                     iColumn1,columnLength[iColumn1],integer1 ? " (integer) " : " ",value1,
                     iColumn2,columnLength[iColumn2],integer2 ? " (integer) " : " ",value2,
                     upper);
            }
            if (debug>0) {
              if (value1>0.0&&objective[iColumn1]*direction<0.0) {
                // will push as high as possible
                // highest effective rhs
                if (value2>0) 
                  upper -= value2 * columnLower[iColumn2];
                else
                  upper -= value2 * columnUpper[iColumn2];
                if (columnUpper[iColumn1]>1.0e20||
                    columnUpper[iColumn1]*value1>=upper) {
                  //printf("looks possible\n");
                  if (rowLower[iRow]<-1.0e20) 
                    oldModel->setRowLower(iRow,upper);
                  else
                    oldModel->setRowLower(iRow,-upper);
                } else {
                  // may be able to make integer
                  // may just be better to use to see objective integral
                  if (upper==floor(upper)&&value2==floor(value2)&&
                      value1==floor(value1)&&objective[iColumn1]==floor(objective[iColumn1]))
                    oldModel->setInteger(iColumn1);
                  //printf("odd3\n");
                }
              } else if (value1<0.0&&objective[iColumn1]*direction>0.0) {
                //printf("odd4\n");
              } else {
                //printf("odd2\n");
              }
            } else if (debug<0) {
              //printf("odd1\n");
            }
          }
        }
      }
      OsiPresolve * pinfo = new OsiPresolve();
      int presolveActions=0;
      // Allow dual stuff on integers
      presolveActions=1;
      // Do not allow all +1 to be tampered with
      if (allPlusOnes)
        presolveActions |= 2;
      pinfo->setPresolveActions(presolveActions);
      presolvedModel = pinfo->presolvedModel(*oldModel,1.0e-8,true,5);
      if (!presolvedModel) {
        returnModel=NULL;
        break;
      }
      //char name[20];
      //sprintf(name,"prex%2.2d.mps",iPass);
      //presolvedModel->writeMpsNative(name, NULL, NULL,0,1,0);
      model_[iPass]=presolvedModel;
      presolve_[iPass]=pinfo;
      if (!presolvedModel->getNumRows()) {
        returnModel=oldModel;
        numberSolvers_=iPass+1;
        break; // model totally solved
      }
      bool constraints = iPass<numberPasses-1;
      presolvedModel->initialSolve();
      if (!presolvedModel->isProvenOptimal()) {
        returnModel=NULL;
        break;
      }
      OsiSolverInterface * newModel = modified(presolvedModel,constraints,numberChanges);
      returnModel=newModel;
      if (!newModel) {
        break;
      }
      modifiedModel_[iPass]=newModel;
      oldModel=newModel;
      //sprintf(name,"pre%2.2d.mps",iPass);
      //newModel->writeMpsNative(name, NULL, NULL,0,1,0);
      if (!numberChanges) {
        numberSolvers_=iPass+1;
        break;
      }
    }
  }
  if (returnModel) {
    if (returnModel->getNumRows()) {
      // tighten bounds
      int infeas = tightenPrimalBounds(*returnModel);
      if (infeas) {
        delete returnModel;
        returnModel=NULL;
      }
    }
  } else {
    handler_->message(CGL_INFEASIBLE,messages_)
      <<CoinMessageEol;
  }
  return returnModel;
}

/* Tightens primal bounds to make dual and branch and cutfaster.  Unless
   fixed, bounds are slightly looser than they could be.
   Returns non-zero if problem infeasible
   Fudge for branch and bound - put bounds on columns of factor *
   largest value (at continuous) - should improve stability
   in branch and bound on infeasible branches (0.0 is off)
*/
int 
CglPreProcess::tightenPrimalBounds(OsiSolverInterface & model,double factor)
{
  
  // Get a row copy in standard format
  CoinPackedMatrix copy = *model.getMatrixByRow();
  // get matrix data pointers
  const int * column = copy.getIndices();
  const CoinBigIndex * rowStart = copy.getVectorStarts();
  const int * rowLength = copy.getVectorLengths(); 
  const double * element = copy.getElements();
  int numberChanged=1,iPass=0;
  double large = model.getInfinity()*0.1; // treat bounds > this as infinite
  int numberInfeasible=0;
  int totalTightened = 0;

  double tolerance;
  model.getDblParam(OsiPrimalTolerance,tolerance);


  int numberColumns=model.getNumCols();
  const double * colLower = model.getColLower();
  const double * colUpper = model.getColUpper();
  // New and saved column bounds
  double * newLower = new double [numberColumns];
  memcpy(newLower,colLower,numberColumns*sizeof(double));
  double * newUpper = new double [numberColumns];
  memcpy(newUpper,colUpper,numberColumns*sizeof(double));
  double * columnLower = new double [numberColumns];
  memcpy(columnLower,colLower,numberColumns*sizeof(double));
  double * columnUpper = new double [numberColumns];
  memcpy(columnUpper,colUpper,numberColumns*sizeof(double));
  const double * solution = model.getColSolution();

  int iRow, iColumn;

  // If wanted - tighten column bounds using solution
  if (factor) {
    double largest=0.0;
    if (factor>0.0) {
      assert (factor>1.0);
      for (iColumn=0;iColumn<numberColumns;iColumn++) {
        if (columnUpper[iColumn]-columnLower[iColumn]>tolerance) {
          largest = CoinMax(largest,fabs(solution[iColumn]));
        }
      }
      largest *= factor;
    } else {
      // absolute
       largest = - factor;
    }
    for (iColumn=0;iColumn<numberColumns;iColumn++) {
      if (columnUpper[iColumn]-columnLower[iColumn]>tolerance) {
        newUpper[iColumn] = CoinMin(columnUpper[iColumn],largest);
        newLower[iColumn] = CoinMax(columnLower[iColumn],-largest);
      }
    }
  }
  int numberRows = model.getNumRows();
  const double * rowLower = model.getRowLower();
  const double * rowUpper = model.getRowUpper();
#ifndef NDEBUG
  double large2= 1.0e10*large;
#endif
#define MAXPASS 10

  // Loop round seeing if we can tighten bounds
  // Would be faster to have a stack of possible rows
  // and we put altered rows back on stack
  int numberCheck=-1;
  while(numberChanged>numberCheck) {

    numberChanged = 0; // Bounds tightened this pass
    
    if (iPass==MAXPASS) break;
    iPass++;
    
    for (iRow = 0; iRow < numberRows; iRow++) {

      if (rowLower[iRow]>-large||rowUpper[iRow]<large) {

	// possible row
	int infiniteUpper = 0;
	int infiniteLower = 0;
	double maximumUp = 0.0;
	double maximumDown = 0.0;
	double newBound;
	CoinBigIndex rStart = rowStart[iRow];
	CoinBigIndex rEnd = rowStart[iRow]+rowLength[iRow];
	CoinBigIndex j;
	// Compute possible lower and upper ranges
      
	for (j = rStart; j < rEnd; ++j) {
	  double value=element[j];
	  iColumn = column[j];
	  if (value > 0.0) {
	    if (newUpper[iColumn] >= large) {
	      ++infiniteUpper;
	    } else {
	      maximumUp += newUpper[iColumn] * value;
	    }
	    if (newLower[iColumn] <= -large) {
	      ++infiniteLower;
	    } else {
	      maximumDown += newLower[iColumn] * value;
	    }
	  } else if (value<0.0) {
	    if (newUpper[iColumn] >= large) {
	      ++infiniteLower;
	    } else {
	      maximumDown += newUpper[iColumn] * value;
	    }
	    if (newLower[iColumn] <= -large) {
	      ++infiniteUpper;
	    } else {
	      maximumUp += newLower[iColumn] * value;
	    }
	  }
	}
	// Build in a margin of error
	maximumUp += 1.0e-8*fabs(maximumUp);
	maximumDown -= 1.0e-8*fabs(maximumDown);
	double maxUp = maximumUp+infiniteUpper*1.0e31;
	double maxDown = maximumDown-infiniteLower*1.0e31;
	if (maxUp <= rowUpper[iRow] + tolerance && 
	    maxDown >= rowLower[iRow] - tolerance) {
	  
	  // Row is redundant - make totally free
	} else {
	  if (maxUp < rowLower[iRow] -100.0*tolerance ||
	      maxDown > rowUpper[iRow]+100.0*tolerance) {
	    // problem is infeasible - exit at once
	    numberInfeasible++;
	    break;
	  }
	  double lower = rowLower[iRow];
	  double upper = rowUpper[iRow];
	  for (j = rStart; j < rEnd; ++j) {
	    double value=element[j];
	    iColumn = column[j];
	    double nowLower = newLower[iColumn];
	    double nowUpper = newUpper[iColumn];
	    if (value > 0.0) {
	      // positive value
	      if (lower>-large) {
		if (!infiniteUpper) {
		  assert(nowUpper < large2);
		  newBound = nowUpper + 
		    (lower - maximumUp) / value;
		  // relax if original was large
		  if (fabs(maximumUp)>1.0e8)
		    newBound -= 1.0e-12*fabs(maximumUp);
		} else if (infiniteUpper==1&&nowUpper>large) {
		  newBound = (lower -maximumUp) / value;
		  // relax if original was large
		  if (fabs(maximumUp)>1.0e8)
		    newBound -= 1.0e-12*fabs(maximumUp);
		} else {
		  newBound = -COIN_DBL_MAX;
		}
		if (newBound > nowLower + 1.0e-12&&newBound>-large) {
		  // Tighten the lower bound 
		  newLower[iColumn] = newBound;
		  numberChanged++;
		  // check infeasible (relaxed)
		  if (nowUpper - newBound < 
		      -100.0*tolerance) {
		    numberInfeasible++;
		  }
		  // adjust
		  double now;
		  if (nowLower<-large) {
		    now=0.0;
		    infiniteLower--;
		  } else {
		    now = nowLower;
		  }
		  maximumDown += (newBound-now) * value;
		  nowLower = newBound;
		}
	      } 
	      if (upper <large) {
		if (!infiniteLower) {
		  assert(nowLower >- large2);
		  newBound = nowLower + 
		    (upper - maximumDown) / value;
		  // relax if original was large
		  if (fabs(maximumDown)>1.0e8)
		    newBound += 1.0e-12*fabs(maximumDown);
		} else if (infiniteLower==1&&nowLower<-large) {
		  newBound =   (upper - maximumDown) / value;
		  // relax if original was large
		  if (fabs(maximumDown)>1.0e8)
		    newBound += 1.0e-12*fabs(maximumDown);
		} else {
		  newBound = COIN_DBL_MAX;
		}
		if (newBound < nowUpper - 1.0e-12&&newBound<large) {
		  // Tighten the upper bound 
		  newUpper[iColumn] = newBound;
		  numberChanged++;
		  // check infeasible (relaxed)
		  if (newBound - nowLower < 
		      -100.0*tolerance) {
		    numberInfeasible++;
		  }
		  // adjust 
		  double now;
		  if (nowUpper>large) {
		    now=0.0;
		    infiniteUpper--;
		  } else {
		    now = nowUpper;
		  }
		  maximumUp += (newBound-now) * value;
		  nowUpper = newBound;
		}
	      }
	    } else {
	      // negative value
	      if (lower>-large) {
		if (!infiniteUpper) {
		  assert(nowLower < large2);
		  newBound = nowLower + 
		    (lower - maximumUp) / value;
		  // relax if original was large
		  if (fabs(maximumUp)>1.0e8)
		    newBound += 1.0e-12*fabs(maximumUp);
		} else if (infiniteUpper==1&&nowLower<-large) {
		  newBound = (lower -maximumUp) / value;
		  // relax if original was large
		  if (fabs(maximumUp)>1.0e8)
		    newBound += 1.0e-12*fabs(maximumUp);
		} else {
		  newBound = COIN_DBL_MAX;
		}
		if (newBound < nowUpper - 1.0e-12&&newBound<large) {
		  // Tighten the upper bound 
		  newUpper[iColumn] = newBound;
		  numberChanged++;
		  // check infeasible (relaxed)
		  if (newBound - nowLower < 
		      -100.0*tolerance) {
		    numberInfeasible++;
		  }
		  // adjust
		  double now;
		  if (nowUpper>large) {
		    now=0.0;
		    infiniteLower--;
		  } else {
		    now = nowUpper;
		  }
		  maximumDown += (newBound-now) * value;
		  nowUpper = newBound;
		}
	      }
	      if (upper <large) {
		if (!infiniteLower) {
		  assert(nowUpper < large2);
		  newBound = nowUpper + 
		    (upper - maximumDown) / value;
		  // relax if original was large
		  if (fabs(maximumDown)>1.0e8)
		    newBound -= 1.0e-12*fabs(maximumDown);
		} else if (infiniteLower==1&&nowUpper>large) {
		  newBound =   (upper - maximumDown) / value;
		  // relax if original was large
		  if (fabs(maximumDown)>1.0e8)
		    newBound -= 1.0e-12*fabs(maximumDown);
		} else {
		  newBound = -COIN_DBL_MAX;
		}
		if (newBound > nowLower + 1.0e-12&&newBound>-large) {
		  // Tighten the lower bound 
		  newLower[iColumn] = newBound;
		  numberChanged++;
		  // check infeasible (relaxed)
		  if (nowUpper - newBound < 
		      -100.0*tolerance) {
		    numberInfeasible++;
		  }
		  // adjust
		  double now;
		  if (nowLower<-large) {
		    now=0.0;
		    infiniteUpper--;
		  } else {
		    now = nowLower;
		  }
		  maximumUp += (newBound-now) * value;
		  nowLower = newBound;
		}
	      }
	    }
	  }
	}
      }
    }
    totalTightened += numberChanged;
    if (iPass==1)
      numberCheck=numberChanged>>4;
    if (numberInfeasible) break;
  }
  if (!numberInfeasible) {
    // Set bounds slightly loose unless integral
    double useTolerance = 1.0e-2;
    for (iColumn=0;iColumn<numberColumns;iColumn++) {
      if (columnUpper[iColumn]>columnLower[iColumn]) {
        double lower = newLower[iColumn];
        double upper = newUpper[iColumn];
        if (model.isInteger(iColumn)) {
          if (fabs(lower-floor(lower+0.5))<1.0e-5)
            lower=floor(lower+0.5);
          else
            lower = ceil(lower);
          if (fabs(upper-floor(upper+0.5))<1.0e-5)
            upper=floor(upper+0.5);
          else
            upper = floor(upper);
          if (lower>upper)
            numberInfeasible++;
        } else {
          if (upper-lower<1.0e-8) {
            if (fabs(lower)<1.0e-8&&fabs(upper)<1.0e-8) 
              lower=0.0;
            upper=lower;
          } else {
            // Relax unless integral
            if (fabs(lower-floor(lower+0.5))>1.0e-9)
              lower -= useTolerance;
            else
              lower = floor(lower+0.5);
            lower=CoinMax(columnLower[iColumn],lower);
            if (fabs(upper-floor(upper+0.5))>1.0e-9)
              upper += useTolerance;
            else
              upper = floor(upper+0.5);
            upper=CoinMin(columnUpper[iColumn],upper);
          }
	}
        model.setColLower(iColumn,lower);
        model.setColUpper(iColumn,upper);
      }
    }
  }
  if (numberInfeasible) {
    handler_->message(CGL_INFEASIBLE,messages_)
      <<CoinMessageEol;
    // restore column bounds
    for (iColumn=0;iColumn<numberColumns;iColumn++) {
      model.setColLower(iColumn,columnLower[iColumn]);
      model.setColUpper(iColumn,columnUpper[iColumn]);
    }
  }
  delete [] newLower;
  delete [] newUpper;
  delete [] columnLower;
  delete [] columnUpper;
  return (numberInfeasible);
}

void
CglPreProcess::postProcess(OsiSolverInterface & modelIn)
{
  // Do presolves
  OsiHintStrength saveStrength;
  bool saveHint;
  originalModel_->getHintParam(OsiDoPresolveInResolve,saveHint,saveStrength);
  OsiSolverInterface * modelM = &modelIn;
  for (int iPass=numberSolvers_-1;iPass>=0;iPass--) {
    OsiSolverInterface * model = model_[iPass];
    if (model->getNumCols()) {
      int numberColumns = modelM->getNumCols();
      const double * solutionM = modelM->getColSolution();
      int iColumn;
      for (iColumn=0;iColumn<numberColumns;iColumn++) {
        if (modelM->isInteger(iColumn)) {
          double value = solutionM[iColumn];
          double value2 = floor(value+0.5);
          assert (fabs(value-value2)<1.0e-3);
          model->setColLower(iColumn,value2);
          model->setColUpper(iColumn,value2);
        }
      }
    }
    model->setHintParam(OsiDoPresolveInResolve,true,OsiHintTry);
    model->resolve();
    presolve_[iPass]->postsolve(true);
    delete modifiedModel_[iPass];;
    delete model_[iPass];;
    delete presolve_[iPass];
    modifiedModel_[iPass]=NULL;
    model_[iPass]=NULL;
    presolve_[iPass]=NULL;
    if (iPass)
      modelM = modifiedModel_[iPass-1];
    else
      modelM = startModel_;
  }
  // should be back to startModel_;
  OsiSolverInterface * model = originalModel_;
  // Use number of columns in original
  int numberColumns = model->getNumCols();
  const double * solutionM = modelM->getColSolution();
  int iColumn;
  for (iColumn=0;iColumn<numberColumns;iColumn++) {
    if (modelM->isInteger(iColumn)) {
      double value = solutionM[iColumn];
      double value2 = floor(value+0.5);
      assert (fabs(value-value2)<1.0e-3);
      model->setColLower(iColumn,value2);
      model->setColUpper(iColumn,value2);
    }
  }
  model->setHintParam(OsiDoPresolveInResolve,true,OsiHintTry);
  model->resolve();
  model->setHintParam(OsiDoPresolveInResolve,saveHint,saveStrength);
}
/* Return model with useful modifications.  
   If constraints true then adds any x+y=1 or x-y=0 constraints
   If NULL infeasible
*/
OsiSolverInterface * 
CglPreProcess::modified(OsiSolverInterface * model,
                     bool constraints,
                     int & numberChanges)
{
  OsiSolverInterface * newModel = model->clone();
  OsiCuts twoCuts;
  int numberRows = newModel->getNumRows();
  int numberColumns = newModel->getNumCols();
  int number01Integers=0;
  int iColumn;
  for (iColumn=0;iColumn<numberColumns;iColumn++) {
    if (newModel->isBinary(iColumn))
      number01Integers++;
  }
  OsiRowCut ** whichCut = new OsiRowCut * [numberRows];
  numberChanges=0;
  CglTreeInfo info;
  info.level = 0;
  info.pass = 0;
  info.formulation_rows = numberRows;
  info.inTree = false;
  info.strengthenRow= whichCut;
  bool feasible=true;
  int firstGenerator=0;
  int lastGenerator=numberCutGenerators_;
  int numberPasses=10;
  for (int iPass=0;iPass<numberPasses;iPass++) {
    // Statistics
    int numberFixed=0;
    int numberTwo=0;
    int numberStrengthened=0;
    info.pass = iPass;
    int numberChangedThisPass=0;
    int numberFromCglDuplicate=0;
    const int * duplicate=NULL;
    for (int iGenerator=firstGenerator;iGenerator<lastGenerator;iGenerator++) {
      OsiCuts cs;
      CoinZeroN(whichCut,numberRows);
      if (iGenerator>=0) {
        //char name[20];
        //sprintf(name,"prex%2.2d.mps",iGenerator);
        //newModel->writeMpsNative(name, NULL, NULL,0,1,0);
        // refresh as model may have changed
        generator_[iGenerator]->refreshSolver(newModel);
        generator_[iGenerator]->generateCuts(*newModel,cs,info);
        // If CglDuplicate may give us useless rows
        CglDuplicateRow * dupRow = dynamic_cast<CglDuplicateRow *> (generator_[iGenerator]);
        if (dupRow) {
          numberFromCglDuplicate = dupRow->numberOriginalRows();
          duplicate = dupRow->duplicate();
        }
      } else {
        // special probing
        CglProbing generator1;
        generator1.setUsingObjective(false);
        generator1.setMaxPass(3);
        generator1.setMaxProbe(100);
        generator1.setMaxLook(100);
        generator1.setRowCuts(3);
        if(!generator1.snapshot(*newModel,NULL,false)) {
          generator1.createCliques(*newModel,2,1000,true);
          generator1.setMode(0);
          // To get special stuff
          info.pass=4;
          CoinZeroN(whichCut,numberRows);
          generator1.generateCutsAndModify(*newModel,cs,info);
        } else {
          feasible=false;
        }
      }
      // check changes
      // first are any rows strengthened by cuts
      int iRow;
      for (iRow=0;iRow<numberRows;iRow++) {
        if(whichCut[iRow])
          numberStrengthened++;
      }
      // Also can we get rid of duplicate rows
      int numberDrop=0;
      for (iRow=0;iRow<numberFromCglDuplicate;iRow++) {
        if (duplicate[iRow]==-2||duplicate[iRow]>=0) {
          numberDrop++;
          newModel->setRowBounds(iRow,-COIN_DBL_MAX,COIN_DBL_MAX);
        }
      }
      const double * columnLower = newModel->getColLower();
      const double * columnUpper = newModel->getColUpper();
      if ((numberStrengthened||numberDrop)&&feasible) {
        // Easier to recreate entire matrix
        const CoinPackedMatrix * rowCopy = newModel->getMatrixByRow();
        const int * column = rowCopy->getIndices();
        const CoinBigIndex * rowStart = rowCopy->getVectorStarts();
        const int * rowLength = rowCopy->getVectorLengths(); 
        const double * rowElements = rowCopy->getElements();
        const double * rowLower = newModel->getRowLower();
        const double * rowUpper = newModel->getRowUpper();
        CoinBuild build;
        for (iRow=0;iRow<numberRows;iRow++) {
          OsiRowCut * thisCut = whichCut[iRow];
          whichCut[iRow]=NULL;
          if (rowLower[iRow]>-1.0e20||rowUpper[iRow]<1.0e20) {
            if (!thisCut) {
              // put in old row
              int start=rowStart[iRow];
              build.addRow(rowLength[iRow],column+start,rowElements+start,
                           rowLower[iRow],rowUpper[iRow]);
            } else {
              // strengthens this row (should we check?)
              // may be worth going round again
              numberChangedThisPass++;
              int n=thisCut->row().getNumElements();
              const int * columnCut = thisCut->row().getIndices();
              const double * elementCut = thisCut->row().getElements();
              double lower = thisCut->lb();
              double upper = thisCut->ub();
              if (iGenerator==1) {
                int i;
                int n1=rowLength[iRow];
                int start=rowStart[iRow];
                int nFree=0;
                for ( i=0;i<n1;i++) {
                  int iColumn = column[start+i];
                  if (columnUpper[iColumn]>columnLower[iColumn]+1.0e-12)
                    nFree++;
                }
                if (n==nFree) {
                  //printf("Original row %d %g <= ",iRow,rowLower[iRow]);
                  //for ( i=0;i<n1;i++) 
                  //printf("%g * x%d ",rowElements[start+i],column[start+i]);
                  //printf("<= %g\n",rowUpper[iRow]);
                  //printf("New %g <= ",lower);
                  //for ( i=0;i<n;i++) 
                  //printf("%g * x%d ",elementCut[i],columnCut[i]);
                  //printf("<= %g\n",upper);
                } else {
                  // can't use
                  n=-1;
                  // put in old row
                  int start=rowStart[iRow];
                  build.addRow(rowLength[iRow],column+start,rowElements+start,
                               rowLower[iRow],rowUpper[iRow]);
                }
              }
              if (n>0) {
                build.addRow(n,columnCut,elementCut,lower,upper);
              } else if (!n) {
                // Either infeasible or redundant
                if (lower<=0.0&&upper>=0.0) {
                  // redundant - row will go
                } else {
                  // infeasible!
                  feasible=false;
                  break;
                }
              }
            }
          }
        }
        // recreate
        int * del = new int[numberRows];
        for (iRow=0;iRow<numberRows;iRow++) 
          del[iRow]=iRow;
        newModel->deleteRows(numberRows,del);
        newModel->addRows(build);
        numberRows = newModel->getNumRows();
        delete [] del;
      }
      if (!feasible)
        break;
      
      // now see if we have any x=y x+y=1
      if (constraints) {
        int numberRowCuts = cs.sizeRowCuts() ;
        for (int k = 0;k<numberRowCuts;k++) {
          OsiRowCut * thisCut = cs.rowCutPtr(k) ;
          int n=thisCut->row().getNumElements();
#ifndef NDEBUG
          const double * elementCut = thisCut->row().getElements();
#endif
          double lower = thisCut->lb();
          double upper = thisCut->ub();
          if (n==2&&lower==upper) {
            numberTwo++;
#ifndef NDEBUG
            if (lower) {
              assert (elementCut[0]==1.0&&elementCut[1]==1.0);
            } else {
              assert ((elementCut[0]==-1.0&&elementCut[1]==1.0)||
                      (elementCut[0]==1.0&&elementCut[1]==-1.0));
            }
#endif
            twoCuts.insert(*thisCut);
          }
        }
      }
      // see if we have any column cuts
      int numberColumnCuts = cs.sizeColCuts() ;
      int numberBounds=0;
      for (int k = 0;k<numberColumnCuts;k++) {
        OsiColCut * thisCut = cs.colCutPtr(k) ;
	const CoinPackedVector & lbs = thisCut->lbs() ;
	const CoinPackedVector & ubs = thisCut->ubs() ;
	int j ;
	int n ;
	const int * which ;
	const double * values ;
	n = lbs.getNumElements() ;
	which = lbs.getIndices() ;
	values = lbs.getElements() ;
	for (j = 0;j<n;j++) {
	  int iColumn = which[j] ;
          if (values[j]>columnLower[iColumn]) {
            //printf("%d lower from %g to %g\n",iColumn,columnLower[iColumn],values[j]);
            newModel->setColLower(iColumn,values[j]) ;
            if (false) {
              OsiSolverInterface * xx = newModel->clone();
              xx->initialSolve();
              assert (xx->isProvenOptimal());
              delete xx;
            }
            numberChangedThisPass++;
            if (columnLower[iColumn]==columnUpper[iColumn])
              numberFixed++;
            else
              numberBounds++;
          }
	}
	n = ubs.getNumElements() ;
	which = ubs.getIndices() ;
	values = ubs.getElements() ;
	for (j = 0;j<n;j++) {
	  int iColumn = which[j] ;
          if (values[j]<columnUpper[iColumn]) {
            //printf("%d upper from %g to %g\n",iColumn,columnUpper[iColumn],values[j]);
            newModel->setColUpper(iColumn,values[j]) ;
            if (false) {
              OsiSolverInterface * xx = newModel->clone();
              xx->initialSolve();
              assert (xx->isProvenOptimal());
              delete xx;
            }
            numberChangedThisPass++;
            if (columnLower[iColumn]==columnUpper[iColumn])
              numberFixed++;
            else
              numberBounds++;
          }
        }
      }
      if (numberFixed||numberTwo||numberStrengthened||numberBounds)
        handler_->message(CGL_PROCESS_STATS,messages_)
          <<numberFixed<<numberBounds<<numberStrengthened<<numberTwo
          <<CoinMessageEol;
      if (!feasible)
        break;
    }
    if (!feasible)
      break;
    numberChanges +=  numberChangedThisPass;
    if (iPass<numberPasses-1) {
      if (!numberChangedThisPass||iPass==numberPasses-2) {
        // do special probing at end
        firstGenerator=-1;
        lastGenerator=0;
        iPass=numberPasses-2;
      }
    }
  }
  delete [] whichCut;
  int numberRowCuts = twoCuts.sizeRowCuts() ;
  if (numberRowCuts) {
    // add in x=y etc
    CoinBuild build;
    for (int k = 0;k<numberRowCuts;k++) {
      OsiRowCut * thisCut = twoCuts.rowCutPtr(k) ;
      int n=thisCut->row().getNumElements();
      const int * columnCut = thisCut->row().getIndices();
      const double * elementCut = thisCut->row().getElements();
      double lower = thisCut->lb();
      double upper = thisCut->ub();
      build.addRow(n,columnCut,elementCut,lower,upper);
    }
    newModel->addRows(build);
  }
  if (!feasible) {
    delete newModel;
    newModel=NULL;
  }
  return newModel;
}

/* Default Constructor

*/
CglPreProcess::CglPreProcess() 

:
  originalModel_(NULL),
  startModel_(NULL),
  numberSolvers_(0),
  model_(NULL),
  modifiedModel_(NULL),
  presolve_(NULL),
  handler_(NULL),
  defaultHandler_(true),
  appData_(NULL),
  originalColumn_(NULL),
  originalRow_(NULL),
  numberCutGenerators_(0),
  generator_(NULL)
{
  handler_ = new CoinMessageHandler();
  handler_->setLogLevel(2);
  messages_ = CglMessage();
}

// Copy constructor.

CglPreProcess::CglPreProcess(const CglPreProcess & rhs)
:
  numberSolvers_(rhs.numberSolvers_),
  defaultHandler_(rhs.defaultHandler_),
  appData_(rhs.appData_),
  originalColumn_(NULL),
  originalRow_(NULL),
  numberCutGenerators_(rhs.numberCutGenerators_)
{
  if (defaultHandler_) {
    handler_ = new CoinMessageHandler();
    handler_->setLogLevel(rhs.handler_->logLevel());
  } else {
    handler_ = rhs.handler_;
  }
  messages_ = rhs.messages_;
  if (numberCutGenerators_) {
    generator_ = new CglCutGenerator * [numberCutGenerators_];
    for (int i=0;i<numberCutGenerators_;i++) {
      generator_[i]=rhs.generator_[i]->clone();
    }
  } else {
    generator_=NULL;
  }
  if (rhs.originalModel_) {
    originalModel_ = rhs.originalModel_;
    // If no make equality then solvers are same
    if (rhs.originalModel_!=rhs.startModel_) {
      startModel_=rhs.startModel_->clone();
    } else {
      startModel_=originalModel_;
    }
  } else {
    originalModel_=NULL;
    startModel_=NULL;
  }
  if (numberSolvers_) {
    model_ = new OsiSolverInterface * [numberSolvers_];
    modifiedModel_ = new OsiSolverInterface * [numberSolvers_];
    presolve_ = new OsiPresolve * [numberSolvers_];
    for (int i=0;i<numberSolvers_;i++) {
      model_[i]=rhs.model_[i]->clone();
      modifiedModel_[i]=rhs.modifiedModel_[i]->clone();
      presolve_[i]=new OsiPresolve(*rhs.presolve_[i]);
    }
  } else {
    model_=NULL;
    presolve_=NULL;
  }
}
  
// Assignment operator 
CglPreProcess & 
CglPreProcess::operator=(const CglPreProcess& rhs)
{
  if (this!=&rhs) {
    gutsOfDestructor();
    numberSolvers_=rhs.numberSolvers_;
    defaultHandler_=rhs.defaultHandler_;
    appData_=rhs.appData_;
    numberCutGenerators_=rhs.numberCutGenerators_;
    if (defaultHandler_) {
      handler_ = new CoinMessageHandler();
      handler_->setLogLevel(rhs.handler_->logLevel());
    } else {
      handler_ = rhs.handler_;
    }
    messages_ = rhs.messages_;
    if (numberCutGenerators_) {
      generator_ = new CglCutGenerator * [numberCutGenerators_];
      for (int i=0;i<numberCutGenerators_;i++) {
        generator_[i]=rhs.generator_[i]->clone();
      }
    }
    if (rhs.originalModel_) {
      originalModel_ = rhs.originalModel_;
      // If no make equality then solvers are same
      if (rhs.originalModel_!=rhs.startModel_) {
        startModel_=rhs.startModel_->clone();
      } else {
        startModel_=originalModel_;
      }
    } else {
      originalModel_=NULL;
      startModel_=NULL;
    }
    if (numberSolvers_) {
      model_ = new OsiSolverInterface * [numberSolvers_];
      modifiedModel_ = new OsiSolverInterface * [numberSolvers_];
      presolve_ = new OsiPresolve * [numberSolvers_];
      for (int i=0;i<numberSolvers_;i++) {
        model_[i]=rhs.model_[i]->clone();
        modifiedModel_[i]=rhs.modifiedModel_[i]->clone();
        presolve_[i]=new OsiPresolve(*rhs.presolve_[i]);
      }
    } else {
      model_=NULL;
      presolve_=NULL;
    }
  }
  return *this;
}
  
// Destructor 
CglPreProcess::~CglPreProcess ()
{
  gutsOfDestructor();
}
// Clears out as much as possible (except solver)
void 
CglPreProcess::gutsOfDestructor()
{
  if (defaultHandler_) {
    delete handler_;
    handler_ = NULL;
  }
  if (startModel_!=originalModel_) 
    delete startModel_;
  startModel_=NULL;
  //delete originalModel_;
  originalModel_=NULL;
  int i;
  for (i=0;i<numberCutGenerators_;i++) {
    delete generator_[i];
  }
  delete [] generator_;
  generator_=NULL;
  for (i=0;i<numberSolvers_;i++) {
    delete model_[i];
    delete modifiedModel_[i];
    delete presolve_[i];
  }
  delete [] model_;
  delete [] modifiedModel_;
  delete [] presolve_;
  model_=NULL;
  presolve_=NULL;
  delete [] originalColumn_;
  delete [] originalRow_;
  originalColumn_=NULL;
  originalRow_=NULL;
}
// Add one generator
void 
CglPreProcess::addCutGenerator(CglCutGenerator * generator)
{
  CglCutGenerator ** temp = generator_;
  generator_ = new CglCutGenerator * [numberCutGenerators_+1];
  memcpy(generator_,temp,numberCutGenerators_*sizeof(CglCutGenerator *));
  delete[] temp ;
  generator_[numberCutGenerators_++]=generator->clone(); 
}
//#############################################################################
// Set/Get Application Data
// This is a pointer that the application can store into and retrieve
// This field is the application to optionally define and use.
//#############################################################################

void CglPreProcess::setApplicationData(void * appData)
{
  appData_ = appData;
}
//-----------------------------------------------------------------------------
void * CglPreProcess::getApplicationData() const
{
  return appData_;
}
/* Set cutoff bound on the objective function.
   
When using strict comparison, the bound is adjusted by a tolerance to
avoid accidentally cutting off the optimal solution.
*/
void 
CglPreProcess::setCutoff(double value) 
{
  // Solvers know about direction
  double direction = originalModel_->getObjSense();
  originalModel_->setDblParam(OsiDualObjectiveLimit,value*direction); 
}

// Get the cutoff bound on the objective function - always as minimize
double 
CglPreProcess::getCutoff() const
{ 
  double value ;
  originalModel_->getDblParam(OsiDualObjectiveLimit,value) ;
  return value * originalModel_->getObjSense() ;
}
// Pass in Message handler (not deleted at end)
void 
CglPreProcess::passInMessageHandler(CoinMessageHandler * handler)
{
  if (defaultHandler_)
    delete handler_;
  defaultHandler_=false;
  handler_=handler;
}
// Set language
void 
CglPreProcess::newLanguage(CoinMessages::Language language)
{
  messages_ = CglMessage(language);
}
// Return a pointer to the original columns (without clique slacks)
const int * 
CglPreProcess::originalColumns() const
{
  if (!originalColumn_)
    createOriginalIndices();
  return originalColumn_;
}
// Return a pointer to the original rows
const int * 
CglPreProcess::originalRows() const
{
  if (!originalRow_)
    createOriginalIndices();
  return originalRow_;
}
// create original columns and rows
void 
CglPreProcess::createOriginalIndices() const
{
  // Find last model and presolve
  int iPass;
  for (iPass=numberSolvers_-1;iPass>=0;iPass--) {
    if (presolve_[iPass])
      break;
  }
  int nRows,nColumns;
  if (iPass>=0) {
    nRows=model_[iPass]->getNumRows();
    nColumns=model_[iPass]->getNumCols();
  } else {
    nRows=originalModel_->getNumRows();
    nColumns=originalModel_->getNumCols();
  }
  originalColumn_=new int [nColumns];
  originalRow_ = new int[nRows];
  if (iPass>=0) {
    memcpy(originalColumn_,presolve_[iPass]->originalColumns(),
           nColumns*sizeof(int));
    memcpy(originalRow_,presolve_[iPass]->originalRows(),
           nRows*sizeof(int));
    iPass--;
    for (;iPass>=0;iPass--) { 
      const int * originalColumns = presolve_[iPass]->originalColumns();
      int i;
      for (i=0;i<nColumns;i++)
        originalColumn_[i]=originalColumns[originalColumn_[i]];
      const int * originalRows = presolve_[iPass]->originalRows();
      for (i=0;i<nRows;i++)
        originalRow_[i]=originalRows[originalRow_[i]];
    }
  } else {
    int i;
    for (i=0;i<nColumns;i++)
      originalColumn_[i]=i;
    for (i=0;i<nRows;i++)
      originalRow_[i]=i;
  }
}
/* Fix some of problem - returning new problem.
   Uses reduced costs.
   Optional signed character array
   1 always keep, -1 always discard, 0 use djs
   
*/
OsiSolverInterface * 
CglPreProcess::someFixed(OsiSolverInterface & model, 
                                 double fractionToKeep,
                                 bool fixContinuousAsWell,
                                 char * keep) const
{
  model.resolve();
  int numberColumns = model.getNumCols();
  OsiSolverInterface * newModel = model.clone();
  int i;
  const double * lower = model.getColLower();
  const double * upper = model.getColUpper();
  const double * solution = model.getColSolution();
  double * dj = CoinCopyOfArray(model.getReducedCost(),numberColumns);
  int * sort = new int[numberColumns];
  int number=0;
  int numberThrow=0;
  int numberContinuous=0;
  for (i=0;i<numberColumns;i++) {
    if (!model.isInteger(i)&&upper[i]>lower[i])
      numberContinuous++;
    if (model.isInteger(i)||fixContinuousAsWell) {
      if (keep) {
        if (keep[i]==1) {
          continue; // always keep
        } else if (keep[i]==-1) {
          // always fix
          dj[number]=-1.0e30;
          numberThrow++;
          sort[number++]=i;
          continue;
        }
      }
      double value = solution[i];
      if (value<lower[i]+1.0e-8) {
        dj[number]=-dj[i];
        sort[number++]=i;
      } else if (value>upper[number]-1.0e-8) {
        dj[number]=-dj[i];
        sort[number++]=i;
      }
    }
  }
  CoinSort_2(dj,dj+number,sort);
  int numberToFix = (int) (numberColumns *(1.0-fractionToKeep));
  if (!fixContinuousAsWell)
    numberToFix = (int) ((numberColumns-numberContinuous) *(1.0-fractionToKeep));
  numberToFix = CoinMax(numberToFix,numberThrow);
  numberToFix = CoinMin(number,numberToFix);
  printf("%d columns fixed\n",numberToFix);
  for (i=0;i<numberToFix;i++) {
    int iColumn = sort[i];
    double value = solution[iColumn];
    if (value<lower[iColumn]+1.0e-8) {
      newModel->setColUpper(iColumn,lower[iColumn]);
    } else if (value>upper[number]-1.0e-8) {
      newModel->setColLower(iColumn,lower[iColumn]);
    } else {
      // must be a throw away on - go to lower
      newModel->setColUpper(iColumn,lower[iColumn]);
    }
  }
  return newModel;
}
