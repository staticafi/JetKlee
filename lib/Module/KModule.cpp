//===-- KModule.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "KModule"

#include "Passes.h"

#include "klee/Config/Version.h"
#include "klee/Core/Interpreter.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Module/Cell.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#if LLVM_VERSION_CODE < LLVM_VERSION(8, 0)
#include "llvm/IR/CallSite.h"
#endif
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/Scalar.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
#include "llvm/Transforms/Scalar/Scalarizer.h"
#endif
#include "llvm/Transforms/Utils/Cloning.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(7, 0)
#include "llvm/Transforms/Utils.h"
#endif

#include <cctype>
#include <fstream>
#include <sstream>

using namespace llvm;
using namespace klee;

namespace klee {
cl::OptionCategory
    ModuleCat("Module-related options",
              "These options affect the compile-time processing of the code.");
}

namespace {
  enum SwitchImplType {
    eSwitchTypeSimple,
    eSwitchTypeLLVM,
    eSwitchTypeInternal
  };

  cl::opt<bool>
  OutputSource("output-source",
               cl::desc("Write the assembly for the final transformed source (default=true)"),
               cl::init(true),
	       cl::cat(ModuleCat));

  cl::opt<bool>
  OutputModule("output-module",
               cl::desc("Write the bitcode for the final transformed module"),
               cl::init(false),
	       cl::cat(ModuleCat));

  cl::opt<SwitchImplType>
  SwitchType("switch-type", cl::desc("Select the implementation of switch (default=internal)"),
             cl::values(clEnumValN(eSwitchTypeSimple, "simple", 
                                   "lower to ordered branches"),
                        clEnumValN(eSwitchTypeLLVM, "llvm", 
                                   "lower using LLVM"),
                        clEnumValN(eSwitchTypeInternal, "internal", 
                                   "execute switch internally")),
             cl::init(eSwitchTypeInternal),
	     cl::cat(ModuleCat));
  
  cl::opt<bool>
  DebugPrintEscapingFunctions("debug-print-escaping-functions", 
                              cl::desc("Print functions whose address is taken (default=false)"),
			      cl::cat(ModuleCat));

  // Don't run VerifierPass when checking module
  cl::opt<bool>
  DontVerify("disable-verify",
             cl::desc("Do not verify the module integrity (default=false)"),
             cl::init(false), cl::cat(klee::ModuleCat));

  cl::opt<bool>
  OptimiseKLEECall("klee-call-optimisation",
                             cl::desc("Allow optimization of functions that "
                                      "contain KLEE calls (default=true)"),
                             cl::init(true), cl::cat(ModuleCat));
}

/***/

namespace llvm {
extern void Optimize(Module *, llvm::ArrayRef<const char *> preservedFunctions);
}

// what a hack
static Function *getStubFunctionForCtorList(Module *m,
                                            GlobalVariable *gv, 
                                            std::string name) {
  assert(!gv->isDeclaration() && !gv->hasInternalLinkage() &&
         "do not support old LLVM style constructor/destructor lists");

  std::vector<Type *> nullary;

  Function *fn = Function::Create(FunctionType::get(Type::getVoidTy(m->getContext()),
						    nullary, false),
				  GlobalVariable::InternalLinkage, 
				  name,
                              m);
  BasicBlock *bb = BasicBlock::Create(m->getContext(), "entry", fn);
  llvm::IRBuilder<> Builder(bb);

  // From lli:
  // Should be an array of '{ int, void ()* }' structs.  The first value is
  // the init priority, which we ignore.
  auto arr = dyn_cast<ConstantArray>(gv->getInitializer());
  if (arr) {
    for (unsigned i=0; i<arr->getNumOperands(); i++) {
      auto cs = cast<ConstantStruct>(arr->getOperand(i));
      // There is a third element in global_ctor elements (``i8 @data``).
#if LLVM_VERSION_CODE >= LLVM_VERSION(9, 0)
      assert(cs->getNumOperands() == 3 &&
             "unexpected element in ctor initializer list");
#else
      // before LLVM 9.0, the third operand was optional
      assert((cs->getNumOperands() == 2 || cs->getNumOperands() == 3) &&
             "unexpected element in ctor initializer list");
#endif
      auto fp = cs->getOperand(1);
      if (!fp->isNullValue()) {
        if (auto ce = dyn_cast<llvm::ConstantExpr>(fp))
          fp = ce->getOperand(0);

        if (auto f = dyn_cast<Function>(fp)) {
          Builder.CreateCall(f);
        } else {
          assert(0 && "unable to get function pointer from ctor initializer list");
        }
      }
    }
  }

  Builder.CreateRetVoid();

  return fn;
}

