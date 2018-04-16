//===-- Memory.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_MEMORY_H
#define KLEE_MEMORY_H

#include "Context.h"
#include "TimingSolver.h"

#include "klee/ADT/BitArray.h"
#include "klee/Module/KValue.h"

#include "llvm/ADT/StringExtras.h"

#include <string>
#include <vector>

namespace llvm {
  class Value;
}

namespace klee {

class ArrayCache;
class ExecutionState;
class MemoryManager;
class Solver;

class MemoryObject {
  friend class STPBuilder;
  friend class ObjectState;
  friend class ExecutionState;
  friend class ref<MemoryObject>;
  friend class ref<const MemoryObject>;

private:
  static int counter;
  /// @brief Required by klee::ref-managed objects
  mutable class ReferenceCounter _refCount;

public:
  unsigned id;
  uint64_t segment;
  uint64_t address;

  /// size in bytes
  ref<Expr> size;
  mutable std::string name;

  bool isLocal;
  mutable bool isGlobal;
  bool isFixed;

  bool isUserSpecified;

  MemoryManager *parent;

  /// "Location" for which this memory object was allocated. This
  /// should be either the allocating instruction or the global object
  /// it was allocated for (or whatever else makes sense).
  const llvm::Value *allocSite;

  // DO NOT IMPLEMENT
  MemoryObject(const MemoryObject &b);
  MemoryObject &operator=(const MemoryObject &b);

public:
  // XXX this is just a temp hack, should be removed
  explicit
  MemoryObject(uint64_t _address) 
    : id(counter++),
      segment(0),
      address(_address),
      size(0),
      isFixed(true),
      parent(NULL),
      allocSite(0) {
  }

  MemoryObject(uint64_t _address, ref<Expr> _size,
               bool _isLocal, bool _isGlobal, bool _isFixed,
               const llvm::Value *_allocSite,
               MemoryManager *_parent)
    : id(counter++),
      segment(0),
      address(_address),
      size(ZExtExpr::create(_size, Context::get().getPointerWidth())),
      name("unnamed"),
      isLocal(_isLocal),
      isGlobal(_isGlobal),
      isFixed(_isFixed),
      isUserSpecified(false),
      parent(_parent), 
      allocSite(_allocSite) {
  }

    MemoryObject(uint64_t segment, uint64_t _address, ref<Expr> _size,
                 bool _isLocal, bool _isGlobal, bool _isFixed,
                 const llvm::Value *_allocSite,
                 MemoryManager *_parent)
    : id(counter++),
      segment(segment),
      address(_address),
      size(ZExtExpr::create(_size, Context::get().getPointerWidth())),
      name("unnamed"),
      isLocal(_isLocal),
      isGlobal(_isGlobal),
      isFixed(_isFixed),
      isUserSpecified(false),
      parent(_parent),
      allocSite(_allocSite) {
  }

  ~MemoryObject();

  /// Get an identifying string for this allocation.
  void getAllocInfo(std::string &result) const;

  void setName(std::string name) const {
    this->name = name;
  }

