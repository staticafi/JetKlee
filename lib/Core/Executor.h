//===-- Executor.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Class to perform actual execution, hides implementation details from external
// interpreter.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTOR_H
#define KLEE_EXECUTOR_H

#include "ExecutionState.h"
#include "UserSearcher.h"

#include "klee/ADT/RNG.h"
#include "klee/Core/BranchTypes.h"
#include "klee/Core/Interpreter.h"
#include "klee/Core/TerminationTypes.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ArrayExprOptimizer.h"
#include "klee/Module/Cell.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/System/Time.h"

#include "llvm/ADT/Twine.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct KTest;

namespace llvm {
  class BasicBlock;
  class BranchInst;
  class CallInst;
  class LandingPadInst;
  class Constant;
  class ConstantExpr;
  class Function;
  class GlobalValue;
  class Instruction;
  class LLVMContext;
  class DataLayout;
  class Twine;
  class Value;
}

namespace klee {  
  class Array;
  struct Cell;
  class ExecutionState;
  class ExternalDispatcher;
  class Expr;
  class InstructionInfoTable;
  struct KFunction;
  struct KInstruction;
  class KInstIterator;
  class KModule;
  class MemoryManager;
  class MemoryObject;
  class ObjectState;
  class PTree;
  class Searcher;
  class SeedInfo;
  class SpecialFunctionHandler;
  struct StackFrame;
  class StatsTracker;
  class TimingSolver;
  class TreeStreamWriter;
  class MergeHandler;
  class MergingSearcher;
  template<class T> class ref;



  /// \todo Add a context object to keep track of data only live
  /// during an instruction step. Should contain addedStates,
  /// removedStates, and haltExecution, among others.

class Executor : public Interpreter {
  friend class OwningSearcher;
  friend class WeightedRandomSearcher;
  friend class SpecialFunctionHandler;
  friend class StatsTracker;
  friend class MergeHandler;
  friend klee::Searcher *klee::constructUserSearcher(Executor &executor);

public:
  typedef std::pair<ExecutionState*,ExecutionState*> StatePair;

  /// The random number generator.
  RNG theRNG;

private:
  std::unique_ptr<KModule> kmodule;
  InterpreterHandler *interpreterHandler;
  Searcher *searcher;

  ExternalDispatcher *externalDispatcher;
  TimingSolver *solver;
  MemoryManager *memory;
  std::set<ExecutionState*, ExecutionStateIDCompare> states;
  StatsTracker *statsTracker;
  TreeStreamWriter *pathWriter, *symPathWriter;
  SpecialFunctionHandler *specialFunctionHandler;
  TimerGroup timers;
  std::unique_ptr<PTree> processTree;
  std::tuple<std::string, unsigned, unsigned> errorLoc;

  /// Used to track states that have been added during the current
  /// instructions step. 
  /// \invariant \ref addedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::vector<ExecutionState *> addedStates;
  /// Used to track states that have been removed during the current
  /// instructions step. 
  /// \invariant \ref removedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::vector<ExecutionState *> removedStates;

  /// When non-empty the Executor is running in "seed" mode. The
  /// states in this map will be executed in an arbitrary order
  /// (outside the normal search interface) until they terminate. When
  /// the states reach a symbolic branch then either direction that
  /// satisfies one or more seeds will be added to this map. What
  /// happens with other states (that don't satisfy the seeds) depends
  /// on as-yet-to-be-determined flags.
  std::map<ExecutionState*, std::vector<SeedInfo> > seedMap;
  
  /// Map of globals to their representative memory object.
  std::map<const llvm::GlobalValue*, MemoryObject*> globalObjects;

  /// Map of globals to their bound address. This also includes
  /// globals that have no representative object (i.e. functions).
  std::map<const llvm::GlobalValue*, KValue> globalAddresses;

  /// Map of legal function addresses to the corresponding Function.
  /// Used to validate and dereference function pointers.
  std::unordered_map<std::uint64_t, llvm::Function*> legalFunctions;

  /// When non-null the bindings that will be used for calls to
  /// klee_make_symbolic in order replay.
  const struct KTest *replayKTest;

  // values for replaying nondet values -- for each object
  // with a given name, line, col and sequence number we have
  // a concrete value -- this is a sequence of the values
  // as they appear on the path being replayed
  std::vector<std::tuple<std::string, unsigned, unsigned,
                         ConcreteValue>> replayNondet;

