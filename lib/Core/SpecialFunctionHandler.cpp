//===-- SpecialFunctionHandler.cpp ----------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SpecialFunctionHandler.h"

#include "ExecutionState.h"
#include "Executor.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "MergeHandler.h"
#include "Searcher.h"
#include "StatsTracker.h"
#include "TimingSolver.h"

#include "klee/Config/config.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Support/Casting.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/OptionCategories.h"

#include "llvm/ADT/Twine.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"

#include <errno.h>
#include <sstream>

using namespace llvm;
using namespace klee;

namespace {
cl::opt<bool>
    ReadablePosix("readable-posix-inputs", cl::init(false),
                  cl::desc("Prefer creation of POSIX inputs (command-line "
                           "arguments, files, etc.) with human readable bytes. "
                           "Note: option is expensive when creating lots of "
                           "tests (default=false)"),
                  cl::cat(TestGenCat));

cl::opt<bool>
    SilentKleeAssume("silent-klee-assume", cl::init(false),
                     cl::desc("Silently terminate paths with an infeasible "
                              "condition given to klee_assume() rather than "
                              "emitting an error (default=false)"),
                     cl::cat(TerminationCat));

cl::opt<bool>
    SymbolicMallocs("malloc-symbolic-contents", cl::init(false),
                     cl::desc("Make malloc'ed memory symbolic "
                              "(default=false)"));

} // namespace

/// \todo Almost all of the demands in this file should be replaced
/// with terminateState calls.

///

// FIXME: We are more or less committed to requiring an intrinsic
// library these days. We can move some of this stuff there,
// especially things like realloc which have complicated semantics
// w.r.t. forking. Among other things this makes delayed query
// dispatch easier to implement.
static SpecialFunctionHandler::HandlerInfo handlerInfo[] = {
#define add(name, handler, ret) { name, \
                                  &SpecialFunctionHandler::handler, \
                                  false, ret, false }
#define addDNR(name, handler) { name, \
                                &SpecialFunctionHandler::handler, \
                                true, false, false }
  addDNR("__assert_rtn", handleAssertFail),
  addDNR("__assert_fail", handleAssertFail),
  addDNR("__assert", handleAssertFail),
  addDNR("_assert", handleAssert),
  addDNR("abort", handleAbort),
  addDNR("_Exit", handleExit),
  addDNR("_exit", handleExit),
  { "exit", &SpecialFunctionHandler::handleExit, true, false, true },
  addDNR("klee_abort", handleAbort),
  addDNR("klee_silent_exit", handleSilentExit),
  addDNR("klee_report_error", handleReportError),
  add("calloc", handleCalloc, true),
  add("free", handleFree, false),
  add("klee_assume", handleAssume, false),
  add("klee_check_memory_access", handleCheckMemoryAccess, false),
  add("klee_get_valuef", handleGetValue, true),
  add("klee_get_valued", handleGetValue, true),
  add("klee_get_valuel", handleGetValue, true),
  add("klee_get_valuell", handleGetValue, true),
  add("klee_get_value_i32", handleGetValue, true),
  add("klee_get_value_i64", handleGetValue, true),
  add("klee_define_fixed_object", handleDefineFixedObject, false),
  add("klee_get_obj_size", handleGetObjSize, true),
  add("klee_get_errno", handleGetErrno, true),
#ifndef __APPLE__
  add("__errno_location", handleErrnoLocation, true),
#else
  add("__error", handleErrnoLocation, true),
#endif
  add("klee_is_symbolic", handleIsSymbolic, true),
  add("klee_make_symbolic", handleMakeSymbolic, false),
  add("klee_mark_global", handleMarkGlobal, false),
  add("klee_open_merge", handleOpenMerge, false),
  add("klee_close_merge", handleCloseMerge, false),
  add("klee_prefer_cex", handlePreferCex, false),
  add("klee_posix_prefer_cex", handlePosixPreferCex, false),
  add("klee_print_expr", handlePrintExpr, false),
  add("klee_print_range", handlePrintRange, false),
  add("klee_set_forking", handleSetForking, false),
  add("klee_stack_trace", handleStackTrace, false),
  add("klee_warning", handleWarning, false),
  add("klee_warning_once", handleWarningOnce, false),
  add("malloc", handleMalloc, true),
  add("memalign", handleMemalign, true),
  add("realloc", handleRealloc, true),
  add("__VERIFIER_scope_enter", handleScopeEnter, false),
  add("__VERIFIER_scope_leave", handleScopeLeave, false),
  // SV-COMP special functions. We could define them using
  // klee_make_symbolic, but if we handle them here,
  // it is much easier to generate counter-examples later.
  add("__VERIFIER_nondet_bool", handleVerifierNondetBool, true),
  add("__VERIFIER_nondet__Bool", handleVerifierNondet_Bool, true),
  add("__VERIFIER_nondet_char", handleVerifierNondetChar, true),
  add("__VERIFIER_nondet_int", handleVerifierNondetInt, true),
  add("__VERIFIER_nondet_float", handleVerifierNondetFloat, true),
  add("__VERIFIER_nondet_double", handleVerifierNondetDouble, true),
  add("__VERIFIER_nondet_loff_t", handleVerifierNondetLOffT, true),
  add("__VERIFIER_nondet_long", handleVerifierNondetLong, true),
  add("__VERIFIER_nondet_pchar", handleVerifierNondetPChar, true),
  add("__VERIFIER_nondet_pointer", handleVerifierNondetPointer, true),
  add("__VERIFIER_nondet_pthread_t", handleVerifierNondetPthreadT, true),
  add("__VERIFIER_nondet_sector_t", handleVerifierNondetSectorT, true),
  add("__VERIFIER_nondet_short", handleVerifierNondetShort, true),
  add("__VERIFIER_nondet_size_t", handleVerifierNondetSizeT, true),
  add("__VERIFIER_nondet_u32", handleVerifierNondetU32, true),
  add("__VERIFIER_nondet_uchar", handleVerifierNondetUChar, true),
  add("__VERIFIER_nondet_uint", handleVerifierNondetUInt, true),
  add("__VERIFIER_nondet_uint128", handleVerifierNondetUInt128, true),
  add("__VERIFIER_nondet_ulong", handleVerifierNondetULong, true),
  add("__VERIFIER_nondet_unsigned", handleVerifierNondetUnsigned, true),
  add("__VERIFIER_nondet_ushort", handleVerifierNondetUShort, true),

  add("__VERIFIER_assume", handleAssume, false),