static void
injectStaticConstructorsAndDestructors(Module *m,
                                       llvm::StringRef entryFunction) {
  GlobalVariable *ctors = m->getNamedGlobal("llvm.global_ctors");
  GlobalVariable *dtors = m->getNamedGlobal("llvm.global_dtors");

  if (!ctors && !dtors)
    return;

  Function *mainFn = m->getFunction(entryFunction);
  if (!mainFn)
    klee_error("Entry function '%s' not found in module.",
               entryFunction.str().c_str());

  if (ctors) {
    llvm::IRBuilder<> Builder(&*mainFn->begin()->begin());
    Builder.CreateCall(getStubFunctionForCtorList(m, ctors, "klee.ctor_stub"));
  }

  if (dtors) {
    Function *dtorStub = getStubFunctionForCtorList(m, dtors, "klee.dtor_stub");
    for (Function::iterator it = mainFn->begin(), ie = mainFn->end(); it != ie;
         ++it) {
      if (isa<ReturnInst>(it->getTerminator())) {
        llvm::IRBuilder<> Builder(it->getTerminator());
        Builder.CreateCall(dtorStub);
      }
    }
  }
}

void KModule::addInternalFunction(const char* functionName){
  Function* internalFunction = module->getFunction(functionName);
  if (!internalFunction) {
    KLEE_DEBUG(klee_warning(
        "Failed to add internal function %s. Not found.", functionName));
    return ;
  }
  KLEE_DEBUG(klee_message("Added function %s.",functionName));
  internalFunctions.insert(internalFunction);
}

bool KModule::link(std::vector<std::unique_ptr<llvm::Module>> &modules,
                   const std::string &entryPoint) {
  auto numRemainingModules = modules.size();
  // Add the currently active module to the list of linkables
  modules.push_back(std::move(module));
  std::string error;
  module = std::unique_ptr<llvm::Module>(
      klee::linkModules(modules, entryPoint, error));
  if (!module)
    klee_error("Could not link KLEE files %s", error.c_str());

  targetData = std::unique_ptr<llvm::DataLayout>(new DataLayout(module.get()));

  // Check if we linked anything
  return modules.size() != numRemainingModules;
}

void KModule::instrument(const Interpreter::ModuleOptions &opts) {
  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.
  legacy::PassManager pm;
  pm.add(new RaiseAsmPass());

  // This pass will scalarize as much code as possible so that the Executor
  // does not need to handle operands of vector type for most instructions
  // other than InsertElementInst and ExtractElementInst.
  //
  // NOTE: Must come before division/overshift checks because those passes
  // don't know how to handle vector instructions.
  pm.add(createScalarizerPass());

  // This pass will replace atomic instructions with non-atomic operations
  pm.add(createLowerAtomicPass());
  if (opts.CheckDivZero) pm.add(new DivCheckPass());
  if (opts.CheckOvershift) pm.add(new OvershiftCheckPass());

  pm.add(new IntrinsicCleanerPass(*targetData));
  pm.run(*module);
}

void KModule::optimiseAndPrepare(
    const Interpreter::ModuleOptions &opts,
    llvm::ArrayRef<const char *> preservedFunctions) {
  // Preserve all functions containing klee-related function calls from being
  // optimised around
  if (!OptimiseKLEECall) {
    legacy::PassManager pm;
    pm.add(new OptNonePass());
    pm.run(*module);
  }

  if (opts.Optimize)
    Optimize(module.get(), preservedFunctions);

  // Add internal functions which are not used to check if instructions
  // have been already visited
  if (opts.CheckDivZero)
    addInternalFunction("klee_div_zero_check");
  if (opts.CheckOvershift)
    addInternalFunction("klee_overshift_check");

  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module.get(), opts.EntryPoint);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  legacy::PassManager pm3;
  pm3.add(createCFGSimplificationPass());
  switch(SwitchType) {
  case eSwitchTypeInternal: break;
  case eSwitchTypeSimple: pm3.add(new LowerSwitchPass()); break;
  case eSwitchTypeLLVM:  pm3.add(createLowerSwitchPass()); break;
  default: klee_error("invalid --switch-type");
  }
  pm3.add(new IntrinsicCleanerPass(*targetData));
  pm3.add(createScalarizerPass());
  pm3.add(new PhiCleanerPass());
  pm3.add(new FunctionAliasPass());
  pm3.run(*module);
}

