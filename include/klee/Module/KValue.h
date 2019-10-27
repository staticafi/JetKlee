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

namespace klee {
  class KValue {
  public:
    ref<Expr> value;
    ref<Expr> pointerSegment;

  public:
    KValue() {}
    KValue(const KValue &other) : value(other.value), pointerSegment(other.pointerSegment) {}
    KValue(ref<Expr> value)
      : value(value), pointerSegment(ConstantExpr::alloc(0, value->getWidth())) {}
    KValue(ref<Expr> segment, ref<Expr> offset)
      : value(offset), pointerSegment(segment) {}

    KValue& operator=(const KValue &other) = default;

    ref<Expr> getValue() const { return value; }
    ref<Expr> getOffset() const { return value; }
    ref<Expr> getSegment() const { return pointerSegment; }

    ref<Expr> createIsZero() const {
      return AndExpr::create(Expr::createIsZero(getSegment()),
                             Expr::createIsZero(getOffset()));
    }

    void set(ref<Expr> value) {
      this->value = value;
      this->pointerSegment = ConstantExpr::alloc(0, value->getWidth());
    }

    void set(ref<Expr> segment, ref<Expr> offset) {
      this->pointerSegment = segment;
      this->value = offset;
    }

    void setOffset(ref<Expr> offset) {
      this->value = offset;
    }

    bool isConstant() const {
      return isa<ConstantExpr>(value) && isa<ConstantExpr>(pointerSegment);
    }

    ref<Expr> isPointer() const {
      return Expr::createIsZero(pointerSegment);
    }

    Expr::Width getWidth() const {
      return getValue()->getWidth();
    }
  };
}

#endif