#ifdef SUPPORT_KLEE_EH_CXX
  add("_klee_eh_Unwind_RaiseException_impl", handleEhUnwindRaiseExceptionImpl, false),
  add("klee_eh_typeid_for", handleEhTypeid, true),
#endif

  // operator delete[](void*)
  add("_ZdaPv", handleDeleteArray, false),
  // operator delete(void*)
  add("_ZdlPv", handleDelete, false),

  // operator new[](unsigned int)
  add("_Znaj", handleNewArray, true),
  // operator new(unsigned int)
  add("_Znwj", handleNew, true),

  // FIXME-64: This is wrong for 64-bit long...

  // operator new[](unsigned long)
  add("_Znam", handleNewArray, true),
  // operator new(unsigned long)
  add("_Znwm", handleNew, true),

  // Run clang with -fsanitize=signed-integer-overflow and/or
  // -fsanitize=unsigned-integer-overflow
  add("__ubsan_handle_add_overflow", handleAddOverflow, false),
  add("__ubsan_handle_sub_overflow", handleSubOverflow, false),
  add("__ubsan_handle_mul_overflow", handleMulOverflow, false),
  add("__ubsan_handle_divrem_overflow", handleDivRemOverflow, false),

  add("pthread_create", handlePthreadCreate, true),
  add("pthread_join", handlePthreadJoin, true),
  add("pthread_key_create", handleUnsupportedPthread, true),
  add("pthread_setspecific", handleUnsupportedPthread, true),
  add("pthread_getspecific", handleUnsupportedPthread, true),
  add("scanf", handleScanf, true),
  add("__isoc99_scanf", handleScanf, true),
  add("__isoc99_wscanf", handleScanf, true),
  add("fscanf", handleFscanf, true),
  add("__isoc99_fscanf", handleFscanf, true),
  add("__isoc99_sscanf", handleFscanf, true),
  add("__isoc99_swscanf", handleFscanf, true),

#undef addDNR
#undef add
};

SpecialFunctionHandler::const_iterator SpecialFunctionHandler::begin() {
  return SpecialFunctionHandler::const_iterator(handlerInfo);
}

SpecialFunctionHandler::const_iterator SpecialFunctionHandler::end() {
  // NULL pointer is sentinel
  return SpecialFunctionHandler::const_iterator(0);
}

SpecialFunctionHandler::const_iterator& SpecialFunctionHandler::const_iterator::operator++() {
  ++index;
  if ( index >= SpecialFunctionHandler::size())
  {
    // Out of range, return .end()
    base=0; // Sentinel
    index=0;
  }

  return *this;
}

int SpecialFunctionHandler::size() {
	return sizeof(handlerInfo)/sizeof(handlerInfo[0]);
}

SpecialFunctionHandler::SpecialFunctionHandler(Executor &_executor) 
  : executor(_executor) {}

void SpecialFunctionHandler::prepare(
    std::vector<const char *> &preservedFunctions) {
  unsigned N = size();

  for (unsigned i=0; i<N; ++i) {
    HandlerInfo &hi = handlerInfo[i];
    Function *f = executor.kmodule->module->getFunction(hi.name);

    // No need to create if the function doesn't exist, since it cannot
    // be called in that case.
    if (f && (!hi.doNotOverride || f->isDeclaration())) {
      preservedFunctions.push_back(hi.name);
      // Make sure NoReturn attribute is set, for optimization and
      // coverage counting.
      if (hi.doesNotReturn)
        f->addFnAttr(Attribute::NoReturn);

      // Change to a declaration since we handle internally (simplifies
      // module and allows deleting dead code).
      if (!f->isDeclaration())
        f->deleteBody();
    }
  }
}

void SpecialFunctionHandler::bind() {
  unsigned N = sizeof(handlerInfo)/sizeof(handlerInfo[0]);

  for (unsigned i=0; i<N; ++i) {
    HandlerInfo &hi = handlerInfo[i];
    Function *f = executor.kmodule->module->getFunction(hi.name);
    
    if (f && (!hi.doNotOverride || f->isDeclaration()))
      handlers[f] = std::make_pair(hi.handler, hi.hasReturnValue);
  }
}


bool SpecialFunctionHandler::handle(ExecutionState &state, 
                                    Function *f,
                                    KInstruction *target,
                                    const std::vector<Cell> &arguments) {
  handlers_ty::iterator it = handlers.find(f);
  if (it != handlers.end()) {    
    Handler h = it->second.first;
    bool hasReturnValue = it->second.second;
     // FIXME: Check this... add test?
    if (!hasReturnValue && !target->inst->use_empty()) {
      executor.terminateStateOnExecError(state, 
                                         "expected return value from void special function");
    } else {
      (this->*h)(state, target, arguments);
    }
    return true;
  } else {
    return false;
  }
}

/****/

