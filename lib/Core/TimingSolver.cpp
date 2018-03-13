//===-- TimingSolver.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TimingSolver.h"

#include "ExecutionState.h"

#include "klee/Config/Version.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Statistics/TimerStatIncrementer.h"
#include "klee/Solver/Solver.h"

#include "CoreStats.h"

using namespace klee;
using namespace llvm;

/***/

bool TimingSolver::evaluate(const ConstraintSet &constraints, ref<Expr> expr,
                            Solver::Validity &result,
                            SolverQueryMetaData &metaData) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? Solver::True : Solver::False;
    return true;
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = ConstraintManager::simplifyExpr(constraints, expr);

  bool success = solver->evaluate(Query(constraints, expr), result);

  metaData.queryCost += timer.delta();

  return success;
}

bool TimingSolver::mustBeTrue(const ConstraintSet &constraints, ref<Expr> expr,
                              bool &result, SolverQueryMetaData &metaData) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? true : false;
    return true;
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = ConstraintManager::simplifyExpr(constraints, expr);

  bool success = solver->mustBeTrue(Query(constraints, expr), result);

  metaData.queryCost += timer.delta();

  return success;
}

bool TimingSolver::mustBeFalse(const ConstraintSet &constraints, ref<Expr> expr,
                               bool &result, SolverQueryMetaData &metaData) {
  return mustBeTrue(constraints, Expr::createIsZero(expr), result, metaData);
}

bool TimingSolver::mayBeTrue(const ConstraintSet &constraints, ref<Expr> expr,
                             bool &result, SolverQueryMetaData &metaData) {
  bool res;
  if (!mustBeFalse(constraints, expr, res, metaData))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::mayBeFalse(const ConstraintSet &constraints, ref<Expr> expr,
                              bool &result, SolverQueryMetaData &metaData) {
  bool res;
  if (!mustBeTrue(constraints, expr, res, metaData))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::getValue(const ConstraintSet &constraints, ref<Expr> expr,
                            ref<ConstantExpr> &result,
                            SolverQueryMetaData &metaData) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE;
    return true;
  }
  
  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs)
    expr = ConstraintManager::simplifyExpr(constraints, expr);

  bool success = solver->getValue(Query(constraints, expr), result);

  metaData.queryCost += timer.delta();

  return success;
}

bool TimingSolver::getValue(const ConstraintSet& constraints, KValue value,
                            ref<ConstantExpr> &segmentResult,
                            ref<ConstantExpr> &offsetResult,
                            SolverQueryMetaData &metaData) {
  ref<Expr> segment = value.getSegment();
  ref<Expr> offset = value.getOffset();
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(segment)) {
    segmentResult = CE;
    return getValue(constraints, offset, offsetResult, metaData);
  }
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset)) {
    offsetResult = CE;
    return getValue(constraints, segment, segmentResult, metaData);
  }

  TimerStatIncrementer timer(stats::solverTime);

  if (simplifyExprs) {
    segment = ConstraintManager::simplifyExpr(constraints, segment);
    offset = ConstraintManager::simplifyExpr(constraints, offset);
  }

  Query query(constraints, ConstantExpr::alloc(0, Expr::Bool));
  std::shared_ptr<const Assignment> assignment;
  bool success = solver->getInitialValues(query, assignment);
  if (success) {
    segmentResult = cast<ConstantExpr>(assignment->evaluate(segment));
    offsetResult = cast<ConstantExpr>(assignment->evaluate(offset));
  }

  metaData.queryCost += timer.delta() / 1e6;

  return success;
}

bool TimingSolver::getInitialValues(
    const ConstraintSet &constraints, std::shared_ptr<const Assignment> &result,
    SolverQueryMetaData &metaData) {
  TimerStatIncrementer timer(stats::solverTime);

  bool success = solver->getInitialValues(
      Query(constraints,
                                                ConstantExpr::alloc(0, Expr::Bool)),
                                          result);

  metaData.queryCost += timer.delta();
  return success;
}

std::pair<ref<Expr>, ref<Expr>>
TimingSolver::getRange(const ConstraintSet &constraints, ref<Expr> expr,
                       SolverQueryMetaData &metaData) {
  TimerStatIncrementer timer(stats::solverTime);
  auto result = solver->getRange(Query(constraints, expr));
  metaData.queryCost += timer.delta();
  return result;
}