void KModule::manifest(InterpreterHandler *ih, bool forceSourceOutput) {
  if (OutputSource || forceSourceOutput) {
    std::unique_ptr<llvm::raw_fd_ostream> os(ih->openOutputFile("assembly.ll"));
    assert(os && !os->has_error() && "unable to open source output");
    *os << *module;
  }

  if (OutputModule) {
    std::unique_ptr<llvm::raw_fd_ostream> f(ih->openOutputFile("final.bc"));
#if LLVM_VERSION_CODE >= LLVM_VERSION(7, 0)
    WriteBitcodeToFile(*module, *f);
#else
    WriteBitcodeToFile(module.get(), *f);
#endif
  }

  /* Build shadow structures */

  infos = std::unique_ptr<InstructionInfoTable>(
      new InstructionInfoTable(*module.get()));

  std::vector<Function *> declarations;

  for (auto &Function : *module) {
    if (Function.isDeclaration()) {
      declarations.push_back(&Function);
      continue;
    }

    auto kf = std::unique_ptr<KFunction>(new KFunction(&Function, this));

    for (unsigned i=0; i<kf->numInstructions; ++i) {
      KInstruction *ki = kf->instructions[i];
      ki->info = &infos->getInfo(*ki->inst);
    }

    functionMap.insert(std::make_pair(&Function, kf.get()));
    functions.push_back(std::move(kf));
  }

  /* Compute various interesting properties */

  for (auto &kf : functions) {
    if (functionEscapes(kf->function))
      escapingFunctions.insert(kf->function);
  }

  for (auto &declaration : declarations) {
    if (functionEscapes(declaration))
      escapingFunctions.insert(declaration);
  }

  if (DebugPrintEscapingFunctions && !escapingFunctions.empty()) {
    llvm::errs() << "KLEE: escaping functions: [";
    std::string delimiter = "";
    for (auto &Function : escapingFunctions) {
      llvm::errs() << delimiter << Function->getName();
      delimiter = ", ";
    }
    llvm::errs() << "]\n";
  }
}

void KModule::checkModule() {
  InstructionOperandTypeCheckPass *operandTypeCheckPass =
      new InstructionOperandTypeCheckPass();

  legacy::PassManager pm;
  if (!DontVerify)
    pm.add(createVerifierPass());
  pm.add(operandTypeCheckPass);
  pm.run(*module);

  // Enforce the operand type invariants that the Executor expects.  This
  // implicitly depends on the "Scalarizer" pass to be run in order to succeed
  // in the presence of vector instructions.
  if (!operandTypeCheckPass->checkPassed()) {
    klee_error("Unexpected instruction operand types detected");
  }
}

KConstant* KModule::getKConstant(const Constant *c) {
  auto it = constantMap.find(c);
  if (it != constantMap.end())
    return it->second.get();
  return NULL;
}

unsigned KModule::getConstantID(Constant *c, KInstruction* ki) {
  if (KConstant *kc = getKConstant(c))
    return kc->id;  

  unsigned id = constants.size();
  auto kc = std::unique_ptr<KConstant>(new KConstant(c, id, ki));
  constantMap.insert(std::make_pair(c, std::move(kc)));
  constants.push_back(c);
  return id;
}

/***/

KConstant::KConstant(llvm::Constant* _ct, unsigned _id, KInstruction* _ki) {
  ct = _ct;
  id = _id;
  ki = _ki;
}

/***/

