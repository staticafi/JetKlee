//===-- ExecutionState.h ----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTIONSTATE_H
#define KLEE_EXECUTIONSTATE_H

#include "AddressSpace.h"
#include "MergeHandler.h"

#include "klee/ADT/ImmutableSet.h"
#include "klee/ADT/TreeStream.h"
#include "klee/Core/ConcreteValue.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/KInstIterator.h"
#include "klee/Solver/Solver.h"
#include "klee/System/Time.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

namespace klee {
class Array;
class CallPathNode;
struct Cell;
struct KFunction;
struct KInstruction;
class MemoryObject;
class PTreeNode;
struct InstructionInfo;

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const MemoryMap &mm);

struct StackFrame {
  KInstIterator caller;
  KFunction *kf;
  CallPathNode *callPathNode;

  std::vector<const MemoryObject *> allocas;
  Cell *locals;

  /// Minimum distance to an uncovered instruction once the function
  /// returns. This is not a good place for this but is used to
  /// quickly compute the context sensitive minimum distance to an
  /// uncovered instruction. This value is updated by the StatsTracker
  /// periodically.
  unsigned minDistToUncoveredOnReturn;

  // For vararg functions: arguments not passed via parameter are
  // stored (packed tightly) in a local (alloca) memory object. This
  // is set up to match the way the front-end generates vaarg code (it
  // does not pass vaarg through as expected). VACopy is lowered inside
  // of intrinsic lowering.
  MemoryObject *varargs;

  StackFrame(KInstIterator caller, KFunction *kf);
  StackFrame(const StackFrame &s);
  ~StackFrame();
};

/// Shared pointer with copy-on-write support
template <typename T>
class cow_shared_ptr {
  std::shared_ptr<T> ptr{nullptr};
  // am I the owner of the copy?
  bool owner{false};

public:
  cow_shared_ptr() = default;
  cow_shared_ptr(T *p) : ptr(p) {}
  cow_shared_ptr(cow_shared_ptr&&) = delete;
  cow_shared_ptr(const cow_shared_ptr& rhs)
      : ptr(rhs.ptr), owner(false) {}

  const T *get() const { return ptr.get(); }

  T *getWriteable() {
    if (owner)
      return ptr.get();
    // create a copy of the object and claim the ownership
    if (ptr) {
      ptr.reset(new T(*get()));
    } else {
      ptr.reset(new T());
    }
    owner = true;
    return ptr.get();
  }
};

/// Contains information related to unwinding (Itanium ABI/2-Phase unwinding)
class UnwindingInformation {
public:
  enum class Kind {
    SearchPhase, // first phase
    CleanupPhase // second phase
  };

private:
  const Kind kind;

public:
  // _Unwind_Exception* of the thrown exception, used in both phases
  ref<ConstantExpr> exceptionObject;

  Kind getKind() const { return kind; }

  explicit UnwindingInformation(ref<ConstantExpr> exceptionObject, Kind k)
      : kind(k), exceptionObject(exceptionObject) {}
  virtual ~UnwindingInformation() = default;

  virtual std::unique_ptr<UnwindingInformation> clone() const = 0;
};

struct SearchPhaseUnwindingInformation : public UnwindingInformation {
  // Keeps track of the stack index we have so far unwound to.
  std::size_t unwindingProgress;

  // MemoryObject that contains a serialized version of the last executed
  // landingpad, so we can clean it up after the personality fn returns.
  MemoryObject *serializedLandingpad = nullptr;

  SearchPhaseUnwindingInformation(ref<ConstantExpr> exceptionObject,
                                  std::size_t const unwindingProgress)
      : UnwindingInformation(exceptionObject,
                             UnwindingInformation::Kind::SearchPhase),
        unwindingProgress(unwindingProgress) {}

  std::unique_ptr<UnwindingInformation> clone() const {
    return std::make_unique<SearchPhaseUnwindingInformation>(*this);
  }