// reads a concrete string from memory
std::string 
SpecialFunctionHandler::readStringAtAddress(ExecutionState &state, 
                                            const Cell &addressCell) {
  ObjectPair op;
  auto offsetExpr = executor.toUnique(state, addressCell.getOffset());
  if (!isa<ConstantExpr>(offsetExpr)) {
    executor.terminateStateOnUserError(
      state, "String with symbolic offset passed to one of the klee_ functions");
    return "";
  }

  auto segmentExpr = executor.toUnique(state, addressCell.getSegment());
  if (!isa<ConstantExpr>(segmentExpr)) {
    executor.terminateStateOnUserError(
      state, "String with symbolic segment passed to one of the klee_ functions");
    return "";
  }

  KValue address(segmentExpr, offsetExpr);
  if (!state.addressSpace.resolveOneConstantSegment(address, op)) {
    executor.terminateStateOnUserError(
        state, "Invalid string pointer passed to one of the klee_ functions");
    return "";
  }
  const MemoryObject *mo = op.first;
  const ObjectState *os = op.second;

  assert(isa<ConstantExpr>(mo->size) && "string must not be symbolic size");
  size_t size = cast<ConstantExpr>(mo->size)->getZExtValue();

  auto relativeOffset = mo->getOffsetExpr(address.getOffset());
  // the relativeOffset must be concrete as the address is concrete
  size_t offset = cast<ConstantExpr>(relativeOffset)->getZExtValue();

  std::ostringstream buf;
  char c = 0;
  for (size_t i = offset; i < size; ++i) {
    ref<Expr> cur = os->read8(i).getValue();
    cur = executor.toUnique(state, cur);
    assert(isa<ConstantExpr>(cur) && 
           "hit symbolic char while reading concrete string");
    c = cast<ConstantExpr>(cur)->getZExtValue(8);
    if (c == '\0') {
      // we read the whole string
      break;
    }

    buf << c;
  }

  if (c != '\0') {
      klee_warning_once(0, "String not terminated by \\0 passed to "
                           "one of the klee_ functions");
  }

  return buf.str();
}

/****/

void SpecialFunctionHandler::handleAbort(ExecutionState &state,
                                         KInstruction *target,
                                         const std::vector<Cell> &arguments) {
  assert(arguments.size() == 0 && "invalid number of arguments to abort");
  executor.terminateStateOnError(state, "abort failure",
                                 StateTerminationType::Abort);
}

void SpecialFunctionHandler::handleExit(ExecutionState &state,
                                        KInstruction *target,
                                        const std::vector<Cell> &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to exit");
  executor.terminateStateOnExit(state);
}

void SpecialFunctionHandler::handleSilentExit(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to exit");
  executor.terminateStateEarly(state, "", StateTerminationType::SilentExit);
}

void SpecialFunctionHandler::handleAssert(ExecutionState &state,
                                          KInstruction *target,
                                          const std::vector<Cell> &arguments) {
  assert(arguments.size() == 3 && "invalid number of arguments to _assert");
  executor.terminateStateOnError(
      state, "ASSERTION FAIL: " + readStringAtAddress(state, arguments[0]),
      StateTerminationType::Assert);
}

void SpecialFunctionHandler::handleAssertFail(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  assert(arguments.size() == 4 &&
         "invalid number of arguments to __assert_fail");
  executor.terminateStateOnError(
      state, "ASSERTION FAIL: " + readStringAtAddress(state, arguments[0]),
      StateTerminationType::Assert);
}

void SpecialFunctionHandler::handleReportError(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  assert(arguments.size() == 4 &&
         "invalid number of arguments to klee_report_error");

  // arguments[0,1,2,3] are file, line, message, suffix
  executor.terminateStateOnError(
      state, readStringAtAddress(state, arguments[2]),
      StateTerminationType::ReportError, "",
      readStringAtAddress(state, arguments[3]).c_str());
}

void SpecialFunctionHandler::handleOpenMerge(ExecutionState &state,
    KInstruction *target,
    const std::vector<Cell> &arguments) {
  if (!UseMerge) {
    klee_warning_once(0, "klee_open_merge ignored, use '-use-merge'");
    return;
  }

  state.openMergeStack.push_back(
      ref<MergeHandler>(new MergeHandler(&executor, &state)));

  if (DebugLogMerge)
    llvm::errs() << "open merge: " << &state << "\n";
}

void SpecialFunctionHandler::handleCloseMerge(ExecutionState &state,
    KInstruction *target,
    const std::vector<Cell> &arguments) {
  if (!UseMerge) {
    klee_warning_once(0, "klee_close_merge ignored, use '-use-merge'");
    return;
  }
  Instruction *i = target->inst;

  if (DebugLogMerge)
    llvm::errs() << "close merge: " << &state << " at [" << *i << "]\n";

  if (state.openMergeStack.empty()) {
    std::ostringstream warning;
    warning << &state << " ran into a close at " << i << " without a preceding open";
    klee_warning("%s", warning.str().c_str());
  } else {
    assert(executor.mergingSearcher->inCloseMerge.find(&state) ==
               executor.mergingSearcher->inCloseMerge.end() &&
           "State cannot run into close_merge while being closed");
    executor.mergingSearcher->inCloseMerge.insert(&state);
    state.openMergeStack.back()->addClosedState(&state, i);
    state.openMergeStack.pop_back();
  }
}

void SpecialFunctionHandler::handleNew(ExecutionState &state,
                         KInstruction *target,
                         const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to new");

  executor.executeAlloc(state, arguments[0].value, false, target);
}

void SpecialFunctionHandler::handleDelete(ExecutionState &state,
                            KInstruction *target,
                            const std::vector<Cell> &arguments) {
  // FIXME: Should check proper pairing with allocation type (malloc/free,
  // new/delete, new[]/delete[]).

  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to delete");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleNewArray(ExecutionState &state,
                              KInstruction *target,
                              const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to new[]");
  executor.executeAlloc(state, arguments[0].value, false, target);
}

void SpecialFunctionHandler::handleDeleteArray(ExecutionState &state,
                                 KInstruction *target,
                                 const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to delete[]");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleMalloc(ExecutionState &state,
                                  KInstruction *target,
                                  const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to malloc");
  auto *mo = executor.executeAlloc(state, arguments[0].value, false, target);

  if (SymbolicMallocs && mo) {
    executor.executeMakeSymbolic(state, mo, "malloc"+std::to_string(mo->id));
  }
}