  uint64_t getSegment() const {
    return segment;
  }
  ref<ConstantExpr> getSegmentExpr() const {
    return ConstantExpr::create(segment, Context::get().getPointerWidth());
  }
  ref<ConstantExpr> getBaseExpr() const {
    return ConstantExpr::create(address, Context::get().getPointerWidth());
  }
  KValue getPointer() const {
    return KValue(getSegmentExpr(), getBaseExpr());
  }
  KValue getPointer(uint64_t offset) const {
    return KValue(getSegmentExpr(),
                  AddExpr::create(getBaseExpr(),
                                  ConstantExpr::create(offset,
                                                       Context::get().getPointerWidth())));
  }
  std::string getAddressString() const {
    return std::to_string(address);
  }
  std::string getSizeString() const {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
      return std::to_string(CE->getZExtValue());
    } else {
      return "symbolic";
    }
  }
  ref<Expr> getSizeExpr() const {
    return size;
  }
  ref<Expr> getOffsetExpr(ref<Expr> pointer) const {
    return SubExpr::create(pointer, getBaseExpr());
  }
  ref<Expr> getBoundsCheckPointer(KValue pointer) const {
    return AndExpr::create(
            getBoundsCheckSegment(pointer.getSegment()),
            getBoundsCheckOffset(getOffsetExpr(pointer.getOffset())));
  }
  ref<Expr> getBoundsCheckPointer(KValue pointer, unsigned bytes) const {
    return AndExpr::create(
            getBoundsCheckSegment(pointer.getSegment()),
            getBoundsCheckOffset(getOffsetExpr(pointer.getOffset()), bytes));
  }
  ref<Expr> getBoundsCheckOffset(ref<Expr> offset) const {
    if (isa<ConstantExpr>(size) && cast<ConstantExpr>(size)->isZero()) {
      return EqExpr::create(offset,
                            ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      return UltExpr::create(offset, getSizeExpr());
    }
  }
  ref<Expr> getBoundsCheckOffset(ref<Expr> offset, unsigned bytes) const {
    return UltExpr::create(offset,
                           SubExpr::create(size,
                                           ConstantExpr::alloc(bytes - 1,
                                                               size->getWidth())));
  }

  /// Compare this object with memory object b.
  /// \param b memory object to compare with
  /// \return <0 if this is smaller, 0 if both are equal, >0 if b is smaller
  int compare(const MemoryObject &b) const {
    // Short-cut with id
    if (id == b.id)
      return 0;
    if (address != b.address)
      return (address < b.address ? -1 : 1);

    if (size != b.size)
      return (size < b.size ? -1 : 1);

    if (allocSite != b.allocSite)
      return (allocSite < b.allocSite ? -1 : 1);

    return 0;
  }

private:
  ref<Expr> getBoundsCheckSegment(ref<Expr> segment) const {
    return OrExpr::create(
            EqExpr::create(segment, ConstantExpr::alloc(0, segment->getWidth())),
            EqExpr::create(getSegmentExpr(), segment));
  }
};

class ObjectStatePlane {
private:
  friend class AddressSpace;
  friend class ref<ObjectState>;

  /// @brief Required by klee::ref-managed objects
  class ReferenceCounter _refCount;

  ref<const ObjectState> parent;

  /// @brief Holds all known concrete bytes
  std::vector<uint8_t> concreteStore;

  /// @brief concreteMask[byte] is set if byte is known to be concrete
  BitArray concreteMask;

  /// knownSymbolics[byte] holds the symbolic expression for byte,
  /// if byte is known to be symbolic
  std::vector<ref<Expr>> knownSymbolics;

  /// unflushedMask[byte] is set if byte is unflushed
  /// mutable because may need flushed during read of const
  mutable BitArray unflushedMask;

  // mutable because we may need flush during read of const
  mutable UpdateList updates;

public:
  unsigned sizeBound;

  bool symbolic;

  uint8_t initialValue;

public:
  /// Create a new object state for the given memory object with concrete
  /// contents. The initial contents are undefined, it is the callers
  /// responsibility to initialize the object contents appropriately.
  ObjectStatePlane(const ObjectState *parent);

  /// Create a new object state for the given memory object with symbolic
  /// contents.
  ObjectStatePlane(const ObjectState *parent, const Array *array);

  ObjectStatePlane(const ObjectState *parent, const ObjectStatePlane &os);
  ~ObjectStatePlane() = default;

  /// Make contents all concrete and zero
  void initializeToZero();

  /// Make contents all concrete and random
  void initializeToRandom();

  ref<Expr> read(ref<Expr> offset, Expr::Width width) const;
  ref<Expr> read(unsigned offset, Expr::Width width) const;
  ref<Expr> read8(unsigned offset) const;