  /// When non-null a list of branch decisions to be used for replay.
  const std::vector<bool> *replayPath;

  /// The index into the current \ref replayKTest or \ref replayPath
  /// object.
  unsigned replayPosition;

  /// When non-null a list of "seed" inputs which will be used to
  /// drive execution.
  const std::vector<struct KTest *> *usingSeeds;  

  /// Disables forking, instead a random path is chosen. Enabled as
  /// needed to control memory usage. \see fork()
  bool atMemoryLimit;

  /// Disables forking, set by client. \see setInhibitForking()
  bool inhibitForking;

  /// Signals the executor to halt execution at the next instruction
  /// step.
  bool haltExecution;  

  /// Whether implied-value concretization is enabled. Currently
  /// false, it is buggy (it needs to validate its writes).
  bool ivcEnabled;

  /// The maximum time to allow for a single core solver query.
  /// (e.g. for a single STP query)
  time::Span coreSolverTimeout;

  /// Maximum time to allow for a single instruction.
  time::Span maxInstructionTime;

  /// Assumes ownership of the created array objects
  ArrayCache arrayCache;

  /// File to print executed instructions to
  std::unique_ptr<llvm::raw_ostream> debugInstFile;

  // @brief Buffer used by logBuffer
  std::string debugBufferString;

  // @brief buffer to store logs before flushing to file
  llvm::raw_string_ostream debugLogBuffer;

  /// Optimizes expressions
  ExprOptimizer optimizer;

  /// Points to the merging searcher of the searcher chain,
  /// `nullptr` if merging is disabled
  MergingSearcher *mergingSearcher = nullptr;

  /// Typeids used during exception handling
  std::vector<ref<Expr>> eh_typeids;

  /// Return the typeid corresponding to a certain `type_info`
  ref<ConstantExpr> getEhTypeidFor(ref<Expr> type_info);

  llvm::Function* getTargetFunction(llvm::Value *calledVal,
                                    ExecutionState &state);

  void executeInstruction(ExecutionState &state, KInstruction *ki);

  void run(ExecutionState &initialState);

  // Given a concrete object in our [klee's] address space, add it to 
  // objects checked code can reference.
  MemoryObject *addExternalObject(ExecutionState &state, void *addr, 
                                  unsigned size, bool isReadOnly,
                                  uint64_t specialSegment = 0);

  void initializeGlobalAlias(const llvm::Constant *c);
  void initializeGlobalObject(ExecutionState &state, ObjectState *os, 
			      const llvm::Constant *c,
			      unsigned offset);
  void initializeGlobals(ExecutionState &state, bool isEntryFunctionMain);
  void allocateGlobalObjects(ExecutionState &state, bool isEntryFunctionMain);
  void initializeGlobalAliases();
  void initializeGlobalObjects(ExecutionState &state, bool isEntryFunctionMain);

  void stepInstruction(ExecutionState &state);
  void updateStates(ExecutionState *current);
  void transferToBasicBlock(llvm::BasicBlock *dst, 
			    llvm::BasicBlock *src,
			    ExecutionState &state);

  void callExternalFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            const std::vector<Cell> &arguments);

  ObjectState *bindObjectInState(ExecutionState &state, const MemoryObject *mo,
                                 bool isLocal, const Array *array = 0);

  /// Resolve a pointer to the memory objects it could point to the
  /// start of, forking execution when necessary and generating errors
  /// for pointers to invalid locations (either out of bounds or
  /// address inside the middle of objects).
  ///
  /// \param results[out] A list of ((MemoryObject,ObjectState),
  /// state) pairs for each object the given address can point to the
  /// beginning of.
  typedef std::vector< std::pair<std::pair<const MemoryObject*, const ObjectState*>, 
                                 ExecutionState*> > ExactResolutionList;
  void resolveExact(ExecutionState &state,
                    const KValue &address,
                    ExactResolutionList &results,
                    const std::string &name);

  /// Allocate and bind a new object in a particular state. NOTE: This
  /// function may fork.
  ///
  /// \param isLocal Flag to indicate if the object should be
  /// automatically deallocated on function return (this also makes it
  /// illegal to free directly).
  ///
  /// \param target Value at which to bind the base address of the new
  /// object.
  ///
  /// \param reallocFrom If non-zero and the allocation succeeds,
  /// initialize the new object from the given one and unbind it when
  /// done (realloc semantics). The initialized bytes will be the
  /// minimum of the size of the old and new objects, with remaining
  /// bytes initialized as specified by zeroMemory.
  ///
  /// \param allocationAlignment If non-zero, the given alignment is
  /// used. Otherwise, the alignment is deduced via
  /// Executor::getAllocationAlignment
  MemoryObject* executeAlloc(ExecutionState &state,
                             ref<Expr> size,
                             bool isLocal,
                             KInstruction *target,
                             bool zeroMemory=false,
                             const ObjectState *reallocFrom=nullptr,
                             size_t allocationAlignment=0);