void SpecialFunctionHandler::handleMemalign(ExecutionState &state,
                                            KInstruction *target,
                                            const std::vector<Cell> &arguments) {
  if (arguments.size() != 2) {
    executor.terminateStateOnUserError(state,
      "Incorrect number of arguments to memalign(size_t alignment, size_t size)");
    return;
  }

  if (!arguments[0].getSegment()->isZero()) {
    executor.terminateStateOnUserError(state,
      "memalign: alignment argument is not a number");
    return;
  }

  if (!arguments[1].getSegment()->isZero()) {
    executor.terminateStateOnUserError(state,
      "memalign: size argument is not a number");
    return;
  }

  auto alignmentRangeExpr =
      executor.solver->getRange(state.constraints, arguments[0].getValue(),
                                state.queryMetaData);
  ref<Expr> alignmentExpr = alignmentRangeExpr.first;
  auto alignmentConstExpr = dyn_cast<ConstantExpr>(alignmentExpr);

  if (!alignmentConstExpr) {
    executor.terminateStateOnUserError(state, "Could not determine size of symbolic alignment");
    return;
  }

  uint64_t alignment = alignmentConstExpr->getZExtValue();

  // Warn, if the expression has more than one solution
  if (alignmentRangeExpr.first != alignmentRangeExpr.second) {
    klee_warning_once(
        0, "Symbolic alignment for memalign. Choosing smallest alignment");
  }

  executor.executeAlloc(state, arguments[1].getValue(), false, target, false, 0,
                        alignment);
}

#ifdef SUPPORT_KLEE_EH_CXX
void SpecialFunctionHandler::handleEhUnwindRaiseExceptionImpl(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  assert(arguments.size() == 1 &&
         "invalid number of arguments to _klee_eh_Unwind_RaiseException_impl");

  ref<ConstantExpr> exceptionObject = dyn_cast<ConstantExpr>(arguments[0].value);
  if (!exceptionObject.get()) {
    executor.terminateStateOnExecError(state, "Internal error: Symbolic exception pointer");
    return;
  }

  if (isa_and_nonnull<SearchPhaseUnwindingInformation>(
          state.unwindingInformation.get())) {
    executor.terminateStateOnExecError(
        state,
        "Internal error: Unwinding restarted during an ongoing search phase");
    return;
  }

  state.unwindingInformation =
      std::make_unique<SearchPhaseUnwindingInformation>(exceptionObject,
                                                        state.stack.size() - 1);

  executor.unwindToNextLandingpad(state);
}

void SpecialFunctionHandler::handleEhTypeid(ExecutionState &state,
                                            KInstruction *target,
                                            const std::vector<Cell> &arguments) {
  assert(arguments.size() == 1 &&
         "invalid number of arguments to klee_eh_typeid_for");

  executor.bindLocal(target, state, executor.getEhTypeidFor(arguments[0].value));
}
#endif // SUPPORT_KLEE_EH_CXX

void SpecialFunctionHandler::handleAssume(ExecutionState &state,
                            KInstruction *target,
                            const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_assume");

  ref<Expr> e = arguments[0].value;
  
  if (e->getWidth() != Expr::Bool)
    e = NeExpr::create(e, ConstantExpr::create(0, e->getWidth()));
  
  bool res;
  bool success __attribute__((unused)) = executor.solver->mustBeFalse(
      state.constraints, e, res, state.queryMetaData);
  assert(success && "FIXME: Unhandled solver failure");
  if (res) {
    if (SilentKleeAssume) {
      executor.terminateState(state);
    } else {
      executor.terminateStateOnUserError(
          state, "invalid klee_assume call (provably false)");
    }
  } else {
    executor.addConstraint(state, e);
  }
}

void SpecialFunctionHandler::handleIsSymbolic(ExecutionState &state,
                                KInstruction *target,
                                const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_is_symbolic");

  KValue result(ConstantExpr::create(!arguments[0].isConstant(), Expr::Int32));
  executor.bindLocal(target, state, result);
}

void SpecialFunctionHandler::handlePreferCex(ExecutionState &state,
                                             KInstruction *target,
                                             const std::vector<Cell> &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_prefex_cex");

  ref<Expr> cond = arguments[1].value;
  if (cond->getWidth() != Expr::Bool)
    cond = NeExpr::create(cond, ConstantExpr::alloc(0, cond->getWidth()));

  state.addCexPreference(cond);
}

void SpecialFunctionHandler::handlePosixPreferCex(ExecutionState &state,
                                             KInstruction *target,
                                             const std::vector<Cell> &arguments) {
  if (ReadablePosix)
    return handlePreferCex(state, target, arguments);
}

void SpecialFunctionHandler::handlePrintExpr(ExecutionState &state,
                                  KInstruction *target,
                                  const std::vector<Cell> &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_print_expr");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  llvm::errs() << msg_str << ":";
  if (!arguments[1].getSegment()->isZero())
    llvm::errs() << arguments[1].getSegment() << ":";
  llvm::errs() << arguments[1].getValue() << "\n";
}

void SpecialFunctionHandler::handleSetForking(ExecutionState &state,
                                              KInstruction *target,
                                              const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_set_forking");
  ref<Expr> value = executor.toUnique(state, arguments[0].value);
  
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    state.forkDisabled = CE->isZero();
  } else {
    executor.terminateStateOnUserError(state, "klee_set_forking requires a constant arg");
  }
}

void SpecialFunctionHandler::handleStackTrace(ExecutionState &state,
                                              KInstruction *target,
                                              const std::vector<Cell> &arguments) {
  state.dumpStack(outs());
}

void SpecialFunctionHandler::handleWarning(ExecutionState &state,
                                           KInstruction *target,
                                           const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_warning");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  klee_warning("%s: %s", state.stack.back().kf->function->getName().data(), 
               msg_str.c_str());
}

void SpecialFunctionHandler::handleWarningOnce(ExecutionState &state,
                                               KInstruction *target,
                                               const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_warning_once");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  klee_warning_once(0, "%s: %s", state.stack.back().kf->function->getName().data(),
                    msg_str.c_str());
}

void SpecialFunctionHandler::handlePrintRange(ExecutionState &state,
                                  KInstruction *target,
                                  const std::vector<Cell> &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_print_range");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  llvm::errs() << msg_str << ":" << arguments[1];
  if (!isa<ConstantExpr>(arguments[1].value)) {
    // FIXME: Pull into a unique value method?
    ref<ConstantExpr> value;
    bool success __attribute__((unused)) = executor.solver->getValue(
        state.constraints, arguments[1].value, value, state.queryMetaData);
    assert(success && "FIXME: Unhandled solver failure");
    bool res;
    success = executor.solver->mustBeTrue(state.constraints,
                                          EqExpr::create(arguments[1].value, value),
                                          res, state.queryMetaData);
    assert(success && "FIXME: Unhandled solver failure");
    if (res) {
      llvm::errs() << " == " << value;
    } else { 
      llvm::errs() << " ~= " << value;
      std::pair<ref<Expr>, ref<Expr>> res = executor.solver->getRange(
          state.constraints, arguments[1].value, state.queryMetaData);
      llvm::errs() << " (in [" << res.first << ", " << res.second <<"])";
    }
  }
  llvm::errs() << "\n";
}

