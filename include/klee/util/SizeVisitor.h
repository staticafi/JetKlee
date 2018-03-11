//===-- SizeVisitor.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_UTIL_SIZE_VISITOR_H
#define KLEE_UTIL_SIZE_VISITOR_H

#include "klee/Constraints.h"
#include "klee/Expr.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprVisitor.h"

#include <map>

namespace klee {
  class SizeVisitor : public ExprVisitor {
  public:
    std::map<const Array *, uint64_t> sizes;

    ExprVisitor::Action visitRead(const ReadExpr &expr) {
      ref<Expr> index = evaluate(expr.index);
      assert(isa<ConstantExpr>(index) && "index didn't evaluate to a constant");
      uint64_t &size = sizes[expr.updates.root];
      size = std::max(size, cast<ConstantExpr>(index)->getZExtValue() + 1);
      return Action::doChildren();
    }

    void visitQuery(const Query &query) {
      for(ConstraintManager::constraint_iterator it = query.constraints.begin();
        it != query.constraints.end(); ++it){
        visit(*it);
      }
    }

  protected:
    virtual ref<Expr> evaluate(ref<Expr> expr) = 0;
  };

  class AssignmentSizeVisitor : public SizeVisitor {
  private:
    const Assignment &assignment;
  public:
    AssignmentSizeVisitor(const Assignment &assignment) : assignment(assignment) {}
  protected:
    ref<Expr> evaluate(ref<Expr> expr) {
      return assignment.evaluate(expr);
    }
  };
}

#endif