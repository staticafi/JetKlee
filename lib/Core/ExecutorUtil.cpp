//===-- ExecutorUtil.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Context.h"
#include "Executor.h"

#include "klee/Config/Version.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/KModule.h"
#include "klee/Solver/Solver.h"
#include "klee/Support/ErrorHandling.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>

using namespace llvm;

namespace klee {

  KValue Executor::evalConstant(const Constant *c,
                                const KInstruction *ki) {
    if (!ki) {
      KConstant* kc = kmodule->getKConstant(c);
      if (kc)
        ki = kc->ki;
    }

    if (const llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      return evalConstantExpr(ce, ki);
    } else {
      if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
        return KValue(ConstantExpr::alloc(ci->getValue()));
      } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {
        return KValue(ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt()));
      } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
        auto it = globalAddresses.find(gv);
        assert(it != globalAddresses.end());
        return it->second;
      } else if (isa<ConstantPointerNull>(c)) {
        return KValue(Expr::createPointer(0));
      } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
        if (getWidthForLLVMType(c->getType()) == 0) {
          if (isa<llvm::LandingPadInst>(ki->inst)) {
            klee_warning_once(0, "Using zero size array fix for landingpad instruction filter");
            return KValue(ConstantExpr::create(0, 1));
          }
        }
        return KValue(ConstantExpr::create(0, getWidthForLLVMType(c->getType())));
      } else if (const ConstantDataSequential *cds =
                 dyn_cast<ConstantDataSequential>(c)) {
        // Handle a vector or array: first element has the smallest address,
        // the last element the highest
        std::vector<KValue> kids;
        for (unsigned i = cds->getNumElements(); i != 0; --i) {
          kids.push_back(evalConstant(cds->getElementAsConstant(i - 1), ki));
        }
        assert(Context::get().isLittleEndian() &&
               "FIXME:Broken for big endian");
        return KValue::concatValues(kids);
      } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
        const StructLayout *sl = kmodule->targetData->getStructLayout(cs->getType());
        llvm::SmallVector<KValue, 4> kids;
        for (unsigned i = cs->getNumOperands(); i != 0; --i) {
          unsigned op = i-1;
          KValue kid = evalConstant(cs->getOperand(op), ki);

          uint64_t thisOffset = sl->getElementOffsetInBits(op),
            nextOffset = (op == cs->getNumOperands() - 1)
            ? sl->getSizeInBits()
            : sl->getElementOffsetInBits(op+1);
          if (nextOffset-thisOffset > kid.getWidth()) {
            uint64_t paddingWidth = nextOffset-thisOffset-kid.getWidth();
            kids.push_back(KValue(ConstantExpr::create(0, paddingWidth)));
          }

          kids.push_back(kid);
        }
        assert(Context::get().isLittleEndian() &&
               "FIXME:Broken for big endian");
        return KValue::concatValues(kids);
      } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)){
        llvm::SmallVector<KValue, 4> kids;
        for (unsigned i = ca->getNumOperands(); i != 0; --i) {
          unsigned op = i-1;
          kids.push_back(evalConstant(ca->getOperand(op), ki));
        }
        assert(Context::get().isLittleEndian() &&
               "FIXME:Broken for big endian");
        return KValue::concatValues(kids);
      } else if (const ConstantVector *cv = dyn_cast<ConstantVector>(c)) {
        llvm::SmallVector<KValue, 8> kids;
        const size_t numOperands = cv->getNumOperands();
        kids.reserve(numOperands);
        for (unsigned i = numOperands; i != 0; --i) {
          kids.push_back(evalConstant(cv->getOperand(i - 1), ki));
        }
        assert(Context::get().isLittleEndian() &&
               "FIXME:Broken for big endian");
        return KValue::concatValues(kids);
      } else if (const BlockAddress * ba = dyn_cast<BlockAddress>(c)) {
        // return the address of the specified basic block in the specified function
        const auto arg_bb = (BasicBlock *) ba->getOperand(1);
        const auto res = Expr::createPointer(reinterpret_cast<std::uint64_t>(arg_bb));
        return KValue(res);
      } else {
        std::string msg("Cannot handle constant ");
        llvm::raw_string_ostream os(msg);
        os << "'" << *c << "' at location "
           << (ki ? ki->getSourceLocation() : "[unknown]");
        klee_error("%s", os.str().c_str());
      }
    }
  }

  KValue Executor::evalConstantExpr(const llvm::ConstantExpr *ce,
                                    const KInstruction *ki) {
    llvm::Type *type = ce->getType();

    KValue op1, op2, op3;
    int numOperands = ce->getNumOperands();

    if (numOperands > 0)
      op1 = evalConstant(ce->getOperand(0), ki);
    if (numOperands > 1)
      op2 = evalConstant(ce->getOperand(1), ki);
    if (numOperands > 2)
      op3 = evalConstant(ce->getOperand(2), ki);

    if (numOperands == 2 && op1.isZero() && ce->getOpcode() == Instruction::GetElementPtr)
      if (!op2.getSegment().isNull())
        op1 = op2;

    /* Checking for possible errors during constant folding */
    switch (ce->getOpcode()) {
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::SRem:
    case Instruction::URem:
      if (cast<ConstantExpr>(op2.getValue())->getLimitedValue() == 0) {
        std::string msg("Division/modulo by zero during constant folding at location ");
        llvm::raw_string_ostream os(msg);
        os << (ki ? ki->getSourceLocation() : "[unknown]");
        klee_error("%s", os.str().c_str());
      }
      break;
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      if (cast<ConstantExpr>(op2.getValue())->getLimitedValue() >= op1.getWidth()) {
        std::string msg("Overshift during constant folding at location ");
        llvm::raw_string_ostream os(msg);
        os << (ki ? ki->getSourceLocation() : "[unknown]");
        klee_error("%s", os.str().c_str());
      }
    }

    std::string msg("Unknown ConstantExpr type");
    llvm::raw_string_ostream os(msg);

    switch (ce->getOpcode()) {
    default :
      os << "'" << *ce << "' at location "
         << (ki ? ki->getSourceLocation() : "[unknown]");
      klee_error("%s", os.str().c_str());

    case Instruction::Trunc:
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
    case Instruction::ZExt:  return op1.ZExt(getWidthForLLVMType(type));
    case Instruction::SExt:  return op1.SExt(getWidthForLLVMType(type));
    case Instruction::Add:   return op1.Add(op2);
    case Instruction::Sub:   return op1.Sub(op2);
    case Instruction::Mul:   return op1.Mul(op2);
    case Instruction::SDiv:  return op1.SDiv(op2);
    case Instruction::UDiv:  return op1.UDiv(op2);
    case Instruction::SRem:  return op1.SRem(op2);
    case Instruction::URem:  return op1.URem(op2);
    case Instruction::And:   return op1.And(op2);
    case Instruction::Or:    return op1.Or(op2);
    case Instruction::Xor:   return op1.Xor(op2);
    case Instruction::Shl:   return op1.Shl(op2);
    case Instruction::LShr:  return op1.LShr(op2);
    case Instruction::AShr:  return op1.AShr(op2);
    case Instruction::BitCast:  return op1;

    case Instruction::GetElementPtr: {
      const Expr::Width pointerWidth = Context::get().getPointerWidth();
      KValue base = op1.ZExt(pointerWidth);

      for (gep_type_iterator ii = gep_type_begin(ce), ie = gep_type_end(ce);
           ii != ie; ++ii) {
        KValue indexOp
            = evalConstant(cast<Constant>(ii.getOperand()), ki);
        ref<ConstantExpr> indexValue = cast<ConstantExpr>(indexOp.getValue());
        if (indexValue->isZero())
			continue;

        // Handle a struct index, which adds its field offset to the pointer.
        if (auto STy = ii.getStructTypeOrNull()) {
          unsigned ElementIdx = indexValue->getZExtValue();
          const StructLayout *SL = kmodule->targetData->getStructLayout(STy);
          base = base.Add(ConstantExpr::alloc(
							APInt(Context::get().getPointerWidth(),
						    SL->getElementOffset(ElementIdx))));
		  continue;
        }

        // For array or vector indices, scale the index by the size of the type.
        // Indices can be negative

        base = base.Add(indexOp.SExt(Context::get().getPointerWidth())
                             .Mul(ConstantExpr::alloc(
                                 APInt(Context::get().getPointerWidth(),
                                       kmodule->targetData->getTypeAllocSize(
										 ii.getIndexedType())))));
      }
      return base;
    }
      
    case Instruction::ICmp: {
      switch(ce->getPredicate()) {
      default: assert(0 && "unhandled ICmp predicate");
      case ICmpInst::ICMP_EQ:  return op1.Eq(op2);
      case ICmpInst::ICMP_NE:  return op1.Ne(op2);
      case ICmpInst::ICMP_UGT: return op1.Ugt(op2);
      case ICmpInst::ICMP_UGE: return op1.Uge(op2);
      case ICmpInst::ICMP_ULT: return op1.Ult(op2);
      case ICmpInst::ICMP_ULE: return op1.Ule(op2);
      case ICmpInst::ICMP_SGT: return op1.Sgt(op2);
      case ICmpInst::ICMP_SGE: return op1.Sge(op2);
      case ICmpInst::ICMP_SLT: return op1.Slt(op2);
      case ICmpInst::ICMP_SLE: return op1.Sle(op2);
      }
    }

    case Instruction::Select:
      return op1.Select(op2, op3);

    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FCmp:
      assert(0 && "floating point ConstantExprs unsupported");
    }
    llvm_unreachable("Unsupported expression in evalConstantExpr");
    return op1;
  }
}