void SpecialFunctionHandler::handleGetObjSize(ExecutionState &state,
                                  KInstruction *target,
                                  const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_get_obj_size");
  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "klee_get_obj_size");
  for (Executor::ExactResolutionList::iterator it = rl.begin(), 
         ie = rl.end(); it != ie; ++it) {
    executor.bindLocal(
        target, *it->second,
        KValue(it->first.first->size).ZExt(
          executor.kmodule->targetData->getTypeSizeInBits(target->inst->getType())));
  }
}

void SpecialFunctionHandler::handleGetErrno(ExecutionState &state,
                                            KInstruction *target,
                                            const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==0 &&
         "invalid number of arguments to klee_get_errno");
#ifndef WINDOWS
  int *errno_addr = executor.getErrnoLocation(state);
#else
  int *errno_addr = nullptr;
#endif

  // Retrieve the memory object of the errno variable
  ObjectPair result;
  auto segmentExpr = ConstantExpr::create(ERRNO_SEGMENT, Expr::Int64);
  auto addrExpr = ConstantExpr::create((uint64_t)errno_addr, Context::get().getPointerWidth());
  bool resolved;
  Optional<uint64_t> temp;
  state.addressSpace.resolveOne(state, executor.solver,
                                KValue(segmentExpr, addrExpr),
                                result, resolved, temp);
  if (!resolved)
    executor.terminateStateOnUserError(state, "Could not resolve address for errno");
  executor.bindLocal(target, state,
                     KValue(ConstantExpr::create(errno, Expr::Int32)));
}

void SpecialFunctionHandler::handleErrnoLocation(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  // Returns the address of the errno variable
  assert(arguments.size() == 0 &&
         "invalid number of arguments to __errno_location/__error");

#ifndef WINDOWS
  int *errno_addr = executor.getErrnoLocation(state);
#else
  int *errno_addr = nullptr;
#endif

  executor.bindLocal(
      target, state,
      ConstantExpr::create((uint64_t)errno_addr,
                           executor.kmodule->targetData->getTypeSizeInBits(
                               target->inst->getType())));
}
void SpecialFunctionHandler::handleCalloc(ExecutionState &state,
                            KInstruction *target,
                            const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==2 &&
         "invalid number of arguments to calloc");

  ref<Expr> size = MulExpr::create(arguments[0].value,
                                   arguments[1].value);
  executor.executeAlloc(state, size, false, target, true);
}

void SpecialFunctionHandler::handleRealloc(ExecutionState &state,
                            KInstruction *target,
                            const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==2 &&
         "invalid number of arguments to realloc");
  const KValue &address = arguments[0];
  ref<Expr> size = arguments[1].value;

  // If ptr is NULL, then the call is equivalent to malloc(size), for all
  // values of size; if size is equal to zero, and ptr is not NULL, then the
  // call is equivalent to free(ptr).

  auto zeroAddr = executor.fork(
      state, address.createIsZero(), true, BranchType::Realloc);

  if (zeroAddr.first) { // addr == NULL, behave like a 'malloc' was called
    executor.executeAlloc(*zeroAddr.first, size, false, target);
  }

  if (!zeroAddr.second) {
    return;
  }

  // addr != 0
  Executor::StatePair zeroSize = executor.fork(*zeroAddr.second,
                                               Expr::createIsZero(size),
                                               true, BranchType::Realloc);

  if (zeroSize.first) { // size == 0
    executor.executeFree(*zeroSize.first, address, target);
  }
  if (zeroSize.second) { // size != 0
    Executor::StatePair zeroPointer =
        executor.fork(*zeroSize.second, address.createIsZero(), true,
                      BranchType::Realloc);
    if (zeroPointer.first) { // address == 0
      executor.executeAlloc(*zeroPointer.first, size, false, target);
    } 
    if (zeroPointer.second) { // address != 0
      Executor::ExactResolutionList rl;
      executor.resolveExact(*zeroPointer.second, address, rl, "realloc");
      
      for (Executor::ExactResolutionList::iterator it = rl.begin(), 
             ie = rl.end(); it != ie; ++it) {
        if (it->first.second->readOnly) {
          executor.terminateStateOnError(*it->second,
                                         "memory error: realloc of read-only object",
                                         StateTerminationType::Ptr,
                                         executor.getKValueInfo(
                                             *it->second, address));
        } else if (it->first.first->isLocal) {
          executor.terminateStateOnError(*it->second,
                                         "memory error: realloc on local object",
                                         StateTerminationType::Ptr,
                                         executor.getKValueInfo(
                                             *it->second, address));
        } else if (it->first.first->isGlobal) {
          executor.terminateStateOnError(*it->second,
                                         "memory error: realloc on global object",
                                         StateTerminationType::Ptr,
                                         executor.getKValueInfo(
                                             *it->second, address));
        } else {
          executor.executeAlloc(*it->second, size, false, target, false,
                                it->first.second);
        }
      }
    }
  }
}

void SpecialFunctionHandler::handleFree(ExecutionState &state,
                          KInstruction *target,
                          const std::vector<Cell> &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 &&
         "invalid number of arguments to free");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleCheckMemoryAccess(ExecutionState &state,
                                                     KInstruction *target,
                                                     const std::vector<Cell>
                                                       &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_check_memory_access");

  const KValue &address = arguments[0];
  ref<Expr> size = executor.toUnique(state, arguments[1].value);
  if (!address.isConstant() || !isa<ConstantExpr>(size)) {
    executor.terminateStateOnUserError(state, "check_memory_access requires constant args");
  } else {
    ObjectPair op;

    if (!state.addressSpace.resolveOneConstantSegment(address, op)) {
      executor.terminateStateOnError(state,
                                     "check_memory_access: memory error",
                                     StateTerminationType::Ptr,
                                     executor.getKValueInfo(state, address));
    } else {
      ref<Expr> chk =
        op.first->getBoundsCheckPointer(address, cast<ConstantExpr>(size)->getZExtValue());
      if (!chk->isTrue()) {
        executor.terminateStateOnError(state,
                                       "check_memory_access: memory error",
                                       StateTerminationType::Ptr,
                                       executor.getKValueInfo(state, address));
      }
    }
  }
}

