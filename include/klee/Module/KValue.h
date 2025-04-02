//===-- KValue.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KVALUE_H
#define KVALUE_H

#include "klee/Expr/Expr.h"
#include <llvm/Support/raw_ostream.h>

// Special segments. Memory allocated via alloca and on heap
// will have segments that have so higher value.
enum SpecialSegment {
    VALUES_SEGMENT = 0,         // an ordinary numbers
    FUNCTIONS_SEGMENT = 2,      // functions
    ERRNO_SEGMENT = 10,         // errno MemoryObject
    FIRST_ORDINARY_SEGMENT = 11 // allocated memory (on stack, heap and globals)
};

namespace llvm {
  class raw_ostream;
}

namespace klee {
  class KValue {
  public:
    ref<Expr> value;
    ref<Expr> pointerSegment;

  public:
    KValue() {}
    KValue(const KValue &other) : value(other.value), pointerSegment(other.pointerSegment) {}
    KValue(ref<Expr> value)
      : value(value), pointerSegment(ConstantExpr::alloc(VALUES_SEGMENT, value->getWidth())) {}
    KValue(ref<ConstantExpr> value)
      : value(value), pointerSegment(ConstantExpr::alloc(VALUES_SEGMENT, value->getWidth())) {}
    KValue(ref<Expr> segment, ref<Expr> offset)
      : value(offset), pointerSegment(segment) {}
    KValue(uint64_t segment, ref<Expr> offset)
      : value(offset), pointerSegment(ConstantExpr::alloc(segment, value->getWidth())) {}
    KValue(SpecialSegment segment, const ref<Expr> &offset)
        : value(offset), pointerSegment(ConstantExpr::alloc(segment, value->getWidth())) {}

    KValue& operator=(const KValue &other) = default;

    ref<Expr> getValue() const { return value; }
    ref<Expr> getOffset() const { return value; }
    ref<Expr> getSegment() const { return pointerSegment; }

    ref<Expr> createIsZero() const {
      return AndExpr::create(Expr::createIsZero(getSegment()),
                             Expr::createIsZero(getOffset()));
    }

    /// Checks if both segment and offset are ConstantExpr and if yes, if they contain zero value
    bool isZero() const {
      ConstantExpr *segment = dyn_cast<ConstantExpr>(pointerSegment);
      ConstantExpr *offset = dyn_cast<ConstantExpr>(value);
      return (segment && offset && segment->isZero() && offset->isZero());
    }

    bool isConstant() const {
      return isa<ConstantExpr>(value) && isa<ConstantExpr>(pointerSegment);
    }

    Expr::Width getWidth() const {
      return getValue()->getWidth();
    }
    
    KValue ZExt(Expr::Width w) const {
      return KValue(ZExtExpr::create(pointerSegment, w),
                    ZExtExpr::create(value, w));
    }

    KValue SExt(Expr::Width w) const {
      return KValue(SExtExpr::create(pointerSegment, w),
                    SExtExpr::create(value, w));
    }

#define _op_seg_different(op) \
     KValue op(const KValue &other) const { \
      if (getSegment().get()->isZero() && other.getSegment().get()->isZero()) { \
        return KValue(op##Expr::create(value, other.value)); \
      } else { \
        KValue retval = KValue(op##Expr::create(value, other.value)); \
        if (getSegment().get()->isZero()) { \
          retval.pointerSegment = other.getSegment(); \
        } else { \
          retval.pointerSegment = getSegment(); \
        } \
        return retval; \
      } \
    }
#define _op_seg_same(op) \
    KValue op(const KValue &other) const { \
      return KValue(op##Expr::create(pointerSegment, other.pointerSegment), \
                    op##Expr::create(value, other.value)); \
    }
#define _op_seg_zero(op) \
    KValue op(const KValue &other) const { \
      return KValue(op##Expr::create(value, other.value)); \
    }

    _op_seg_same(Concat);

    _op_seg_same(Add);
    _op_seg_same(Sub);
    KValue Mul(const KValue &other) const {
      // multiplying pointers doesn't make sense, but we must ensure that identity 1*x==x works
      return KValue(AddExpr::create(pointerSegment, other.pointerSegment),
                    MulExpr::create(value, other.value));
    }

    _op_seg_different(And);
    _op_seg_different(Or);
    _op_seg_different(Xor);

    _op_seg_zero(UDiv);
    _op_seg_zero(SDiv);
    _op_seg_zero(URem);
    _op_seg_zero(SRem);
    _op_seg_zero(Shl);
    _op_seg_zero(LShr);
    _op_seg_zero(AShr);

#define _op_seg_cmp_lexicographic(cmp) \
    KValue cmp(const KValue &other) const { \
      if (isa<ConstantExpr>(value) && isa<ConstantExpr>(other.value)) { \
        return KValue(SelectExpr::create( \
              EqExpr::create(pointerSegment, other.pointerSegment), \
              cmp##Expr::create(value, other.value), \
              cmp##Expr::create(pointerSegment, other.pointerSegment))); \
      } else { \
        return KValue(cmp##Expr::create(value, other.value)); \
      } \
    }

    _op_seg_cmp_lexicographic(Ugt);
    _op_seg_cmp_lexicographic(Uge);
    _op_seg_cmp_lexicographic(Ult);
    _op_seg_cmp_lexicographic(Ule);
    _op_seg_cmp_lexicographic(Sgt);
    _op_seg_cmp_lexicographic(Sge);
    _op_seg_cmp_lexicographic(Slt);
    _op_seg_cmp_lexicographic(Sle);

    KValue SymbCmp(const KValue &other) const {
      return KValue(EqExpr::create(value, other.value));
    }

    KValue Eq(const KValue &other) const {
      return KValue(AndExpr::create(
                      EqExpr::create(pointerSegment, other.pointerSegment),
                      EqExpr::create(value, other.value)));
    }

    KValue Ne(const KValue &other) const {
      return KValue(OrExpr::create(
                      NeExpr::create(pointerSegment, other.pointerSegment),
                      NeExpr::create(value, other.value)));
    }

    KValue Select(const KValue &b1, const KValue &b2) const {
      return KValue(SelectExpr::create(value, b1.pointerSegment, b2.pointerSegment),
                    SelectExpr::create(value, b1.value, b2.value));
    }

    KValue Extract(unsigned bitOff, Expr::Width width) const {
      return KValue(ExtractExpr::create(pointerSegment, bitOff, width),
                    ExtractExpr::create(value, bitOff, width));
    }

    template <class T>
    static KValue concatValues(const T &input) {
      std::vector<ref<Expr> > segments;
      std::vector<ref<Expr> > values;
      for (const KValue& item : input) {
        segments.push_back(item.getSegment());
        values.push_back(item.getValue());
      }
      return KValue(ConcatExpr::createN(segments.size(), segments.data()),
                    ConcatExpr::createN(values.size(), values.data()));
    }
  };

  inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const KValue &kvalue) {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(kvalue.pointerSegment)) {
      if (CE->isZero()) {
        return os << kvalue.value;
      }
    }
    return os << kvalue.pointerSegment << ':' << kvalue.value;
  }
}

#endif
