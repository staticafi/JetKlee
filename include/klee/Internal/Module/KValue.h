//===-- Kvalue.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KVALUE_H
#define KVALUE_H

#include <klee/Expr.h>

namespace klee {
  class KValue {
  public:
    ref<Expr> value;
    ref<Expr> pointerSegment;

  public:
    KValue() {}
    KValue(ref<Expr> value)
      : value(value), pointerSegment(ConstantExpr::alloc(0, value->getWidth())) {}
    KValue(ref<Expr> segment, ref<Expr> offset)
      : value(offset), pointerSegment(segment) {}

    ref<Expr> getValue() const { return value; }
    ref<Expr> getOffset() const { return value; }
    ref<Expr> getSegment() const { return pointerSegment; }

    void set(ref<Expr> value) {
      this->value = value;
      this->pointerSegment = ConstantExpr::alloc(0, value->getWidth());
    }

    void set(ref<Expr> segment, ref<Expr> offset) {
      this->pointerSegment = segment;
      this->value = offset;
    }

    ref<Expr> isPointer() const {
      return Expr::createIsZero(pointerSegment);
    }
  };
}

#endif