static int getOperandNum(Value *v,
                         std::map<Instruction*, unsigned> &registerMap,
                         KModule *km,
                         KInstruction *ki) {
  if (Instruction *inst = dyn_cast<Instruction>(v)) {
    return registerMap[inst];
  } else if (Argument *a = dyn_cast<Argument>(v)) {
    return a->getArgNo();
  } else if (isa<BasicBlock>(v) || isa<InlineAsm>(v) ||
             isa<MetadataAsValue>(v)) {
    return -1;
  } else {
    assert(isa<Constant>(v));
    Constant *c = cast<Constant>(v);
    return -(km->getConstantID(c, ki) + 2);
  }
}

KFunction::KFunction(llvm::Function *_function,
                     KModule *km) 
  : function(_function),
    numArgs(function->arg_size()),
    numInstructions(0),
    trackCoverage(true) {
  // Assign unique instruction IDs to each basic block
  for (auto &BasicBlock : *function) {
    basicBlockEntry[&BasicBlock] = numInstructions;
    numInstructions += BasicBlock.size();
  }

  instructions = new KInstruction*[numInstructions];

  std::map<Instruction*, unsigned> registerMap;

  // The first arg_size() registers are reserved for formals.
  unsigned rnum = numArgs;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it)
      registerMap[&*it] = rnum++;
  }
  numRegisters = rnum;
  
  unsigned i = 0;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it) {
      KInstruction *ki;

      switch(it->getOpcode()) {
      case Instruction::GetElementPtr:
      case Instruction::InsertValue:
      case Instruction::ExtractValue:
        ki = new KGEPInstruction(); break;
      default:
        ki = new KInstruction(); break;
      }

      Instruction *inst = &*it;
      ki->inst = inst;
      ki->dest = registerMap[inst];
      instructionsMap[inst] = ki;

      if (isa<CallInst>(it) || isa<InvokeInst>(it)) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
        const CallBase &cs = cast<CallBase>(*inst);
        Value *val = cs.getCalledOperand();
#else
        const CallSite cs(inst);
        Value *val = cs.getCalledValue();
#endif
        unsigned numArgs = cs.arg_size();
        ki->operands = new int[numArgs+1];
        ki->operands[0] = getOperandNum(val, registerMap, km, ki);
        for (unsigned j=0; j<numArgs; j++) {
          Value *v = cs.getArgOperand(j);
          ki->operands[j+1] = getOperandNum(v, registerMap, km, ki);
        }
      } else {
        unsigned numOperands = it->getNumOperands();
        ki->operands = new int[numOperands];
        for (unsigned j=0; j<numOperands; j++) {
          Value *v = it->getOperand(j);
          ki->operands[j] = getOperandNum(v, registerMap, km, ki);
        }
      }

      instructions[i++] = ki;
    }
  }
}

KFunction::~KFunction() {
  for (unsigned i=0; i<numInstructions; ++i)
    delete instructions[i];
  delete[] instructions;
}

/* Liveness of pointer registers (for the memtrack check) */

struct KFunction::PointerLiveness {
  /// register -> index into the bit vectors, or -1 if not tracked
  std::vector<int> denseOf;
  /// index into the bit vectors -> register
  std::vector<unsigned> regOf;
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> blockIndex;
  /// registers live at the end of each block, including the values that
  /// phi nodes of the successors take from the block
  std::vector<llvm::BitVector> liveOut;
  /// per instruction index: the fresh pointer registers that die there
  /// (KInstruction::dyingFreshPointers points into this)
  std::vector<std::vector<unsigned>> dying;

  bool tracked(int reg) const { return reg >= 0 && denseOf[reg] >= 0; }
};

// Call f(reg) for every register that a (non-phi) instruction reads.
template <typename Fn>
static void forEachRegisterUse(const KInstruction *ki, Fn f) {
  unsigned n;
  if (const auto *cb = dyn_cast<CallBase>(ki->inst))
    n = cb->arg_size() + 1;
  else
    n = ki->inst->getNumOperands();
  for (unsigned j = 0; j < n; ++j)
    if (ki->operands[j] >= 0)
      f(ki->operands[j]);
}

