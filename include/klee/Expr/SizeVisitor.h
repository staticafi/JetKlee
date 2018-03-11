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

#include "klee/Expr/Assignment.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Solver/Solver.h"

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
      for (const auto &constraint : query.constraints) {
        visit(constraint);
      }
      visit(Expr::createIsZero(query.expr));
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
