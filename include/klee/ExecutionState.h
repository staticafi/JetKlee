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

#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Internal/ADT/TreeStream.h"
#include "klee/Internal/System/Time.h"
#include "klee/MergeHandler.h"

// FIXME: We do not want to be exposing these? :(
#include "../../lib/Core/AddressSpace.h"
#include "klee/Internal/Module/KInstIterator.h"
#include "klee/ConcreteValue.h"

#include <map>
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

///
// Shared pointer with copy-on-write support
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

/// @brief ExecutionState representing a path under exploration
class ExecutionState {
public:
  typedef std::vector<StackFrame> stack_ty;

private:
  // unsupported, use copy constructor
  ExecutionState &operator=(const ExecutionState &);

public:
  // Execution - Control Flow specific

  struct NondetValue {
      KValue value;
      // info about name and where the object was created...
      NondetValue() = default;

      NondetValue(ref<Expr> e, const std::string& n) : value(e), name(n) {}
      NondetValue(const KValue& val, const std::string& n) : value(val), name(n) {}

      NondetValue(ref<Expr> e, KInstruction *ki, const std::string& n)
      : value(e), kinstruction(ki), name(n) {}
      NondetValue(const KValue& val, KInstruction *ki, const std::string& n)
      : value(val), kinstruction(ki), name(n) {}

      NondetValue(ref<Expr> e, bool sgned, const std::string& n)
      : value(e), isSigned(sgned), name(n) {}
      NondetValue(const KValue& val, bool sgned, const std::string& n)
      : value(val), isSigned(sgned), name(n) {}

      NondetValue(ref<Expr> e, bool sgned, KInstruction *ki, const std::string& n)
      : value(e), isSigned(sgned), kinstruction(ki), name(n) {}
      NondetValue(const KValue& val, bool sgned, KInstruction *ki, const std::string& n)
      : value(val), isSigned(sgned), kinstruction(ki), name(n) {}

      bool isSigned{false};
      KInstruction *kinstruction{nullptr};
      const std::string name{};

      // when an instruction that creates a nodet value is called
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
  unsigned incomingBBIndex;

  // Overall state of the state - Data specific

  /// @brief Address space used by this state (e.g. Global and Heap)
  AddressSpace addressSpace;

  /// @brief Constraints collected so far
  ConstraintManager constraints;

  /// Statistics and information

  /// @brief Costs for all queries issued for this state, in seconds
  mutable time::Span queryCost;

  /// @brief Weight assigned for importance of this state.  Can be
  /// used for searchers to decide what paths to explore
  double weight;

  /// @brief Exploration depth, i.e., number of times KLEE branched for this state
  unsigned depth;

  /// @brief History of complete path: represents branches taken to
  /// reach/create this state (both concrete and symbolic)
  TreeOStream pathOS;

  /// @brief History of symbolic path: represents symbolic branches
  /// taken to reach/create this state
  TreeOStream symPathOS;

  /// @brief Counts how many instructions were executed since the last new
  /// instruction was covered.
  unsigned instsSinceCovNew;

  /// @brief Whether a new instruction was covered in this state
  bool coveredNew;

  /// @brief Disables forking for this state. Set by user code
  bool forkDisabled;

  /// @brief Set containing which lines in which files are covered by this state
  std::map<const std::string *, std::set<unsigned> > coveredLines;

  /// @brief Pointer to the process tree of the current state
  PTreeNode *ptreeNode;

  /// @brief Ordered list of symbolics: used to generate test cases.
  //
  // FIXME: Move to a shared list structure (not critical).
  std::vector<std::pair<const MemoryObject *, const Array *> > symbolics;

  /// @brief Set of used array names for this state.  Used to avoid collisions.
  std::set<std::string> arrayNames;

  // The objects handling the klee_open_merge calls this state ran through
  std::vector<ref<MergeHandler> > openMergeStack;

  // The numbers of times this state has run through Executor::stepInstruction
  std::uint64_t steppedInstructions;

  NondetValue& addNondetValue(const KValue& val, bool isSigned, const std::string& name);

  bool includeConstantConstraints = false;

private:
  ExecutionState() : ptreeNode(0) {}

public:
  ExecutionState(KFunction *kf, bool includeConstantConstraints);

  // XXX total hack, just used to make a state so solver can
  // use on structure
  ExecutionState(const std::vector<ref<Expr> > &assumptions);

  ExecutionState(const ExecutionState &state);

  ~ExecutionState();

  ExecutionState *branch();

  void pushFrame(KInstIterator caller, KFunction *kf);
  void popFrame();
  void removeAlloca(const MemoryObject *mo);

  void addSymbolic(const MemoryObject *mo, const Array *array);
  void addConstraint(ref<Expr> e) { constraints.addConstraint(e); }

  bool merge(const ExecutionState &b);
  void dumpStack(llvm::raw_ostream &out) const;
};
}

#endif /* KLEE_EXECUTIONSTATE_H */