void SpecialFunctionHandler::handleGetValue(ExecutionState &state,
                                            KInstruction *target,
                                            const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_get_value");

  // TODO segment
  executor.executeGetValue(state, arguments[0], target);
}

void SpecialFunctionHandler::handleDefineFixedObject(ExecutionState &state,
                                                     KInstruction *target,
                                                     const std::vector<Cell> &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_define_fixed_object");
  // TODO segment
  assert(isa<ConstantExpr>(arguments[0].value) &&
         "expect constant address argument to klee_define_fixed_object");
  // TODO segment
  assert(isa<ConstantExpr>(arguments[1].value) &&
         "expect constant size argument to klee_define_fixed_object");

  // TODO segment
  uint64_t size = cast<ConstantExpr>(arguments[1].value)->getZExtValue();
  ref<ConstantExpr> addressExpr = cast<ConstantExpr>(arguments[0].value);
  uint64_t address = addressExpr->getZExtValue();

  ResolutionList rl;
  Optional<uint64_t> temp;
  state.addressSpace.resolveAddressWithOffset(
      state, executor.solver, addressExpr, rl, temp);
  if (!rl.empty())
    klee_error("Trying to allocate an overlapping object");

  MemoryObject *mo = executor.memory->allocateFixed(size, state.prevPC->inst);
  executor.bindObjectInState(state, mo, false);
  state.addressSpace.concreteAddressMap.emplace(address, mo->segment);
  state.addressSpace.segmentMap.insert(std::make_pair(mo->segment, mo));
  mo->isUserSpecified = true; // XXX hack;
}

void SpecialFunctionHandler::handleScopeEnter(ExecutionState &state,
                                              KInstruction *target,
                                              const std::vector<Cell> &arguments) {
  llvm::Instruction *mem
    = llvm::dyn_cast<Instruction>(target->inst->getOperand(0)->stripPointerCasts());
  if (!mem) {
    executor.terminateStateOnExecError(state,
        "Unhandled argument for scope marker (not an instruction).");
    return;
  }

  auto kinstMem = executor.kmodule->getKInstruction(mem);
  if (!llvm::isa<llvm::AllocaInst>(kinstMem->inst)) {
    executor.terminateStateOnExecError(state,
        "Unhandled argument for scope marker (not alloca)");
    return;
  }

  executor.executeLifetimeIntrinsic(state, target,
                                    kinstMem, arguments[0], false /* is end */);
}

void SpecialFunctionHandler::handleScopeLeave(ExecutionState &state,
                                              KInstruction *target,
                                              const std::vector<Cell> &arguments) {
  llvm::Instruction *mem
    = llvm::dyn_cast<Instruction>(target->inst->getOperand(0)->stripPointerCasts());
  if (!mem) {
    executor.terminateStateOnExecError(state,
        "Unhandled argument for scope marker (not an instruction).");
    return;
  }

  auto kinstMem = executor.kmodule->getKInstruction(mem);
  if (!llvm::isa<llvm::AllocaInst>(kinstMem->inst)) {
    executor.terminateStateOnExecError(state,
        "Unhandled argument for scope marker (not alloca)");
    return;
  }

  executor.executeLifetimeIntrinsic(state, target,
                                    kinstMem, arguments[0], true /* is end */);
}

void SpecialFunctionHandler::handleMakeSymbolic(ExecutionState &state,
                                                KInstruction *target,
                                                const std::vector<Cell> &arguments) {
  std::string name = "";

  if (arguments.size() != 3) {
    executor.terminateStateOnUserError(state,
        "Incorrect number of arguments to klee_make_symbolic(void*, size_t, char*)");
    return;
  }
  bool isZero = arguments[2].pointerSegment->isZero() && arguments[2].value->isZero();
  name = isZero ? "" : readStringAtAddress(state, arguments[2]);

  if (name.length() == 0) {
    name = "unnamed";
    klee_warning("klee_make_symbolic: renamed empty name to \"unnamed\"");
  }

  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "make_symbolic");
  
  for (Executor::ExactResolutionList::iterator it = rl.begin(), 
         ie = rl.end(); it != ie; ++it) {
    const MemoryObject *mo = it->first.first;
    mo->setName(name);
    
    const ObjectState *old = it->first.second;
    ExecutionState *s = it->second;
    
    if (old->readOnly) {
      executor.terminateStateOnUserError(*s, "cannot make readonly object symbolic");
      return;
    } 

    // FIXME: Type coercion should be done consistently somewhere.
    bool res;
    bool success __attribute__((unused)) = executor.solver->mustBeTrue(
        s->constraints,
        EqExpr::create(
            ZExtExpr::create(arguments[1].value, Context::get().getPointerWidth()),
            mo->getSizeExpr()),
        res, s->queryMetaData);
    assert(success && "FIXME: Unhandled solver failure");
    
    if (res) {
      executor.executeMakeSymbolic(*s, mo, name);
    } else {      
      executor.terminateStateOnUserError(*s, "Wrong size given to klee_make_symbolic");
    }
  }
}

void SpecialFunctionHandler::putConcreteValue(ExecutionState& state,
                                              const std::string& name,
                                              bool isSigned,
                                              KInstruction *target,
                                              ref<Expr> expr) {
  assert(isa<ConstantExpr>(expr) && "Assumed constant expression");
  // bind the new concrete value
  executor.bindLocal(target, state, expr);
  // store it in the vector of nondets, so that we have them in the test output
  auto& nnv = state.addNondetValue(KValue(expr), isSigned, name);
  nnv.kinstruction = target;
}