  static bool classof(const UnwindingInformation *u) {
    return u->getKind() == UnwindingInformation::Kind::SearchPhase;
  }
};

struct CleanupPhaseUnwindingInformation : public UnwindingInformation {
  // Phase 1 will try to find a catching landingpad.
  // Phase 2 will unwind up to this landingpad or return from
  // _Unwind_RaiseException if none was found.

  // The selector value of the catching landingpad that was found
  // during the search phase.
  ref<ConstantExpr> selectorValue;

  // Used to know when we have to stop unwinding and to
  // ensure that unwinding stops at the frame for which
  // we first found a handler in the search phase.
  const std::size_t catchingStackIndex;

  CleanupPhaseUnwindingInformation(ref<ConstantExpr> exceptionObject,
                                   ref<ConstantExpr> selectorValue,
                                   const std::size_t catchingStackIndex)
      : UnwindingInformation(exceptionObject,
                             UnwindingInformation::Kind::CleanupPhase),
        selectorValue(selectorValue),
        catchingStackIndex(catchingStackIndex) {}

  std::unique_ptr<UnwindingInformation> clone() const {
    return std::make_unique<CleanupPhaseUnwindingInformation>(*this);
  }

  static bool classof(const UnwindingInformation *u) {
    return u->getKind() == UnwindingInformation::Kind::CleanupPhase;
  }
};

/// @brief ExecutionState representing a path under exploration
class ExecutionState {
#ifdef KLEE_UNITTEST
public:
#else
private:
#endif
  // copy ctor
  ExecutionState(const ExecutionState &state);

public:
  using stack_ty = std::vector<StackFrame>;

  // Execution - Control Flow specific

  struct NondetValue {
    KValue value;
    // info about name and where the object was created...
    NondetValue() = default;

    NondetValue(ref<Expr> e, const std::string& n) : value(e), name(n) {}
    NondetValue(const KValue& val, const std::string& n) : value(val), name(n) {}

    NondetValue(const ref<Expr> &e, KInstruction *ki, const std::string& n)
        : value(e), kinstruction(ki), name(n) {}
    NondetValue(const KValue& val, KInstruction *ki, const std::string& n)
        : value(val), kinstruction(ki), name(n) {}

    NondetValue(const ref<Expr> &e, bool sgned, const std::string& n)
        : value(e), isSigned(sgned), name(n) {}
    NondetValue(const KValue& val, bool sgned, const std::string& n)
        : value(val), isSigned(sgned), name(n) {}

    NondetValue(const ref<Expr> &e, bool sgned, KInstruction *ki,
                const std::string& n)
        : value(e), isSigned(sgned), kinstruction(ki), name(n) {}
    NondetValue(const KValue& val, bool sgned, KInstruction *ki, const std::string& n)
        : value(val), isSigned(sgned), kinstruction(ki), name(n) {}

    bool isSigned{false};
    KInstruction *kinstruction{nullptr};
    const std::string name{};
    // when an instruction that creates a nondet value is called
    // several times, we can assign a sequential number to each
    // of the values here
    //size_t seqNum{0};

    // concrete value
    //MaybeConcreteValue concreteValue;
    //
    //bool hasConcreteValue() const { return concreteValue.hasValue(); }
  };

  // FIXME: wouldn't unique_ptr be more efficient (no ref<> copying)
  std::vector<NondetValue> nondetValues;
  // FIXME: this is a hack to be able to generate termination witnesses for SV-COMP
  llvm::Instruction *lastLoopHead{nullptr};
  size_t lastLoopHeadId{0};
  // so that we do not need to unwind the stack
  llvm::Instruction *lastLoopCheck{nullptr};
  llvm::Instruction *lastLoopFail{nullptr};

  /// @brief Pointer to instruction to be executed after the current
  /// instruction
  KInstIterator pc;

  /// @brief Pointer to instruction which is currently executed
  KInstIterator prevPC;

  /// @brief Stack representing the current instruction stream
  stack_ty stack;