const KFunction::PointerLiveness &KFunction::getPointerLiveness() {
  if (pointerLiveness)
    return *pointerLiveness;

  pointerLiveness = std::make_unique<PointerLiveness>();
  PointerLiveness &L = *pointerLiveness;

  // only registers that hold pointers can keep a block tracked
  L.denseOf.assign(numRegisters, -1);
  auto track = [&](unsigned reg, llvm::Type *type) {
    if (type->isPtrOrPtrVectorTy()) {
      L.denseOf[reg] = L.regOf.size();
      L.regOf.push_back(reg);
    }
  };
  for (unsigned a = 0; a < numArgs; ++a)
    track(getArgRegister(a), function->getArg(a)->getType());
  // the address of a local variable (alloca) never points to the heap, and
  // the variable is a root itself; once its scope ended, it would point to
  // an unbound object and make the search incomplete
  for (unsigned i = 0; i < numInstructions; ++i)
    if (!isa<llvm::AllocaInst>(instructions[i]->inst))
      track(instructions[i]->dest, instructions[i]->inst->getType());

  const unsigned numBlocks = function->size();
  const unsigned width = L.regOf.size();
  std::vector<const llvm::BasicBlock *> blocks;
  blocks.reserve(numBlocks);
  for (const auto &bb : *function) {
    L.blockIndex[&bb] = blocks.size();
    blocks.push_back(&bb);
  }

  // per block: upward-exposed uses, definitions, and the values its
  // successors' phi nodes take from it
  std::vector<llvm::BitVector> use(numBlocks, llvm::BitVector(width)),
      def(numBlocks, llvm::BitVector(width)),
      phiUse(numBlocks, llvm::BitVector(width));
  for (unsigned b = 0; b < numBlocks; ++b) {
    unsigned first = basicBlockEntry[const_cast<llvm::BasicBlock *>(blocks[b])];
    for (unsigned i = first; i < first + blocks[b]->size(); ++i) {
      const KInstruction *ki = instructions[i];
      if (const auto *phi = dyn_cast<llvm::PHINode>(ki->inst)) {
        for (unsigned j = 0; j < phi->getNumIncomingValues(); ++j)
          if (L.tracked(ki->operands[j]))
            phiUse[L.blockIndex[phi->getIncomingBlock(j)]].set(
                L.denseOf[ki->operands[j]]);
      } else {
        forEachRegisterUse(ki, [&](int reg) {
          if (L.tracked(reg) && !def[b].test(L.denseOf[reg]))
            use[b].set(L.denseOf[reg]);
        });
      }
      if (L.tracked(ki->dest))
        def[b].set(L.denseOf[ki->dest]);
    }
  }

  // backward dataflow until a fixpoint:
  // out(b) = phiUse(b) + union of in(s) over the successors s,
  // in(b) = use(b) + (out(b) - def(b))
  L.liveOut.assign(numBlocks, llvm::BitVector(width));
  std::vector<llvm::BitVector> liveIn(numBlocks, llvm::BitVector(width));
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned b = numBlocks; b-- > 0;) {
      llvm::BitVector out = phiUse[b];
      for (const llvm::BasicBlock *succ : llvm::successors(blocks[b]))
        out |= liveIn[L.blockIndex[succ]];
      llvm::BitVector in = out;
      in.reset(def[b]);
      in |= use[b];
      if (out != L.liveOut[b] || in != liveIn[b]) {
        L.liveOut[b] = std::move(out);
        liveIn[b] = std::move(in);
        changed = true;
      }
    }
  }

  computeDyingFreshPointers(blocks);
  return L;
}

// A pointer register is fresh if its value was not loaded from memory: it is
// the result of a call (e.g. malloc), or derived from a fresh pointer by a
// cast, GEP, phi, select or vector operation. When a fresh pointer dies, the
// block it points to may have become untracked. A pointer loaded from memory
// was referenced from memory when it was loaded, and the events that remove
// such references are checked separately, so its death is not checked.
static bool mayPassFreshness(const llvm::Instruction *inst) {
  return isa<llvm::CastInst>(inst) || isa<llvm::GetElementPtrInst>(inst) ||
         isa<llvm::PHINode>(inst) || isa<llvm::SelectInst>(inst) ||
         isa<llvm::ExtractElementInst>(inst) ||
         isa<llvm::InsertElementInst>(inst) ||
         isa<llvm::ShuffleVectorInst>(inst);
}

