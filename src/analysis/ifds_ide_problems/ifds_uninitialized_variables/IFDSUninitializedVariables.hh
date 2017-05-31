/*
 * IFDSTaintAnalysis.hh
 *
 *  Created on: 15.09.2016
 *      Author: pdschbrt
 */
#ifndef ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_UNINITIALIZED_VARIABLES_IFDSUNINITIALIZEDVARIABLES_HH_
#define ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_UNINITIALIZED_VARIABLES_IFDSUNINITIALIZEDVARIABLES_HH_

#include "../../ifds_ide/DefaultIFDSTabulationProblem.hh"
#include "../../ifds_ide/DefaultSeeds.hh"
#include "../../ifds_ide/FlowFunction.hh"
#include "../../ifds_ide/flow_func/Gen.hh"
#include "../../ifds_ide/flow_func/Identity.hh"
#include "../../ifds_ide/flow_func/Kill.hh"
#include "../../ifds_ide/flow_func/KillAll.hh"
#include "../../../utils/utils.hh"
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <map>
#include <memory>
#include <set>
#include "../../ifds_ide/SpecialSummaries.hh"
#include "../../ifds_ide/icfg/LLVMBasedICFG.hh"
#include "../../ifds_ide/IFDSSummaryPool.hh"
#include "../../ifds_ide/ZeroValue.hh"
using namespace std;

class IFDSUnitializedVariables
    : public DefaultIFDSTabulationProblem<
          const llvm::Instruction *, const llvm::Value *,
          const llvm::Function *, LLVMBasedICFG &> {
private:
  IFDSSummaryPool<const llvm::Value*> dynSum;

public:
  IFDSUnitializedVariables(LLVMBasedICFG &icfg);

  virtual ~IFDSUnitializedVariables() = default;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getNormalFlowFunction(const llvm::Instruction *curr,
                        const llvm::Instruction *succ) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getCallFlowFuntion(const llvm::Instruction *callStmt,
                     const llvm::Function *destMthd) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getRetFlowFunction(const llvm::Instruction *callSite,
                     const llvm::Function *calleeMthd,
                     const llvm::Instruction *exitStmt,
                     const llvm::Instruction *retSite) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getCallToRetFlowFunction(const llvm::Instruction *callSite,
                           const llvm::Instruction *retSite) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
	getSummaryFlowFunction(const llvm::Instruction *callStmt,
												 const llvm::Function *destMthd,
												 vector<const llvm::Value*> inputs,
												 vector<bool> context) override;

  map<const llvm::Instruction *, set<const llvm::Value *>>
  initialSeeds() override;

  const llvm::Value *createZeroValue() override;
};

#endif /* ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_TAINT_ANALYSIS_IFDSTAINTANALYSIS_HH_ \
          */