  /// @brief Remember from which Basic Block control flow arrived
  /// (i.e. to select the right phi values)
  std::uint32_t incomingBBIndex;

  // Overall state of the state - Data specific

  /// @brief Exploration depth, i.e., number of times KLEE branched for this state
  std::uint32_t depth = 0;

  /// @brief Address space used by this state (e.g. Global and Heap)
  AddressSpace addressSpace;

  /// @brief Constraints collected so far
  ConstraintSet constraints;

  /// Statistics and information

  /// @brief Metadata utilized and collected by solvers for this state
  mutable SolverQueryMetaData queryMetaData;

  /// @brief History of complete path: represents branches taken to
  /// reach/create this state (both concrete and symbolic)
  TreeOStream pathOS;

  /// @brief History of symbolic path: represents symbolic branches
  /// taken to reach/create this state
  TreeOStream symPathOS;

  /// @brief Set containing which lines in which files are covered by this state
  std::map<const std::string *, std::set<std::uint32_t>> coveredLines;

  /// @brief Pointer to the process tree of the current state
  /// Copies of ExecutionState should not copy ptreeNode
  PTreeNode *ptreeNode = nullptr;

  /// @brief Ordered list of symbolics: used to generate test cases.
  //
  // FIXME: Move to a shared list structure (not critical).
  std::vector<std::pair<ref<const MemoryObject>, const Array *>> symbolics;

  /// @brief A set of boolean expressions
  /// the user has requested be true of a counterexample.
  ImmutableSet<ref<Expr>> cexPreferences;

  /// @brief Set of used array names for this state.  Used to avoid collisions.
  std::set<std::string> arrayNames;

  /// @brief The objects handling the klee_open_merge calls this state ran through
  std::vector<ref<MergeHandler>> openMergeStack;

  /// @brief The numbers of times this state has run through Executor::stepInstruction
  std::uint64_t steppedInstructions = 0;

  /// @brief Counts how many instructions were executed since the last new
  /// instruction was covered.
  std::uint32_t instsSinceCovNew = 0;

  /// @brief Keep track of unwinding state while unwinding, otherwise empty
  std::unique_ptr<UnwindingInformation> unwindingInformation;

  /// @brief the global state counter
  static std::uint32_t nextID;

  /// @brief the state id
  std::uint32_t id = 0;

  /// @brief Whether a new instruction was covered in this state
  bool coveredNew = false;

  /// @brief Disables forking for this state. Set by user code
  bool forkDisabled = false;

public:
#ifdef KLEE_UNITTEST
  // provide this function only in the context of unittests
  ExecutionState() = default;
#endif
  // only to create the initial state
  explicit ExecutionState(KFunction *kf);
  // no copy assignment, use copy constructor
  ExecutionState &operator=(const ExecutionState &) = delete;
  // no move ctor
  ExecutionState(ExecutionState &&) noexcept = delete;
  // no move assignment
  ExecutionState& operator=(ExecutionState &&) noexcept = delete;
  // dtor
  ~ExecutionState();

  ExecutionState *branch();

  void pushFrame(KInstIterator caller, KFunction *kf);
  void popFrame();
  void removeAlloca(const MemoryObject *mo);

  void addSymbolic(const MemoryObject *mo, const Array *array);

  void addConstraint(ref<Expr> e);
  void addCexPreference(const ref<Expr> &cond);

  bool merge(const ExecutionState &b);
  void dumpStack(llvm::raw_ostream &out) const;

  std::uint32_t getID() const { return id; };
  void setID() { id = nextID++; };

  NondetValue& addNondetValue(const KValue &expr, bool isSigned,
                              const std::string& name);

  std::tuple<std::string, unsigned, unsigned> getErrorLocation() const;
};

struct ExecutionStateIDCompare {
  bool operator()(const ExecutionState *a, const ExecutionState *b) const {
    return a->getID() < b->getID();
  }
};
}

#endif /* KLEE_EXECUTIONSTATE_H */
