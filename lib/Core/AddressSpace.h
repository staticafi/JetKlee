//===-- AddressSpace.h ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_ADDRESSSPACE_H
#define KLEE_ADDRESSSPACE_H

#include "Memory.h"

#include "klee/Expr/Expr.h"
#include "klee/ADT/ImmutableMap.h"
#include "klee/System/Time.h"
#include "klee/Module/KValue.h"

namespace klee {
  class ExecutionState;
  class MemoryObject;
  class ObjectState;
  class TimingSolver;

  template<class T> class ref;

  typedef std::pair<const MemoryObject*, const ObjectState*> ObjectPair;
  typedef std::vector<ObjectPair> ResolutionList;  

  /// Function object ordering MemoryObject's by id.
  struct MemoryObjectLT {
    bool operator()(const MemoryObject *a, const MemoryObject *b) const;
  };

  typedef ImmutableMap<const MemoryObject *, ref<ObjectState>, MemoryObjectLT>
      MemoryMap;
  typedef ImmutableMap<uint64_t, const MemoryObject*> SegmentMap;
  typedef std::map</*address*/ const uint64_t, /*segment*/ const uint64_t> ConcreteAddressMap;
  typedef std::map</*segment*/ const uint64_t, /*address*/ const uint64_t> SegmentAddressMap;

  class AddressSpace {
    friend class ExecutionState;

  private:
    /// Epoch counter used to control ownership of objects.
    mutable unsigned cowKey;

    /// Unsupported, use copy constructor
    AddressSpace &operator=(const AddressSpace &);

  public:
    /// The MemoryObject -> ObjectState map that constitutes the
    /// address space.
    ///
    /// The set of objects where o->copyOnWriteOwner == cowKey are the
    /// objects that we own.
    ///
    /// \invariant forall o in objects, o->copyOnWriteOwner <= cowKey
    MemoryMap objects;

    SegmentMap segmentMap;

    ConcreteAddressMap concreteAddressMap;

    AddressSpace() : cowKey(1) {}
    AddressSpace(const AddressSpace &b)
      : cowKey(++b.cowKey),
      objects(b.objects),
      segmentMap(b.segmentMap) { }
    ~AddressSpace() {}

    /// Looks up constant segment in concreteAddressMap.
    /// \param segment segment to search for
    /// \param[out] address found address for given segment
    /// \return true iff address was found
    bool resolveInConcreteMap(const uint64_t &segment, uint64_t &address) const;

    /// Looks up constant segment in segmentMap
    /// \param pointer KValue containing ConstantExpr non-zero segment
    /// \param[out] result ObjectPair found for given segment
    /// \return true iff an ObjectPair was found
    bool resolveOneConstantSegment(const KValue &pointer,
                                   ObjectPair &result) const;

    /// Resolve address to an ObjectPair in result.
    ///
    /// \param state The state this address space is part of.
    /// \param solver A solver used to determine possible 
    ///               locations of the \a address.
    /// \param address The address to search for.
    /// \param[out] result An ObjectPair this address can resolve to 
    ///               (when returning true).
    /// \param[out] offset if resolveOne found OP by address,
    ///               sends back offset value at which it was found.
    /// \return true iff an object was found at \a address.
    bool resolveOne(ExecutionState &state, 
                    TimingSolver *solver,
                    const KValue &pointer,
                    ObjectPair &result,
                    bool &success,
                    llvm::Optional<uint64_t> &offset) const;

    /// Resolve pointer `p` to a list of `ObjectPairs` it can point
    /// to. If `maxResolutions` is non-zero then no more than that many
    /// pairs will be returned.
    ///
    /// \return true iff the resolution is incomplete (`maxResolutions`
    /// is non-zero and it was reached, or a query timed out).
    bool resolve(ExecutionState &state,
                 TimingSolver *solver,
                 const KValue &pointer,
                 ResolutionList &rl, 
                 unsigned maxResolutions=0,
                 time::Span timeout=time::Span()) const;

    /// Resolve pointer, first checking for constant segment in segmentMap
    /// and then for constant address in concreteAddressMap, Without returning the offset value
    /// TODO:: timeout and maxResolutions
    bool resolveConstantPointer(ExecutionState &state,
                                TimingSolver *solver,
                                const KValue &pointer,
                                ResolutionList &rl,
                                unsigned maxResolutions=0,
                                time::Span timeout=time::Span()) const;

    /***/

    /// Add a binding to the address space.
    void bindObject(const MemoryObject *mo, ObjectState *os);

    /// Remove a binding from the address space.
    void unbindObject(const MemoryObject *mo);

    /// Lookup a binding from a MemoryObject.
    const ObjectState *findObject(const MemoryObject *mo) const;

    /// \brief Obtain an ObjectState suitable for writing.
    ///
    /// This returns a writeable object state, creating a new copy of
    /// the given ObjectState if necessary. If the address space owns
    /// the ObjectState then this routine effectively just strips the
    /// const qualifier it.
    ///
    /// \param mo The MemoryObject to get a writeable ObjectState for.
    /// \param os The current binding of the MemoryObject.
    /// \return A writeable ObjectState (\a os or a copy).
    ObjectState *getWriteable(const MemoryObject *mo, const ObjectState *os);

    /// Copy the concrete values of all managed ObjectStates into the
    /// actual system memory location they were allocated at.
    void copyOutConcretes(const SegmentAddressMap &resolved,
                          bool ignoreReadOnly = false);

    /// Copy the concrete values of all managed ObjectStates back from
    /// the actual system memory location they were allocated
    /// at. ObjectStates will only be written to (and thus,
    /// potentially copied) if the memory values are different from
    /// the current concrete values.
    ///
    /// \retval true The copy succeeded. 
    /// \retval false The copy failed because a read-only object was modified.
    bool copyInConcretes(const SegmentAddressMap &resolved,
                         ExecutionState &state, TimingSolver *solver);

    /// Updates the memory object with the raw memory from the address
    ///
    /// @param mo The MemoryObject to update
    /// @param os The associated memory state containing the actual data
    /// @param src_address the address to copy from
    /// @return
    bool copyInConcrete(const MemoryObject *mo, const ObjectState *os,
                        const uint64_t &resolvedAddress, ExecutionState &state,
                        TimingSolver *solver);

    /// Checks if address can be found within bounds of concrete addresses
    /// in AddressSpace::concreteAddressMap
    ///
    /// \param state
    /// \param solver
    /// \param address contains constant address which we are looking for
    /// \param rl ResolutionList containing found ObjectPairs
    void resolveAddressWithOffset(const ExecutionState &state,
                                  TimingSolver *solver,
                                  const ref<Expr> &address, ResolutionList &rl,
                                  llvm::Optional<uint64_t>& offset) const;

    /// writes data from address to ObjectStates concrete store, checking if
    /// value stored in address has same length as pointer on current platform.
    /// if yes, resolveAddressWithOffset is called to check if it is not
    /// a pointer we have already seen before.
    void writeToWOS(ExecutionState &state, TimingSolver *solver,
                    const uint8_t *address, ObjectState *wos) const;
  };
} // End klee namespace

#endif /* KLEE_ADDRESSSPACE_H */