void SpecialFunctionHandler::handleVerifierNondetType(ExecutionState &state,
                                                      KInstruction *target,
                                                      unsigned size,
                                                      bool isSigned,
                                                      const std::string& name,
                                                      bool isPointer) {
  // create nondet value if we are not replaying
  if (executor.replayNondet.empty()) {
    executor.bindLocal(target, state,
                       executor.createNondetValue(state, size,
                                                  isSigned, target,
                                                  name, isPointer));
    return;
  }

  // we're replaying, get the nondet value that we should use
  auto *info = target->info;
  if (info->file.empty()) {
    klee_warning("Replaying nondet without debugging information, "
                 "using nondet value instead of concrete");
    executor.bindLocal(target, state,
                       executor.createNondetValue(state, size,
                                                  isSigned, target,
                                                  name, isPointer));
    return;
  }

  // position in the nondet vector we're in
  static unsigned position = 0;

  if (position >= executor.replayNondet.size()) {
   //klee_warning("Got out of nondet values while replaying, using nondet");
   //executor.bindLocal(target, state,
   //                   executor.createNondetValue(state, size,
   //                                              isSigned, target, name));

    klee_warning("Got out of nondet values while replaying, using 0");
    putConcreteValue(state, name, isSigned,
                     target, ConstantExpr::alloc(0, size));
    return;
  }

  auto& nondet = executor.replayNondet[position];
  //klee_warning("Matching %s:%u:%u", name.c_str(), info->line, info->column);

  if (std::get<0>(nondet) == name &&
      std::get<1>(nondet) == info->line &&
      std::get<2>(nondet) == info->column) {
      auto& val = std::get<3>(nondet);

     //klee_warning("Matched nondet value for: %s:%u:%u to %lu",
     //             std::get<0>(nondet).c_str(), std::get<1>(nondet),
     //             std::get<2>(nondet), val.getZExtValue());

      putConcreteValue(state, name, val.isSigned(), target,
                       ConstantExpr::alloc(val.getZExtValue(), size));

    ++position; // matched, move on
  } else {
    klee_warning("Did not match nondet value for: %s:%u:%u, using 0",
   //              "using nondet value",
                 std::get<0>(nondet).c_str(), std::get<1>(nondet),
                 std::get<2>(nondet));

   //executor.bindLocal(target, state,
   //                   executor.createNondetValue(state, size,
   //                                              isSigned, target, name));
    putConcreteValue(state, name, isSigned,
                     target, ConstantExpr::alloc(0, size));
  }
}

void SpecialFunctionHandler::handleVerifierNondetInt(ExecutionState &state,
                                                     KInstruction *target,
                                                     const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int32,
                           /* isSigned = */ true, "__VERIFIER_nondet_int");
}

void SpecialFunctionHandler::handleVerifierNondetUInt(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int32,
                           /* isSigned = */ false, "__VERIFIER_nondet_uint");
}

void SpecialFunctionHandler::handleVerifierNondetUInt128(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int128,
                           /* isSigned = */ false, "__VERIFIER_nondet_uint128");
}

void SpecialFunctionHandler::handleVerifierNondetBool(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Bool, // XXX: should we use i1?
                           /* isSigned = */ false, "__VERIFIER_nondet_bool");
}

void SpecialFunctionHandler::handleVerifierNondet_Bool(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Bool, // XXX: should we use i1?
                           /* isSigned = */ false, "__VERIFIER_nondet__Bool");
}

void SpecialFunctionHandler::handleVerifierNondetChar(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int8,
                           /* isSigned = */ true, "__VERIFIER_nondet_char");
}

void SpecialFunctionHandler::handleVerifierNondetUChar(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int8,
                           /* isSigned = */ false, "__VERIFIER_nondet_uchar");
}

void SpecialFunctionHandler::handleVerifierNondetFloat(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, 8*sizeof(float), // XXX: fixme
                           /* isSigned = */ true, "__VERIFIER_nondet_float");
}

void SpecialFunctionHandler::handleVerifierNondetDouble(ExecutionState &state,
                                                        KInstruction *target,
                                                        const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, 8*sizeof(double), // XXX: fixme
                           /* isSigned = */ true, "__VERIFIER_nondet_double");
}

void SpecialFunctionHandler::handleVerifierNondetLOffT(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int32, // XXX: fixme
                           /* isSigned = */ false, "__VERIFIER_nondet_loff_t");
}

void SpecialFunctionHandler::handleVerifierNondetLong(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64, // XXX: fixme
                           /* isSigned = */ true, "__VERIFIER_nondet_long");
}

void SpecialFunctionHandler::handleVerifierNondetULong(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64,
                           /* isSigned = */ false, "__VERIFIER_nondet_ulong");
}

void SpecialFunctionHandler::handleVerifierNondetPointer(ExecutionState &state,
                                                         KInstruction *target,
                                                         const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64, // XXX: fixme
                           /* isSigned = */ false, "__VERIFIER_nondet_pointer",
                           /* isPointer = */ true);
}

void SpecialFunctionHandler::handleVerifierNondetPChar(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64, // XXX: fixme
                           /* isSigned = */ false, "__VERIFIER_nondet_pchar",
                           /* isPointer = */ true);
}

void SpecialFunctionHandler::handleVerifierNondetPthreadT(ExecutionState &state,
                                                          KInstruction *target,
                                                          const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64,
                           /* isSigned = */ false, "__VERIFIER_nondet_pthread_t");
}

void SpecialFunctionHandler::handleVerifierNondetUShort(ExecutionState &state,
                                                        KInstruction *target,
                                                        const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int16,
                           /* isSigned = */ false, "__VERIFIER_nondet_ushort");
}

void SpecialFunctionHandler::handleVerifierNondetShort(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int16,
                           /* isSigned = */ true, "__VERIFIER_nondet_short");
}

void SpecialFunctionHandler::handleVerifierNondetSizeT(ExecutionState &state,
                                                       KInstruction *target,
                                                       const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64,
                           /* isSigned = */ false, "__VERIFIER_nondet_u32");
}

void SpecialFunctionHandler::handleVerifierNondetU32(ExecutionState &state,
                                                     KInstruction *target,
                                                     const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int32,
                           /* isSigned = */ false, "__VERIFIER_nondet_u32");
}