  ref<Expr> getSizeForAlloca(ExecutionState& state, KInstruction *ki) const;

  void executeLifetimeIntrinsic(ExecutionState &state,
                                KInstruction *ki,
                                const std::vector<Cell> &arguments,
                                bool isEnd);

  void executeLifetimeIntrinsic(ExecutionState &state,
                                KInstruction *ki,
                                KInstruction *allocSite,
                                const KValue& address,
                                bool isEnd);

  /// Free the given address with checking for errors. If target is
  /// given it will be bound to 0 in the resulting states (this is a
  /// convenience for realloc). Note that this function can cause the
  /// state to fork and that \ref state cannot be safely accessed
  /// afterwards.
  void executeFree(ExecutionState &state,
                   const KValue &address,
                   KInstruction *target = 0);

  /// Serialize a landingpad instruction so it can be handled by the
  /// libcxxabi-runtime
  MemoryObject *serializeLandingpad(ExecutionState &state,
                                    const llvm::LandingPadInst &lpi,
                                    bool &stateTerminated);

  /// Unwind the given state until it hits a landingpad. This is used
  /// for exception handling.
  void unwindToNextLandingpad(ExecutionState &state);

  void executeCall(ExecutionState &state, 
                   KInstruction *ki,
                   llvm::Function *f,
                   const std::vector<Cell> &arguments);

  void executeMemoryRead(ExecutionState &state,
                         const KValue &address,
                         KInstruction *target);
  void executeMemoryWrite(ExecutionState &state,
                          const KValue &address,
                          const KValue &value);
  // do address resolution / object binding / out of bounds checking
  // and perform the operation
  void executeMemoryOperation(ExecutionState &state,
                              bool isWrite,
                              KValue address,
                              KValue value, /* undef if read */
                              KInstruction *target /* undef if write */);

  void executeMakeSymbolic(ExecutionState &state, const MemoryObject *mo,
                           const std::string &name);

  void executeMakeConcrete(ExecutionState &state,
                           const MemoryObject *mo,
                           const std::vector<unsigned char>& data);

  /// Create a new state where each input condition has been added as
  /// a constraint and return the results. The input state is included
  /// as one of the results. Note that the output vector may include
  /// NULL pointers for states which were unable to be created.
  void branch(ExecutionState &state, const std::vector<ref<Expr>> &conditions,
              std::vector<ExecutionState *> &result, BranchType reason);

  /// Fork current and return states in which condition holds / does
  /// not hold, respectively. One of the states is necessarily the
  /// current state, and one of the states may be null.
  StatePair fork(ExecutionState &current, ref<Expr> condition, bool isInternal,
                 BranchType reason);

  // If the MaxStatic*Pct limits have been reached, concretize the condition and
  // return it. Otherwise, return the unmodified condition.
  ref<Expr> maxStaticPctChecks(ExecutionState &current, ref<Expr> condition);

  /// Add the given (boolean) condition as a constraint on state. This
  /// function is a wrapper around the state's addConstraint function
  /// which also manages propagation of implied values,
  /// validity checks, and seed patching.
  void addConstraint(ExecutionState &state, ref<Expr> condition);

  // Called on [for now] concrete reads, replaces constant with a symbolic
  // Used for testing.
  ref<Expr> replaceReadWithSymbolic(ExecutionState &state, ref<Expr> e);


  const Array* CreateArrayWithName(ExecutionState &state,
                                   const Expr::Width& width,
                                   const std::string& name);

  KValue createNondetValue(ExecutionState &state,
                           unsigned size, bool isSigned,
                           KInstruction *instr,
                           const std::string &name,
                           bool isPointer = false);

  const Cell& eval(KInstruction *ki, unsigned index, 
                   ExecutionState &state) const;

  Cell& getArgumentCell(ExecutionState &state,
                        KFunction *kf,
                        unsigned index) {
    return state.stack.back().locals[kf->getArgRegister(index)];
  }