void KFunction::computeDyingFreshPointers(
    const std::vector<const llvm::BasicBlock *> &blocks) {
  PointerLiveness &L = *pointerLiveness;
  const unsigned width = L.regOf.size();

  // fresh registers, iterated to a fixpoint because of phi cycles
  llvm::BitVector fresh(width);
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned i = 0; i < numInstructions; ++i) {
      const KInstruction *ki = instructions[i];
      if (!L.tracked(ki->dest) || fresh.test(L.denseOf[ki->dest]))
        continue;
      bool isFresh = false;
      if (isa<llvm::CallBase>(ki->inst)) {
        isFresh = true;
      } else if (mayPassFreshness(ki->inst)) {
        for (unsigned j = 0; j < ki->inst->getNumOperands(); ++j) {
          int reg = ki->operands[j];
          if (L.tracked(reg) && fresh.test(L.denseOf[reg]))
            isFresh = true;
        }
      }
      if (isFresh) {
        fresh.set(L.denseOf[ki->dest]);
        changed = true;
      }
    }
  }

  // walk each block backwards with the set of live registers
  L.dying.assign(numInstructions, {});
  for (unsigned b = 0; b < blocks.size(); ++b) {
    unsigned first = basicBlockEntry[const_cast<llvm::BasicBlock *>(blocks[b])];
    llvm::BitVector live = L.liveOut[b];
    for (unsigned i = first + blocks[b]->size(); i-- > first;) {
      KInstruction *ki = instructions[i];
      std::vector<unsigned> &dying = L.dying[i];
      auto addIfDying = [&](int reg) {
        if (L.tracked(reg) && fresh.test(L.denseOf[reg]) &&
            !live.test(L.denseOf[reg]) &&
            std::find(dying.begin(), dying.end(), (unsigned)reg) == dying.end())
          dying.push_back(reg);
      };

      // a result that is never used dies right away
      addIfDying(ki->dest);
      // operands used for the last time; not a pointer that is stored
      // (it moves to memory) or returned (checked at the return)
      if (!isa<llvm::PHINode>(ki->inst) && !isa<llvm::ReturnInst>(ki->inst)) {
        const auto *store = dyn_cast<llvm::StoreInst>(ki->inst);
        forEachRegisterUse(ki, [&](int reg) {
          if (!store || reg != ki->operands[0])
            addIfDying(reg);
        });
      }
      if (!dying.empty())
        ki->dyingFreshPointers = &dying;

      if (L.tracked(ki->dest))
        live.reset(L.denseOf[ki->dest]);
      if (!isa<llvm::PHINode>(ki->inst))
        forEachRegisterUse(ki, [&](int reg) {
          if (L.tracked(reg))
            live.set(L.denseOf[reg]);
        });
    }
  }
}

unsigned KFunction::getInstructionIndex(const KInstruction *ki) const {
  auto it = basicBlockEntry.find(ki->inst->getParent());
  assert(it != basicBlockEntry.end());
  unsigned i = it->second;
  while (instructions[i] != ki)
    ++i;
  return i;
}

// the live registers after instructions[index]
static llvm::BitVector
liveAfterIndex(const KFunction::PointerLiveness &L, KInstruction **instructions,
               unsigned blockEnd, unsigned index,
               const llvm::BitVector &liveOut) {
  llvm::BitVector live = liveOut;
  for (unsigned i = blockEnd; i-- > index + 1;) {
    const KInstruction *ki = instructions[i];
    if (L.tracked(ki->dest))
      live.reset(L.denseOf[ki->dest]);
    // the uses of a phi node are on the incoming edges, not here
    if (!isa<llvm::PHINode>(ki->inst))
      forEachRegisterUse(ki, [&](int reg) {
        if (L.tracked(reg))
          live.set(L.denseOf[reg]);
      });
  }
  return live;
}

static void toRegisters(const KFunction::PointerLiveness &L,
                        const llvm::BitVector &live,
                        std::vector<unsigned> &regs) {
  regs.clear();
  for (unsigned d : live.set_bits())
    regs.push_back(L.regOf[d]);
}

void KFunction::getLivePointerRegistersAfter(const KInstruction *ki,
                                             bool includeDef,
                                             std::vector<unsigned> &regs) {
  const PointerLiveness &L = getPointerLiveness();
  const llvm::BasicBlock *bb = ki->inst->getParent();
  unsigned blockEnd = basicBlockEntry[const_cast<llvm::BasicBlock *>(bb)] +
                      bb->size();
  llvm::BitVector live =
      liveAfterIndex(L, instructions, blockEnd, getInstructionIndex(ki),
                     L.liveOut[L.blockIndex.lookup(bb)]);
  if (!includeDef && L.tracked(ki->dest))
    live.reset(L.denseOf[ki->dest]);
  toRegisters(L, live, regs);
}