void SpecialFunctionHandler::handleVerifierNondetUnsigned(ExecutionState &state,
                                                          KInstruction *target,
                                                          const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int32,
                           /* isSigned = */ false, "__VERIFIER_nondet_unsigned");
}

void SpecialFunctionHandler::handleVerifierNondetSectorT(ExecutionState &state,
                                                         KInstruction *target,
                                                         const std::vector<Cell> &arguments) {
  assert(arguments.empty() && "Wrong number of arguments");

  handleVerifierNondetType(state, target, Expr::Int64,
                           /* isSigned = */ false, "__VERIFIER_nondet_sector_t");
}

void SpecialFunctionHandler::handleMarkGlobal(ExecutionState &state,
                                              KInstruction *target,
                                              const std::vector<Cell> &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_mark_global");  

  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "mark_global");
  
  for (Executor::ExactResolutionList::iterator it = rl.begin(), 
         ie = rl.end(); it != ie; ++it) {
    const MemoryObject *mo = it->first.first;
    assert(!mo->isLocal);
    mo->isGlobal = true;
  }
}

void SpecialFunctionHandler::handleAddOverflow(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  executor.terminateStateOnError(state, "overflow on addition",
                                 StateTerminationType::Overflow);
}

void SpecialFunctionHandler::handleSubOverflow(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  executor.terminateStateOnError(state, "overflow on subtraction",
                                 StateTerminationType::Overflow);
}

void SpecialFunctionHandler::handleMulOverflow(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  executor.terminateStateOnError(state, "overflow on multiplication",
                                 StateTerminationType::Overflow);
}

void SpecialFunctionHandler::handleDivRemOverflow(
    ExecutionState &state, KInstruction *target,
    const std::vector<Cell> &arguments) {
  executor.terminateStateOnError(state, "overflow on division or remainder",
                                 StateTerminationType::Overflow);
}

void SpecialFunctionHandler::handlePthreadCreate(ExecutionState &state,
                                                 KInstruction *target,
                                                 const std::vector<Cell> &arguments) {
  executor.terminateStateOnExecError(state,
        "Call to pthread_create.");
}

void SpecialFunctionHandler::handlePthreadJoin(ExecutionState &state,
                                               KInstruction *target,
                                               const std::vector<Cell> &arguments) {
  executor.terminateStateOnExecError(state,
        "Call to pthread_join.");
}

void SpecialFunctionHandler::handleUnsupportedPthread(ExecutionState &state,
                                                      KInstruction *target,
                                                      const std::vector<Cell> &arguments) {
  executor.terminateStateOnExecError(state,
        "unsupported pthread API.");
}

size_t SpecialFunctionHandler::parseArgumentForScanf(ExecutionState& state, const Cell &argument) {
  size_t argCount = 0;
  const auto& formatArg = argument;
  if (dyn_cast<ConstantExpr>(formatArg.getValue())) {
    std::string format = readStringAtAddress(state, formatArg);
    format.erase(remove_if(format.begin(), format.end(), isspace), format.end());
    std::string delimiter = "%";

    size_t last = 0, next = 0;
    if ((format)[0] != '%') {
      klee_warning("(f)scanf: format is wrong, undefined behavior!");
    }

    while ((next = format.find(delimiter, last)) != std::string::npos) {
      last = next + 1;
      if (format[last] != '*') {
        argCount++;
      }
    }
  }
  return argCount ? argCount : SIZE_MAX;
}

void SpecialFunctionHandler::handleScanf(ExecutionState &state,
                                         KInstruction *target,
                                         const std::vector<Cell> &arguments) {
  size_t size = arguments.size();
  if (size < 2) {
    executor.terminateStateOnExecError(state, "scanf: unsupported function model");
    return;
  }

  size_t formatArgCount = parseArgumentForScanf(state, arguments[0]);

  if (formatArgCount == SIZE_MAX) { // FIXME: Should we report this UB as an error?
    klee_warning("scanf: unsupported format specified, might result in undefined behavior!");
    formatArgCount = 0;
  }

  size_t realizedArgs = 0;
  for (size_t i = 1; i < size; ++i) { /* first two arguments are file and format */
    if (realizedArgs == formatArgCount) {
      break;
    }
    Executor::ExactResolutionList rl;
    executor.resolveExact(state, arguments[i], rl, "_scanf");

    for (const auto& it : rl) {
      const MemoryObject *mo = it.first.first;
      /* FIXME: Length of the nondet value should be reduced */
      executor.executeMakeSymbolic(state, mo, "_scanf_"+std::to_string(mo->id));
      ++realizedArgs;
    }
  }

  auto expr = ConstantExpr::create(realizedArgs, Expr::Int64);
  executor.bindLocal(target, state, expr);
}


void SpecialFunctionHandler::handleFscanf(ExecutionState &state,
                                         KInstruction *target,
                                         const std::vector<Cell> &arguments) {
  size_t size = arguments.size();
  if (size < 3) {
    executor.terminateStateOnExecError(state, "fscanf: unsupported function model");
    return;
  }

  size_t formatArgCount = parseArgumentForScanf(state, arguments[1]);

  if (formatArgCount == SIZE_MAX) { // FIXME: Should we report this UB as an error?
    klee_warning("fscanf: unsupported format specified, might result in undefined behavior!");
    formatArgCount = 0;
  }

  size_t realizedArgs = 0;
  for (unsigned i = 2; i < size; ++i) { /* first two arguments are file and format */
    if (realizedArgs == formatArgCount) {
      break;
    }
    Executor::ExactResolutionList rl;
    executor.resolveExact(state, arguments[i], rl, "_fscanf");

    for (const auto& it : rl) {
      const MemoryObject *mo = it.first.first;
      /* FIXME: Length of the nondet value should be reduced */
      executor.executeMakeSymbolic(state, mo, "_fscanf_"+std::to_string(mo->id));
      ++realizedArgs;
    }
  }

  auto expr = ConstantExpr::create(realizedArgs, Expr::Int64);
  executor.bindLocal(target, state, expr);
}
