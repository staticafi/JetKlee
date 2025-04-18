//===-- RaiseAsm.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Config/Version.h"
#include "klee/Support/ErrorHandling.h"

#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Host.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(14, 0)
#include "llvm/MC/TargetRegistry.h"
#else
#include "llvm/Support/TargetRegistry.h"
#endif
#include "llvm/Target/TargetMachine.h"


using namespace llvm;
using namespace klee;

char RaiseAsmPass::ID = 0;

Function *RaiseAsmPass::getIntrinsic(llvm::Module &M, unsigned IID, Type **Tys,
                                     unsigned NumTys) {
  return Intrinsic::getDeclaration(&M, (llvm::Intrinsic::ID) IID,
                                   llvm::ArrayRef<llvm::Type*>(Tys, NumTys));
}

static bool isnumber(const std::string& str) {
  for (unsigned char c : str) {
    if (!isdigit(c)) {
        return false;
    }
  }
  return true;
}

// FIXME: This should just be implemented as a patch to
// X86TargetAsmInfo.cpp, then everyone will benefit.
bool RaiseAsmPass::runOnInstruction(Module &M, Instruction *I) {
  // We can just raise inline assembler using calls
  CallInst *ci = dyn_cast<CallInst>(I);
  if (!ci)
    return false;

#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
  InlineAsm *ia = dyn_cast<InlineAsm>(ci->getCalledOperand());
#else
  InlineAsm *ia = dyn_cast<InlineAsm>(ci->getCalledValue());
#endif
  if (!ia)
    return false;

  // Try to use existing infrastructure
  if (!TLI)
    return false;

  if (TLI->ExpandInlineAsm(ci))
    return true;

  if ((triple.getArch() == llvm::Triple::x86 ||
       triple.getArch() == llvm::Triple::x86_64) &&
      (triple.isOSLinux() || triple.isMacOSX() || triple.isOSFreeBSD())) {

    if (ia->getAsmString() == "" && ia->hasSideEffects() &&
        ia->getFunctionType()->getReturnType()->isVoidTy()) {
      IRBuilder<> Builder(I);
      Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      // try to match the output of asm
      Value *retval = nullptr;
      const auto& constraints = ia->ParseConstraints();
      for (auto& constr : constraints) {
          if (constr.MatchingInput >= 0) {
            retval = I->getOperand(constr.MatchingInput);
            auto bw = I->getType()->getScalarSizeInBits();
            if (bw == 0)
                continue;

            if (constraints[constr.MatchingInput].Codes.size() != 1)
                continue;

            const auto& inputstr = constraints[constr.MatchingInput].Codes[0];
            if (!isnumber(inputstr))
                continue;

            APInt inputval(bw,
                           constraints[constr.MatchingInput].Codes[0],
                           10);
            auto opval = inputval.getZExtValue();
            if (I->getNumOperands() <= opval) {
                llvm::errs() << I << "\n";
                llvm::errs() << "Invalid operand num for inline asm: "
                             << inputval;
                continue;
            }
            retval = I->getOperand(opval);
            break;
          }
      }
      if (retval) {
        I->replaceAllUsesWith(retval);
      } else {
        I->replaceAllUsesWith(UndefValue::get(I->getType()));
      }
      I->eraseFromParent();
      return true;
    }
  }

  return false;
}

bool RaiseAsmPass::runOnModule(Module &M) {
  bool changed = false;
  std::string Err;

  // Use target triple from the module if possible.
  std::string TargetTriple = M.getTargetTriple();
  if (TargetTriple.empty())
    TargetTriple = llvm::sys::getDefaultTargetTriple();
  const Target *Target = TargetRegistry::lookupTarget(TargetTriple, Err);

  TargetMachine * TM = 0;
  if (Target == 0) {
    klee_warning("Warning: unable to select target: %s", Err.c_str());
    TLI = 0;
  } else {
    TM = Target->createTargetMachine(TargetTriple, "", "", TargetOptions(),
                                     None);
    TLI = TM->getSubtargetImpl(*(M.begin()))->getTargetLowering();

    triple = llvm::Triple(TargetTriple);
  }

  for (Module::iterator fi = M.begin(), fe = M.end(); fi != fe; ++fi) {
    for (Function::iterator bi = fi->begin(), be = fi->end(); bi != be; ++bi) {
      for (BasicBlock::iterator ii = bi->begin(), ie = bi->end(); ii != ie;) {
        Instruction *i = &*ii;
        ++ii;  
        changed |= runOnInstruction(M, i);
      }
    }
  }

  delete TM;

  return changed;
}