  Cell& getDestCell(ExecutionState &state,
                    KInstruction *target) {
    return state.stack.back().locals[target->dest];
  }

  void bindLocal(KInstruction *target,
                 ExecutionState &state,
                 const KValue &value);
  void bindArgument(KFunction *kf,
                    unsigned index,
                    ExecutionState &state,
                    const KValue &value);

  /// Evaluates an LLVM constant expression.  The optional argument ki
  /// is the instruction where this constant was encountered, or NULL
  /// if not applicable/unavailable.
  KValue evalConstantExpr(const llvm::ConstantExpr *c,
                          const KInstruction *ki = NULL);

  /// Evaluates an LLVM constant.  The optional argument ki is the
  /// instruction where this constant was encountered, or NULL if
  /// not applicable/unavailable.
  KValue evalConstant(const llvm::Constant *c,
                      const KInstruction *ki = NULL);

  /// Return a unique constant value for the given expression in the
  /// given state, if it has one (i.e. it provably only has a single
  /// value). Otherwise return the original expression.
  ref<Expr> toUnique(const ExecutionState &state, const ref<Expr> &e);

  /// Return a constant value for the given expression, forcing it to
  /// be constant in the given state by adding a constraint if
  /// necessary. Note that this function breaks completeness and
  /// should generally be avoided.
  ///
  /// \param purpose An identify string to printed in case of concretization.
  ref<klee::ConstantExpr> toConstant(ExecutionState &state, ref<Expr> e, 
                                     const char *purpose);

  /// Bind a constant value for e to the given target. NOTE: This
  /// function may fork state if the state has multiple seeds.
  void executeGetValue(ExecutionState &state, const KValue& e, KInstruction *target);

  /// Get textual information regarding a memory address.
  std::string getKValueInfo(ExecutionState &state, const KValue &address) const;

  // Determines the \param lastInstruction of the \param state which is not KLEE
  // internal and returns its InstructionInfo
  const InstructionInfo & getLastNonKleeInternalInstruction(const ExecutionState &state,
      llvm::Instruction** lastInstruction);

  /// Remove state from queue and delete state
  void terminateState(ExecutionState &state);

  /// Call exit handler and terminate state normally
  /// (end of execution path)
  void terminateStateOnExit(ExecutionState &state);

  /// Call exit handler and terminate state early
  /// (e.g. due to state merging or memory pressure)
  void terminateStateEarly(ExecutionState &state, const llvm::Twine &message,
                           StateTerminationType terminationType);

  /// Call error handler and terminate state in case of program errors
  /// (e.g. free()ing globals, out-of-bound accesses)
  void terminateStateOnError(ExecutionState &state, const llvm::Twine &message,
                             StateTerminationType terminationType,
                             const llvm::Twine &longMessage = "",
                             const char *suffix = nullptr);

  /// Call error handler and terminate state in case of execution errors
  /// (things that should not be possible, like illegal instruction or
  /// unlowered intrinsic, or unsupported intrinsics, like inline assembly)
  void terminateStateOnExecError(ExecutionState &state,
                                 const llvm::Twine &message,
                                 const llvm::Twine &info = "");

  /// Call error handler and terminate state in case of solver errors
  /// (solver error or timeout)
  void terminateStateOnSolverError(ExecutionState &state,
                                   const llvm::Twine &message);

  /// Call error handler and terminate state for user errors
  /// (e.g. wrong usage of klee.h API)
  void terminateStateOnUserError(ExecutionState &state,
                                 const llvm::Twine &message);

  /// bindModuleConstants - Initialize the module constant table.
  void bindModuleConstants();

  template <typename TypeIt>
  void computeOffsetsSeqTy(KGEPInstruction *kgepi,
                           ref<ConstantExpr> &constantOffset, uint64_t index,
                           const TypeIt it);

