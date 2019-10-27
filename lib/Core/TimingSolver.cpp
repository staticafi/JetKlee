//===-- TimingSolver.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TimingSolver.h"

#include "klee/Config/Version.h"
#include "klee/ExecutionState.h"
#include "klee/Solver/Solver.h"
#include "klee/Statistics.h"
#include "klee/TimerStatIncrementer.h"

#include "CoreStats.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/ExprUtil.h"

using namespace klee;
using namespace llvm;

/***/

bool TimingSolver::evaluate(const ExecutionState& state, ref<Expr> expr,
                            Solver::Validity &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? Solver::True : Solver::False;
    return true;
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = state.constraints.simplifyExpr(expr);

  bool success = solver->evaluate(Query(state.constraints, expr), result);

  state.queryCost += timer.delta();

  return success;
}

bool TimingSolver::mustBeTrue(const ExecutionState& state, ref<Expr> expr, 
                              bool &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? true : false;
    return true;
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = state.constraints.simplifyExpr(expr);

  bool success = solver->mustBeTrue(Query(state.constraints, expr), result);

  state.queryCost += timer.delta();

  return success;
}

bool TimingSolver::mustBeFalse(const ExecutionState& state, ref<Expr> expr,
                               bool &result) {
  return mustBeTrue(state, Expr::createIsZero(expr), result);
}

bool TimingSolver::mayBeTrue(const ExecutionState& state, ref<Expr> expr, 
                             bool &result) {
  bool res;
  if (!mustBeFalse(state, expr, res))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::mayBeFalse(const ExecutionState& state, ref<Expr> expr, 
                              bool &result) {
  bool res;
  if (!mustBeTrue(state, expr, res))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::getValue(const ExecutionState& state, ref<Expr> expr, 
                            ref<ConstantExpr> &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE;
    return true;
  }
  
  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = state.constraints.simplifyExpr(expr);

  bool success = solver->getValue(Query(state.constraints, expr), result);

  state.queryCost += timer.delta();

  return success;
}

bool TimingSolver::getValue(const ExecutionState& state, KValue value,
                            ref<ConstantExpr> &segmentResult, ref<ConstantExpr> &offsetResult) {
  ref<Expr> segment = value.getSegment();
  ref<Expr> offset = value.getOffset();
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(segment)) {
    segmentResult = CE;
    return getValue(state, offset, offsetResult);
  }
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset)) {
    offsetResult = CE;
    return getValue(state, segment, segmentResult);
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs) {
    segment = state.constraints.simplifyExpr(segment);
    offset = state.constraints.simplifyExpr(offset);
  }

  Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
  std::shared_ptr<const Assignment> assignment;
  bool success = solver->getInitialValues(query, assignment);
  if (success) {
    segmentResult = cast<ConstantExpr>(assignment->evaluate(segment));
    offsetResult = cast<ConstantExpr>(assignment->evaluate(offset));
  }

  state.queryCost += timer.check() / 1e6;

  return success;
}

bool 
TimingSolver::getInitialValues(const ExecutionState& state,
                               std::shared_ptr<const Assignment> &result) {
  TimerStatIncrementer timer(stats::solverTime);

  bool success = solver->getInitialValues(Query(state.constraints,
                                                ConstantExpr::alloc(0, Expr::Bool)), 
                                          result);

  state.queryCost += timer.delta();

  return success;
}

std::pair< ref<ConstantExpr>, ref<ConstantExpr> >
TimingSolver::getRange(const ExecutionState& state, ref<Expr> expr) {
  return solver->getRange(Query(state.constraints, expr));
}