  void write(unsigned offset, ref<Expr> value);
  void write(ref<Expr> offset, ref<Expr> value);

  void write8(unsigned offset, uint8_t value);
  void write16(unsigned offset, uint16_t value);
  void write32(unsigned offset, uint32_t value);
  void write64(unsigned offset, uint64_t value);
  void print() const;

  /*
    Looks at all the symbolic bytes of this object, gets a value for them
    from the solver and puts them in the concreteStore.
  */
  void flushToConcreteStore(TimingSolver *solver,
                            const ExecutionState &state);

private:
  const UpdateList &getUpdates() const;

  void makeConcrete();

  ref<Expr> read8(ref<Expr> offset) const;
  void write8(unsigned offset, ref<Expr> value);
  void write8(ref<Expr> offset, ref<Expr> value);

  void flushForRead() const;
  void flushForWrite();

  /// isByteConcrete ==> !isByteKnownSymbolic
  bool isByteConcrete(unsigned offset) const;

  /// isByteKnownSymbolic ==> !isByteConcrete
  bool isByteKnownSymbolic(unsigned offset) const;

  /// isByteUnflushed(i) => (isByteConcrete(i) || isByteKnownSymbolic(i))
  bool isByteUnflushed(unsigned offset) const;

  void markByteConcrete(unsigned offset);
  void markByteSymbolic(unsigned offset);
  void markByteFlushed(unsigned offset) const;
  void markByteUnflushed(unsigned offset) const;
  void setKnownSymbolic(unsigned offset, Expr *value);
  uint8_t getConcreteValue(unsigned offset) const;
};

class ObjectState {
private:
  friend class AddressSpace;
  unsigned copyOnWriteOwner; // exclusively for AddressSpace

  friend class ObjectHolder;
  friend class ref<ObjectState>;
  friend class ref<const ObjectState>;

  /// @brief Required by klee::ref-managed objects
  mutable ReferenceCounter _refCount;

  ref<const MemoryObject> object;


public:
  bool readOnly;

private:
  ObjectStatePlane *segmentPlane;
  ObjectStatePlane *offsetPlane;

public:
  /// Create a new object state for the given memory object with concrete
  /// contents. The initial contents are undefined, it is the callers
  /// responsibility to initialize the object contents appropriately.
  ObjectState(const MemoryObject *mo);

  /// Create a new object state for the given memory object with symbolic
  /// contents.
  ObjectState(const MemoryObject *mo, const Array *array);

  ObjectState(const ObjectState &os);
  // Copy for realloc
  ObjectState(const ObjectState &os, const MemoryObject *mo);
  ~ObjectState();

  const MemoryObject *getObject() const { return object.get(); }

  void setReadOnly(bool ro) {
    readOnly = ro;
  }

  // make contents all concrete and zero
  void initializeToZero();
  // make contents all concrete and random
  void initializeToRandom();

  void flushToConcreteStore(TimingSolver *solver,
                            const ExecutionState &state) const {
    offsetPlane->flushToConcreteStore(solver, state);
  }

  KValue read(ref<Expr> offset, Expr::Width width) const;
  KValue read(unsigned offset, Expr::Width width) const;
  KValue read8(unsigned offset) const;

  // return bytes written.
  void write(unsigned offset, const KValue &value);
  void write(ref<Expr> offset, const KValue &value);

  void write8(unsigned offset, uint8_t segment, uint8_t value);
  void write16(unsigned offset, uint16_t segment, uint16_t value);
  void write32(unsigned offset, uint32_t segment, uint32_t value);
  void write64(unsigned offset, uint64_t segment, uint64_t value);

  ArrayCache *getArrayCache() const;

private:
  bool prepareSegmentPlane(bool nonzero);
  bool prepareSegmentPlane(ref<Expr> value);
};
  
} // End klee namespace

#endif /* KLEE_MEMORY_H */