  template <typename TypeIt>
  void computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie);

  /// bindInstructionConstants - Initialize any necessary per instruction
  /// constant values.
  void bindInstructionConstants(KInstruction *KI);

  void doImpliedValueConcretization(ExecutionState &state,
                                    ref<Expr> e,
                                    ref<ConstantExpr> value);

  bool getReachableMemoryObjects(ExecutionState &state,
                                 std::set<const MemoryObject *>&);

  void reportError(const llvm::Twine &message, const ExecutionState &state,
                   const llvm::Twine &info, const char *suffix,
                   enum StateTerminationType terminationType);

  /// check memory usage and terminate states when over threshold of -max-memory + 100MB
  /// \return true if below threshold, false otherwise (states were terminated)
  bool checkMemoryUsage();

  /// check if branching/forking is allowed
  bool branchingPermitted(const ExecutionState &state) const;

  void printDebugInstructions(ExecutionState &state);
  void doDumpStates();

  /// Only for debug purposes; enable via debugger or klee-control
  static void dumpState(std::unique_ptr<llvm::raw_fd_ostream>& os, ExecutionState* es);
  void dumpStates();
  void dumpPTree();

public:
  Executor(llvm::LLVMContext &ctx, const InterpreterOptions &opts,
      InterpreterHandler *ie);
  virtual ~Executor();

  const InterpreterHandler& getHandler() {
    return *interpreterHandler;
  }

  void setPathWriter(TreeStreamWriter *tsw) override { pathWriter = tsw; }

  void setSymbolicPathWriter(TreeStreamWriter *tsw) override {
    symPathWriter = tsw;
  }

  void setReplayKTest(const struct KTest *out) override {
    assert(!replayPath && "cannot replay both buffer and path");
    replayKTest = out;
    replayPosition = 0;
  }

  void setReplayPath(const std::vector<bool> *path) override {
    assert(!replayKTest && "cannot replay both buffer and path");
    replayPath = path;
    replayPosition = 0;
  }

  void setReplayNondet(const struct KTest *out) override;

  llvm::Module *setModule(std::vector<std::unique_ptr<llvm::Module>> &modules,
                          const ModuleOptions &opts) override;

  void useSeeds(const std::vector<struct KTest *> *seeds) override {
    usingSeeds = seeds;
  }

  void runFunctionAsMain(llvm::Function *f, int argc, char **argv,
                         char **envp) override;

  /*** Runtime options ***/

  void setHaltExecution(bool value) override { haltExecution = value; }

  void setInhibitForking(bool value) override { inhibitForking = value; }

  void prepareForEarlyExit() override;

  /*** State accessor methods ***/

  unsigned getPathStreamID(const ExecutionState &state) override;

  unsigned getSymbolicPathStreamID(const ExecutionState &state) override;

  void getConstraintLog(const ExecutionState &state, std::string &res,
                        Interpreter::LogType logFormat =
                            Interpreter::STP) override;

  bool getSymbolicSolution(
      const ExecutionState &state,
      std::vector<std::pair<std::string, std::vector<unsigned char>>> &res)
      override;

  // get a sequence of inputs that drive the program to this state
  std::vector<NamedConcreteValue>
  getTestVector(const ExecutionState &state) override;

  void getCoveredLines(const ExecutionState &state,
                       std::map<const std::string *, std::set<unsigned>> &res)
      override;

  Expr::Width getWidthForLLVMType(llvm::Type *type) const;
  size_t getAllocationAlignment(const llvm::Value *allocSite) const;

  /// Returns the errno location in memory of the state
  int *getErrnoLocation(const ExecutionState &state) const;

  MergingSearcher *getMergingSearcher() const { return mergingSearcher; };
  void setMergingSearcher(MergingSearcher *ms) { mergingSearcher = ms; };

  KValue handleReadForLazyInit(ExecutionState &state, KInstruction *target,
                               const MemoryObject *mo, const ObjectState *os,
                               const ref<Expr> &offset, Expr::Width type,
                               bool &shouldReadFromOffset);

  void handleWriteForLazyInit(ExecutionState &state,
                              const uint64_t addressSegment,
                              const KValue& address);
  
  void initializeEntryFunctionArguments(llvm::Function *f,
                                        ExecutionState &state);
  ref<Expr> getPointerSymbolicSizeExpr(ExecutionState &state);
  ref<Expr> createTempReadForType(ExecutionState &state, llvm::Type* ty);

  void getSymbolicAddressForConstantSegment(ExecutionState &state, KValue &value);
  void handleICMPForLazyInit(const llvm::CmpInst::Predicate &predicate,
                             ExecutionState &state, KValue &left,
                             KValue &right);
  void checkWidthMatch(KValue &left, KValue &right) const;
  void handleICMPForLazyMO(ExecutionState &state, KValue &value);
  std::tuple<std::string, unsigned, unsigned> getErrorLocation() override { return errorLoc; }

};
  
} // End klee namespace

#endif /* KLEE_EXECUTOR_H */