void KFunction::getLivePointerRegistersBefore(
    const KInstruction *ki, const llvm::BasicBlock *incomingBB,
    std::vector<unsigned> &regs) {
  const PointerLiveness &L = getPointerLiveness();
  const llvm::BasicBlock *bb = ki->inst->getParent();
  unsigned blockEnd = basicBlockEntry[const_cast<llvm::BasicBlock *>(bb)] +
                      bb->size();
  const llvm::BitVector &liveOut = L.liveOut[L.blockIndex.lookup(bb)];
  unsigned index = getInstructionIndex(ki);
  llvm::BitVector live;

  if (!isa<llvm::PHINode>(ki->inst)) {
    live = liveAfterIndex(L, instructions, blockEnd, index, liveOut);
    if (L.tracked(ki->dest))
      live.reset(L.denseOf[ki->dest]);
    forEachRegisterUse(ki, [&](int reg) {
      if (L.tracked(reg))
        live.set(L.denseOf[reg]);
    });
  } else {
    // the phi nodes from ki on have not executed yet: their results are not
    // defined, and the values they take from incomingBB are live
    unsigned last = index;
    while (last + 1 < blockEnd && isa<llvm::PHINode>(instructions[last + 1]->inst))
      ++last;
    live = liveAfterIndex(L, instructions, blockEnd, last, liveOut);
    for (unsigned i = index; i <= last; ++i) {
      const KInstruction *phiKi = instructions[i];
      if (L.tracked(phiKi->dest))
        live.reset(L.denseOf[phiKi->dest]);
      if (!incomingBB)
        continue;
      int j = cast<llvm::PHINode>(phiKi->inst)->getBasicBlockIndex(incomingBB);
      if (j >= 0 && L.tracked(phiKi->operands[j]))
        live.set(L.denseOf[phiKi->operands[j]]);
    }
  }
  toRegisters(L, live, regs);
}

KInstruction *KModule::getKInstruction(llvm::Instruction *I) const {
    auto it = functionMap.find(I->getParent()->getParent());
    assert(it != functionMap.end());
    return it->second->getKInstruction(I);
}

void KModule::markReturnStatements() {
  // source lines per file; empty if the file cannot be read
  std::map<std::string, std::vector<std::string>> sources;
  auto getLine = [&](const llvm::DILocation *loc) -> const std::string * {
    std::string path = loc->getFilename().str();
    if (path.empty())
      return nullptr;
    if (path[0] != '/' && !loc->getDirectory().empty())
      path = loc->getDirectory().str() + "/" + path;
    auto it = sources.find(path);
    if (it == sources.end()) {
      std::vector<std::string> lines;
      std::ifstream in(path);
      for (std::string line; std::getline(in, line);)
        lines.push_back(std::move(line));
      it = sources.emplace(path, std::move(lines)).first;
    }
    if (loc->getLine() == 0 || loc->getLine() > it->second.size())
      return nullptr;
    return &it->second[loc->getLine() - 1];
  };
  auto isIdentifierChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '$';
  };

  for (auto &kf : functions) {
    for (unsigned i = 0; i < kf->numInstructions; ++i) {
      KInstruction *ki = kf->instructions[i];
      const llvm::DILocation *loc = ki->inst->getDebugLoc().get();
      if (!loc || loc->getInlinedAt() || loc->getColumn() == 0)
        continue;
      const std::string *line = getLine(loc);
      if (!line)
        continue;
      // columns count bytes from 1
      std::size_t col = loc->getColumn() - 1;
      static const std::string keyword = "return";
      if (col >= line->size() ||
          line->compare(col, keyword.size(), keyword) != 0)
        continue;
      std::size_t end = col + keyword.size();
      if ((end < line->size() && isIdentifierChar((*line)[end])) ||
          (col > 0 && isIdentifierChar((*line)[col - 1])))
        continue;
      ki->isReturnStatement = true;
    }
  }
}
