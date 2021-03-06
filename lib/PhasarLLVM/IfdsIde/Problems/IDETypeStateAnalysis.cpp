/******************************************************************************
 * Copyright (c) 2018 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#include <algorithm>

#include <llvm/IR/CallSite.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

#include <phasar/PhasarLLVM/ControlFlow/LLVMBasedICFG.h>
#include <phasar/PhasarLLVM/IfdsIde/EdgeFunctionComposer.h>
#include <phasar/PhasarLLVM/IfdsIde/EdgeFunctions/EdgeIdentity.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunction.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/Gen.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/GenIf.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/Identity.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/Kill.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/KillAll.h>
#include <phasar/PhasarLLVM/IfdsIde/FlowFunctions/KillMultiple.h>
#include <phasar/PhasarLLVM/IfdsIde/LLVMFlowFunctions/MapFactsToCallee.h>
#include <phasar/PhasarLLVM/IfdsIde/LLVMFlowFunctions/MapFactsToCaller.h>
#include <phasar/PhasarLLVM/IfdsIde/LLVMFlowFunctions/PropagateLoad.h>
#include <phasar/PhasarLLVM/IfdsIde/LLVMZeroValue.h>
#include <phasar/PhasarLLVM/IfdsIde/Problems/IDETypeStateAnalysis.h>
#include <phasar/Utils/LLVMIRToSrc.h>
#include <phasar/Utils/LLVMShorthands.h>
#include <phasar/Utils/Logger.h>

using namespace std;
using namespace psr;

namespace psr {

IDETypeStateAnalysis::IDETypeStateAnalysis(IDETypeStateAnalysis::i_t icfg,
                                           const LLVMTypeHierarchy &th,
                                           const ProjectIRDB &irdb,
                                           const TypeStateDescription &tsd,
                                           vector<string> EntryPoints)
    : LLVMDefaultIDETabulationProblem(icfg, th, irdb), TSD(tsd),
      EntryPoints(EntryPoints), TOP(TSD.top()), BOTTOM(TSD.bottom()) {
  DefaultIDETabulationProblem::zerovalue = createZeroValue();
}

// Start formulating our analysis by specifying the parts required for IFDS

shared_ptr<FlowFunction<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::getNormalFlowFunction(IDETypeStateAnalysis::n_t curr,
                                            IDETypeStateAnalysis::n_t succ) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getNormalFlowFunction()");
  // Check if Alloca's type matches the target type. If so, generate from zero
  // value.
  if (auto Alloca = llvm::dyn_cast<llvm::AllocaInst>(curr)) {
    if (hasMatchingType(Alloca)) {
      return make_shared<Gen<IDETypeStateAnalysis::d_t>>(Alloca, zeroValue());
    }
  }
  // Check load instructions for target type. Generate from the loaded value and
  // kill the load instruction if it was generated previously (strong update!).
  if (auto Load = llvm::dyn_cast<llvm::LoadInst>(curr)) {
    if (hasMatchingType(Load)) {
      struct TSFlowFunction : FlowFunction<IDETypeStateAnalysis::d_t> {
        const llvm::LoadInst *Load;

        TSFlowFunction(const llvm::LoadInst *L) : Load(L) {}
        ~TSFlowFunction() override = default;
        set<IDETypeStateAnalysis::d_t>
        computeTargets(IDETypeStateAnalysis::d_t source) override {
          if (source == Load) {
            return {};
          }
          if (source == Load->getPointerOperand()) {
            return {source, Load};
          }
          return {source};
        }
      };
      return make_shared<TSFlowFunction>(Load);
    }
  }
  // Check store instructions for target type. Perform a strong update, i.e.
  // kill the alloca pointed to by the pointer-operand and all alloca's related
  // to the value-operand and then generate them from the value-operand.
  if (auto Store = llvm::dyn_cast<llvm::StoreInst>(curr)) {
    if (hasMatchingType(Store)) {
      auto RelevantAliasesAndAllocas = getLocalAliasesAndAllocas(
          Store->getValueOperand(),
          curr->getParent()->getParent()->getName().str());

      struct TSFlowFunction : FlowFunction<IDETypeStateAnalysis::d_t> {
        const llvm::StoreInst *Store;
        std::set<IDETypeStateAnalysis::d_t> AliasesAndAllocas;
        TSFlowFunction(const llvm::StoreInst *S,
                       std::set<IDETypeStateAnalysis::d_t> AA)
            : Store(S), AliasesAndAllocas(AA) {}
        ~TSFlowFunction() override = default;
        set<IDETypeStateAnalysis::d_t>
        computeTargets(IDETypeStateAnalysis::d_t source) override {
          // We kill all relevant loacal aliases and alloca's
          if (source != Store->getValueOperand() &&
              AliasesAndAllocas.find(source) != AliasesAndAllocas.end()) {
            return {};
          }
          // Generate all local aliases and relevant alloca's from the stored
          // value
          if (source == Store->getValueOperand()) {
            AliasesAndAllocas.insert(source);
            return AliasesAndAllocas;
          }
          return {source};
        }
      };
      return make_shared<TSFlowFunction>(Store, RelevantAliasesAndAllocas);
    }
  }
  return Identity<IDETypeStateAnalysis::d_t>::getInstance();
}

shared_ptr<FlowFunction<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::getCallFlowFunction(IDETypeStateAnalysis::n_t callStmt,
                                          IDETypeStateAnalysis::m_t destMthd) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getCallFlowFunction()");
  // Kill all data-flow facts if we hit a function of the target API.
  // Those functions are modled within Call-To-Return.
  if (TSD.isAPIFunction(cxx_demangle(destMthd->getName().str()))) {
    return KillAll<IDETypeStateAnalysis::d_t>::getInstance();
  }
  // Otherwise, if we have an ordinary function call, we can just use the
  // standard mapping.
  if (llvm::isa<llvm::CallInst>(callStmt) ||
      llvm::isa<llvm::InvokeInst>(callStmt)) {
    return make_shared<MapFactsToCallee>(llvm::ImmutableCallSite(callStmt),
                                         destMthd);
  }
  assert(false && "callStmt not a CallInst nor a InvokeInst");
}

shared_ptr<FlowFunction<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::getRetFlowFunction(IDETypeStateAnalysis::n_t callSite,
                                         IDETypeStateAnalysis::m_t calleeMthd,
                                         IDETypeStateAnalysis::n_t exitStmt,
                                         IDETypeStateAnalysis::n_t retSite) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getRetFlowFunction()");

  // Besides mapping the formal parameter back into the actual parameter and
  // propagating the return value into the caller context, we also propagate
  // all related alloca's of the formal parameter and the return value.
  struct TSFlowFunction : FlowFunction<IDETypeStateAnalysis::d_t> {
    llvm::ImmutableCallSite CallSite;
    const llvm::Function *CalleeMthd;
    const llvm::ReturnInst *ExitStmt;
    IDETypeStateAnalysis *Analysis;
    std::vector<const llvm::Value *> actuals;
    std::vector<const llvm::Value *> formals;
    TSFlowFunction(llvm::ImmutableCallSite cs, const llvm::Function *calleeMthd,
                   const llvm::Instruction *exitstmt,
                   IDETypeStateAnalysis *analysis)
        : CallSite(cs), CalleeMthd(calleeMthd),
          ExitStmt(llvm::dyn_cast<llvm::ReturnInst>(exitstmt)),
          Analysis(analysis) {
      // Set up the actual parameters
      for (unsigned idx = 0; idx < CallSite.getNumArgOperands(); ++idx) {
        actuals.push_back(CallSite.getArgOperand(idx));
      }
      // Set up the formal parameters
      for (unsigned idx = 0; idx < calleeMthd->arg_size(); ++idx) {
        formals.push_back(getNthFunctionArgument(calleeMthd, idx));
      }
    }

    ~TSFlowFunction() override = default;

    set<IDETypeStateAnalysis::d_t>
    computeTargets(IDETypeStateAnalysis::d_t source) override {
      if (!LLVMZeroValue::getInstance()->isLLVMZeroValue(source)) {
        set<const llvm::Value *> res;
        // Handle C-style varargs functions
        if (CalleeMthd->isVarArg() && !CalleeMthd->isDeclaration()) {
          const llvm::Instruction *AllocVarArg;
          // Find the allocation of %struct.__va_list_tag
          for (auto &BB : *CalleeMthd) {
            for (auto &I : BB) {
              if (auto Alloc = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                if (Alloc->getAllocatedType()->isArrayTy() &&
                    Alloc->getAllocatedType()->getArrayNumElements() > 0 &&
                    Alloc->getAllocatedType()
                        ->getArrayElementType()
                        ->isStructTy() &&
                    Alloc->getAllocatedType()
                            ->getArrayElementType()
                            ->getStructName() == "struct.__va_list_tag") {
                  AllocVarArg = Alloc;
                  // TODO break out this nested loop earlier (without goto ;-)
                }
              }
            }
          }
          // Generate the varargs things by using an over-approximation
          if (source == AllocVarArg) {
            for (unsigned idx = formals.size(); idx < actuals.size(); ++idx) {
              res.insert(actuals[idx]);
            }
          }
        }
        // Handle ordinary case
        // Map formal parameter into corresponding actual parameter.
        for (unsigned idx = 0; idx < formals.size(); ++idx) {
          if (source == formals[idx]) {
            res.insert(actuals[idx]); // corresponding actual
          }
        }
        // Collect the return value
        if (source == ExitStmt->getReturnValue()) {
          res.insert(CallSite.getInstruction());
        }
        // Collect all relevant alloca's to map into caller context
        std::set<IDETypeStateAnalysis::d_t> RelAllocas;
        for (auto fact : res) {
          auto allocas = Analysis->getRelevantAllocas(fact);
          RelAllocas.insert(allocas.begin(), allocas.end());
        }
        res.insert(RelAllocas.begin(), RelAllocas.end());
        return res;
      } else {
        return {source};
      }
    }
  };
  return make_shared<TSFlowFunction>(llvm::ImmutableCallSite(callSite),
                                     calleeMthd, exitStmt, this);
}

shared_ptr<FlowFunction<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::getCallToRetFlowFunction(
    IDETypeStateAnalysis::n_t callSite, IDETypeStateAnalysis::n_t retSite,
    set<IDETypeStateAnalysis::m_t> callees) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getCallToRetFlowFunction()");
  const llvm::ImmutableCallSite CS(callSite);
  for (auto Callee : callees) {
    std::string demangledFname = cxx_demangle(Callee->getName().str());
    // Generate the return value of factory functions from zero value
    if (TSD.isFactoryFunction(demangledFname)) {
      struct TSFlowFunction : FlowFunction<IDETypeStateAnalysis::d_t> {
        IDETypeStateAnalysis::d_t CallSite, ZeroValue;

        TSFlowFunction(IDETypeStateAnalysis::d_t CS,
                       IDETypeStateAnalysis::d_t Z)
            : CallSite(CS), ZeroValue(Z) {}
        ~TSFlowFunction() override = default;
        set<IDETypeStateAnalysis::d_t>
        computeTargets(IDETypeStateAnalysis::d_t source) override {
          if (source == CallSite) {
            return {};
          }
          if (source == ZeroValue) {
            return {source, CallSite};
          }
          return {source};
        }
      };
      return make_shared<TSFlowFunction>(callSite, zeroValue());
    }

    // Handle all functions that are not modeld with special semantics.
    // Kill actual parameters of target type and all its aliases
    // and the corresponding alloca(s) as these data-flow facts are
    // (inter-procedurally) propagated via Call- and the corresponding
    // Return-Flow. Otherwise we might propagate facts with not updated
    // states.
    // Alloca's related to the return value of non-api functions will
    // not be killed during call-to-return, since it is not safe to assume
    // that the return value will be used afterwards, i.e. is stored to memory
    // pointed to by related alloca's.
    if (!TSD.isAPIFunction(demangledFname) && !Callee->isDeclaration()) {
      for (auto &Arg : CS.args()) {
        if (hasMatchingType(Arg)) {
          std::set<IDETypeStateAnalysis::d_t> FactsToKill =
              getWMAliasesAndAllocas(Arg.get());
          return make_shared<KillMultiple<IDETypeStateAnalysis::d_t>>(
              FactsToKill);
        }
      }
    }
  }
  return Identity<IDETypeStateAnalysis::d_t>::getInstance();
}

shared_ptr<FlowFunction<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::getSummaryFlowFunction(
    IDETypeStateAnalysis::n_t callStmt, IDETypeStateAnalysis::m_t destMthd) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getSummaryFlowFunction()");
  return nullptr;
}

map<IDETypeStateAnalysis::n_t, set<IDETypeStateAnalysis::d_t>>
IDETypeStateAnalysis::initialSeeds() {
  // just start in main()
  map<IDETypeStateAnalysis::n_t, set<IDETypeStateAnalysis::d_t>> SeedMap;
  for (auto &EntryPoint : EntryPoints) {
    SeedMap.insert(make_pair(&icfg.getMethod(EntryPoint)->front().front(),
                             set<IDETypeStateAnalysis::d_t>({zeroValue()})));
  }
  return SeedMap;
}

IDETypeStateAnalysis::d_t IDETypeStateAnalysis::createZeroValue() {
  // create a special value to represent the zero value!
  return LLVMZeroValue::getInstance();
}

bool IDETypeStateAnalysis::isZeroValue(IDETypeStateAnalysis::d_t d) const {
  return LLVMZeroValue::getInstance()->isLLVMZeroValue(d);
}

// in addition provide specifications for the IDE parts

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::getNormalEdgeFunction(
    IDETypeStateAnalysis::n_t curr, IDETypeStateAnalysis::d_t currNode,
    IDETypeStateAnalysis::n_t succ, IDETypeStateAnalysis::d_t succNode) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getNormalEdgeFunction()");
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Curr Inst : " << IDETypeStateAnalysis::NtoString(curr));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Curr Node :   "
                << IDETypeStateAnalysis::DtoString(currNode));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Succ Inst : " << IDETypeStateAnalysis::NtoString(succ));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Succ Node :   "
                << IDETypeStateAnalysis::DtoString(succNode));
  // Set alloca instructions of target type to uninitialized.
  if (auto Alloca = llvm::dyn_cast<llvm::AllocaInst>(curr)) {
    if (hasMatchingType(Alloca)) {
      if (currNode == zeroValue() && succNode == Alloca) {
        struct TSAllocaEF : public TSEdgeFunction {
          TSAllocaEF(const TypeStateDescription &tsd, const std::string &tok)
              : TSEdgeFunction(tsd, tok) {}

          IDETypeStateAnalysis::v_t
          computeTarget(IDETypeStateAnalysis::v_t source) override {
            CurrentState = TSD.uninit();
            return CurrentState;
          }

          void print(std::ostream &OS, bool isForDebug = false) const override {
            OS << "TSAllocaEF(" << TSD.stateToString(CurrentState) << ")";
          }
        };
        return make_shared<TSAllocaEF>(TSD, "alloca instruction");
      }
    }
  }
  return EdgeIdentity<IDETypeStateAnalysis::v_t>::getInstance();
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::getCallEdgeFunction(
    IDETypeStateAnalysis::n_t callStmt, IDETypeStateAnalysis::d_t srcNode,
    IDETypeStateAnalysis::m_t destinationMethod,
    IDETypeStateAnalysis::d_t destNode) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getCallEdgeFunction()");
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Call Stmt : "
                << IDETypeStateAnalysis::NtoString(callStmt));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Src Node  :   "
                << IDETypeStateAnalysis::DtoString(srcNode));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(M) Callee    : "
                << IDETypeStateAnalysis::MtoString(destinationMethod));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Dest Node :   "
                << IDETypeStateAnalysis::DtoString(destNode));
  return EdgeIdentity<IDETypeStateAnalysis::v_t>::getInstance();
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::getReturnEdgeFunction(
    IDETypeStateAnalysis::n_t callSite, IDETypeStateAnalysis::m_t calleeMethod,
    IDETypeStateAnalysis::n_t exitStmt, IDETypeStateAnalysis::d_t exitNode,
    IDETypeStateAnalysis::n_t reSite, IDETypeStateAnalysis::d_t retNode) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getReturnEdgeFunction()");
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Call Site : "
                << IDETypeStateAnalysis::NtoString(callSite));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(M) Callee    : "
                << IDETypeStateAnalysis::MtoString(calleeMethod));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Exit Stmt : "
                << IDETypeStateAnalysis::NtoString(exitStmt));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Exit Node :   "
                << IDETypeStateAnalysis::DtoString(exitNode));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Ret Site  : "
                << IDETypeStateAnalysis::NtoString(reSite));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Ret Node  :   "
                << IDETypeStateAnalysis::DtoString(retNode));
  return EdgeIdentity<IDETypeStateAnalysis::v_t>::getInstance();
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::getCallToRetEdgeFunction(
    IDETypeStateAnalysis::n_t callSite, IDETypeStateAnalysis::d_t callNode,
    IDETypeStateAnalysis::n_t retSite, IDETypeStateAnalysis::d_t retSiteNode,
    std::set<IDETypeStateAnalysis::m_t> callees) {
  auto &lg = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "IDETypeStateAnalysis::getCallToRetEdgeFunction()");
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Call Site : "
                << IDETypeStateAnalysis::NtoString(callSite));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Call Node :   "
                << IDETypeStateAnalysis::DtoString(callNode));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(N) Ret Site  : "
                << IDETypeStateAnalysis::NtoString(retSite));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "(D) Ret Node  :   "
                << IDETypeStateAnalysis::DtoString(retSiteNode));
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG) << "(M) Callees   : ");
  for (auto Callee : callees) {
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                  << "  " << IDETypeStateAnalysis::MtoString(Callee));
  }
  const llvm::ImmutableCallSite CS(callSite);
  for (auto Callee : callees) {
    std::string demangledFname = cxx_demangle(Callee->getName().str());

    // For now we assume that we can only generate from the return value.
    // We apply the same edge function for the return value, i.e. callsite.
    if (TSD.isFactoryFunction(demangledFname)) {
      LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG) << "Processing factory function");
      if (isZeroValue(callNode) && retSiteNode == CS.getInstruction()) {
        struct TSFactoryEF : public TSEdgeFunction {
          TSFactoryEF(const TypeStateDescription &tsd, const std::string &tok)
              : TSEdgeFunction(tsd, tok) {}

          IDETypeStateAnalysis::v_t
          computeTarget(IDETypeStateAnalysis::v_t source) override {
            CurrentState = TSD.start();
            return CurrentState;
          }

          void print(std::ostream &OS, bool isForDebug = false) const override {
            OS << "TSFactoryEF(" << TSD.stateToString(CurrentState) << ")";
          }
        };
        return make_shared<TSFactoryEF>(TSD, demangledFname);
      }
    }

    // For every consuming parameter and all its aliases and relevant alloca's
    // we apply the same edge function.
    if (TSD.isConsumingFunction(demangledFname)) {
      LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                    << "Processing consuming function");
      for (auto Idx : TSD.getConsumerParamIdx(demangledFname)) {
        std::set<IDETypeStateAnalysis::d_t> PointsToAndAllocas =
            getWMAliasesAndAllocas(CS.getArgument(Idx));

        if (callNode == retSiteNode &&
            PointsToAndAllocas.find(callNode) != PointsToAndAllocas.end()) {
          return make_shared<TSEdgeFunction>(TSD, demangledFname);
        }
      }
    }
  }
  return EdgeIdentity<IDETypeStateAnalysis::v_t>::getInstance();
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::getSummaryEdgeFunction(
    IDETypeStateAnalysis::n_t callStmt, IDETypeStateAnalysis::d_t callNode,
    IDETypeStateAnalysis::n_t retSite, IDETypeStateAnalysis::d_t retSiteNode) {
  return nullptr;
}

IDETypeStateAnalysis::v_t IDETypeStateAnalysis::topElement() { return TOP; }

IDETypeStateAnalysis::v_t IDETypeStateAnalysis::bottomElement() {
  return BOTTOM;
}

IDETypeStateAnalysis::v_t
IDETypeStateAnalysis::join(IDETypeStateAnalysis::v_t lhs,
                           IDETypeStateAnalysis::v_t rhs) {
  if (lhs == TOP && rhs != BOTTOM) {
    return rhs;
  } else if (rhs == TOP && lhs != BOTTOM) {
    return lhs;
  } else if (lhs == rhs) {
    return lhs;
  } else {
    return BOTTOM;
  }
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::allTopFunction() {
  return make_shared<AllTop<IDETypeStateAnalysis::v_t>>(TOP);
}

void IDETypeStateAnalysis::printNode(std::ostream &os, n_t n) const {
  os << llvmIRToString(n);
}

void IDETypeStateAnalysis::printDataFlowFact(std::ostream &os, d_t d) const {
  os << llvmIRToString(d);
}

void IDETypeStateAnalysis::printMethod(ostream &os,
                                       IDETypeStateAnalysis::m_t m) const {
  os << m->getName().str();
}

void IDETypeStateAnalysis::printValue(ostream &os,
                                      IDETypeStateAnalysis::v_t v) const {
  os << TSD.stateToString(v);
}

shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::TSEdgeFunctionComposer::joinWith(
    shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>> otherFunction) {
  if (otherFunction.get() == this ||
      otherFunction->equal_to(this->shared_from_this())) {
    return this->shared_from_this();
  }
  if (auto *AT = dynamic_cast<AllTop<IDETypeStateAnalysis::v_t> *>(
          otherFunction.get())) {
    return this->shared_from_this();
  }
  return make_shared<AllBottom<IDETypeStateAnalysis::v_t>>(botElement);
}

IDETypeStateAnalysis::v_t IDETypeStateAnalysis::TSEdgeFunction::computeTarget(
    IDETypeStateAnalysis::v_t source) {
  auto &lg = lg::get();
  CurrentState = TSD.getNextState(Token, source);
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                << "State machine transition: (" << Token << " , "
                << TSD.stateToString(source) << ") -> "
                << TSD.stateToString(CurrentState));
  return CurrentState;
}

std::shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::TSEdgeFunction::composeWith(
    std::shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>> secondFunction) {
  if (auto *EI = dynamic_cast<EdgeIdentity<IDETypeStateAnalysis::v_t> *>(
          secondFunction.get())) {
    return this->shared_from_this();
  }
  // TODO: Can we reduce the EF if composed with AllTop?
  return make_shared<TSEdgeFunctionComposer>(this->shared_from_this(),
                                             secondFunction, TSD.bottom());
}

std::shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>>
IDETypeStateAnalysis::TSEdgeFunction::joinWith(
    std::shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>> otherFunction) {
  if (otherFunction.get() == this ||
      otherFunction->equal_to(this->shared_from_this())) {
    return this->shared_from_this();
  }
  // if (auto *EI = dynamic_cast<EdgeIdentity<IDETypeStateAnalysis::v_t> *>(
  //         otherFunction.get())) {
  //   return this->shared_from_this();
  // }
  if (auto *AT = dynamic_cast<AllTop<IDETypeStateAnalysis::v_t> *>(
          otherFunction.get())) {
    return this->shared_from_this();
  }
  return make_shared<AllBottom<IDETypeStateAnalysis::v_t>>(TSD.bottom());
}

bool IDETypeStateAnalysis::TSEdgeFunction::equal_to(
    std::shared_ptr<EdgeFunction<IDETypeStateAnalysis::v_t>> other) const {
  if (auto *TSEF =
          dynamic_cast<IDETypeStateAnalysis::TSEdgeFunction *>(other.get())) {
    return this->CurrentState == TSEF->CurrentState;
  }
  return this == other.get();
}

void IDETypeStateAnalysis::TSEdgeFunction::print(ostream &OS,
                                                 bool isForDebug) const {
  OS << "TSEdgeFunc(" << TSD.stateToString(CurrentState) << ")";
}

std::set<IDETypeStateAnalysis::d_t>
IDETypeStateAnalysis::getRelevantAllocas(IDETypeStateAnalysis::d_t V) {
  if (RelevantAllocaCache.find(V) != RelevantAllocaCache.end()) {
    return RelevantAllocaCache[V];
  } else {
    auto PointsToSet = getWMPointsToSet(V);
    std::set<IDETypeStateAnalysis::d_t> RelevantAllocas;
    auto &lg = lg::get();
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                  << "Compute relevant alloca's of "
                  << IDETypeStateAnalysis::DtoString(V));
    for (auto Alias : PointsToSet) {
      LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                    << "Alias: " << IDETypeStateAnalysis::DtoString(Alias));
      // Collect the pointer operand of a aliased load instruciton
      if (auto Load = llvm::dyn_cast<llvm::LoadInst>(Alias)) {
        if (hasMatchingType(Alias)) {
          LOG_IF_ENABLE(
              BOOST_LOG_SEV(lg, DEBUG)
              << " -> Alloca: "
              << IDETypeStateAnalysis::DtoString(Load->getPointerOperand()));
          RelevantAllocas.insert(Load->getPointerOperand());
        }
      } else {
        // For all other types of aliases, e.g. callsites, function arguments,
        // we check store instructions where thoses aliases are value operands.
        for (auto User : Alias->users()) {
          LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                        << "  User: " << IDETypeStateAnalysis::DtoString(User));
          if (auto Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
            if (hasMatchingType(Store)) {
              LOG_IF_ENABLE(BOOST_LOG_SEV(lg, DEBUG)
                            << "    -> Alloca: "
                            << IDETypeStateAnalysis::DtoString(
                                   Store->getPointerOperand()));
              RelevantAllocas.insert(Store->getPointerOperand());
            }
          }
        }
      }
    }
    for (auto Alias : PointsToSet) {
      RelevantAllocaCache[Alias] = RelevantAllocas;
    }
    return RelevantAllocas;
  }
}

std::set<IDETypeStateAnalysis::d_t>
IDETypeStateAnalysis::getWMPointsToSet(IDETypeStateAnalysis::d_t V) {
  if (PointsToCache.find(V) != PointsToCache.end()) {
    return PointsToCache[V];
  } else {
    auto PointsToSet = icfg.getWholeModulePTG().getPointsToSet(V);
    for (auto Alias : PointsToSet) {
      if (hasMatchingType(Alias))
        PointsToCache[Alias] = PointsToSet;
    }
    return PointsToSet;
  }
}

std::set<IDETypeStateAnalysis::d_t>
IDETypeStateAnalysis::getWMAliasesAndAllocas(IDETypeStateAnalysis::d_t V) {
  std::set<IDETypeStateAnalysis::d_t> PointsToAndAllocas;
  std::set<IDETypeStateAnalysis::d_t> RelevantAllocas = getRelevantAllocas(V);
  std::set<IDETypeStateAnalysis::d_t> Aliases = getWMPointsToSet(V);
  PointsToAndAllocas.insert(Aliases.begin(), Aliases.end());
  PointsToAndAllocas.insert(RelevantAllocas.begin(), RelevantAllocas.end());
  return PointsToAndAllocas;
}

std::set<IDETypeStateAnalysis::d_t>
IDETypeStateAnalysis::getLocalAliasesAndAllocas(IDETypeStateAnalysis::d_t V,
                                                const std::string &Fname) {
  std::set<IDETypeStateAnalysis::d_t> PointsToAndAllocas;
  std::set<IDETypeStateAnalysis::d_t> RelevantAllocas = getRelevantAllocas(V);
  std::set<IDETypeStateAnalysis::d_t> Aliases =
      irdb.getPointsToGraph(Fname)->getPointsToSet(V);
  for (auto Alias : Aliases) {
    if (hasMatchingType(Alias))
      PointsToAndAllocas.insert(Alias);
  }
  // PointsToAndAllocas.insert(Aliases.begin(), Aliases.end());
  PointsToAndAllocas.insert(RelevantAllocas.begin(), RelevantAllocas.end());
  return PointsToAndAllocas;
}

bool IDETypeStateAnalysis::hasMatchingType(IDETypeStateAnalysis::d_t V) {
  // General case
  if (V->getType()->isPointerTy()) {
    if (auto StructTy = llvm::dyn_cast<llvm::StructType>(
            V->getType()->getPointerElementType())) {
      if (StructTy->getName().find(TSD.getTypeNameOfInterest()) !=
          llvm::StringRef::npos) {
        return true;
      }
    }
  }
  if (auto Alloca = llvm::dyn_cast<llvm::AllocaInst>(V)) {
    if (Alloca->getAllocatedType()->isPointerTy()) {
      if (auto StructTy = llvm::dyn_cast<llvm::StructType>(
              Alloca->getAllocatedType()->getPointerElementType())) {
        if (StructTy->getName().find(TSD.getTypeNameOfInterest()) !=
            llvm::StringRef::npos) {
          return true;
        }
      }
    }
    return false;
  }
  if (auto Load = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (Load->getPointerOperand()
            ->getType()
            ->getPointerElementType()
            ->isPointerTy()) {
      if (auto StructTy =
              llvm::dyn_cast<llvm::StructType>(Load->getPointerOperand()
                                                   ->getType()
                                                   ->getPointerElementType()
                                                   ->getPointerElementType())) {
        if (StructTy->getName().find(TSD.getTypeNameOfInterest()) !=
            llvm::StringRef::npos) {
          return true;
        }
      }
    }
    return false;
  }
  if (auto Store = llvm::dyn_cast<llvm::StoreInst>(V)) {
    if (Store->getValueOperand()->getType()->isPointerTy()) {
      if (auto StructTy = llvm::dyn_cast<llvm::StructType>(
              Store->getValueOperand()->getType()->getPointerElementType())) {
        if (StructTy->getName().find(TSD.getTypeNameOfInterest()) !=
            llvm::StringRef::npos) {
          return true;
        }
      }
    }
    return false;
  }
  return false;
}

void IDETypeStateAnalysis::printIDEReport(
    std::ostream &os,
    SolverResults<IDETypeStateAnalysis::n_t, IDETypeStateAnalysis::d_t,
                  IDETypeStateAnalysis::v_t> &SR) {
  os << "\n======= TYPE STATE RESULTS =======\n";
  for (auto &f : icfg.getAllMethods()) {
    os << '\n' << llvmFunctionToSrc(f) << '\n';
    for (auto &BB : *f) {
      for (auto &I : BB) {
        auto results = SR.resultsAt(&I, true);
        if (icfg.isExitStmt(&I)) {
          os << "\nAt exit stmt: " << NtoString(&I) << '\n';
          for (auto res : results) {
            if (auto Alloca = llvm::dyn_cast<llvm::AllocaInst>(res.first)) {
              if (res.second == TSD.error()) {
                os << "\n=== ERROR STATE DETECTED ===\nAlloca: "
                   << DtoString(res.first) << '\n'
                   << llvmValueToSrc(res.first, false) << '\n';
                for (auto Pred : icfg.getPredsOf(&I)) {
                  os << "\nPredecessor: " << NtoString(Pred) << '\n'
                     << llvmValueToSrc(Pred, false) << '\n';
                  auto PredResults = SR.resultsAt(Pred, true);
                  for (auto Res : PredResults) {
                    if (Res.first == Alloca) {
                      os << "Pred State: " << VtoString(Res.second) << '\n';
                    }
                  }
                }
                os << "============================\n";
              } else {
                os << "\nAlloca : " << DtoString(res.first)
                   << "\nState  : " << VtoString(res.second) << '\n'
                   << llvmValueToSrc(res.first, false) << '\n';
              }
            }
          }
        } else {
          for (auto res : results) {
            if (auto Alloca = llvm::dyn_cast<llvm::AllocaInst>(res.first)) {
              if (res.second == TSD.error()) {
                os << "\n=== ERROR STATE DETECTED ===\nAlloca: "
                   << DtoString(res.first) << '\n'
                   << llvmValueToSrc(res.first, false)
                   << "\nAt IR Inst: " << NtoString(&I) << '\n'
                   << llvmValueToSrc(&I, false) << '\n';
                for (auto Pred : icfg.getPredsOf(&I)) {
                  os << "\nPredecessor: " << NtoString(Pred) << '\n'
                     << llvmValueToSrc(Pred, false) << '\n';
                  auto PredResults = SR.resultsAt(Pred, true);
                  for (auto Res : PredResults) {
                    if (Res.first == Alloca) {
                      os << "Pred State: " << VtoString(Res.second) << '\n';
                    }
                  }
                }
                os << "============================\n";
              }
            }
          }
        }
      }
    }
    os << "\n--------------------------------------------\n";
  }
}

} // namespace psr
