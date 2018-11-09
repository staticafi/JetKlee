//===-- MemoryManager.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MemoryManager.h"

#include "CoreStats.h"
#include "Memory.h"

#include "klee/Expr/Expr.h"
#include "klee/Support/ErrorHandling.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"

#include <inttypes.h>
#include <sys/mman.h>

using namespace klee;

namespace {

llvm::cl::OptionCategory MemoryCat("Memory management options",
                                   "These options control memory management.");

llvm::cl::opt<bool> DeterministicAllocation(
    "allocate-determ",
    llvm::cl::desc("Allocate memory deterministically (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(MemoryCat));

llvm::cl::opt<unsigned> DeterministicAllocationSize(
    "allocate-determ-size",
    llvm::cl::desc(
        "Preallocated memory for deterministic allocation in MB (default=100)"),
    llvm::cl::init(100), llvm::cl::cat(MemoryCat));

llvm::cl::opt<bool> NullOnZeroMalloc(
    "return-null-on-zero-malloc",
    llvm::cl::desc("Returns NULL if malloc(0) is called (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(MemoryCat));

llvm::cl::opt<unsigned> RedzoneSize(
    "redzone-size",
    llvm::cl::desc("Set the size of the redzones to be added after each "
                   "allocation (in bytes). This is important to detect "
                   "out-of-bounds accesses (default=10)"),
    llvm::cl::init(10), llvm::cl::cat(MemoryCat));

llvm::cl::opt<unsigned long long> DeterministicStartAddress(
    "allocate-determ-start-address",
    llvm::cl::desc("Start address for deterministic allocation. Has to be page "
                   "aligned (default=0x7ff30000000)"),
    llvm::cl::init(0x7ff30000000), llvm::cl::cat(MemoryCat));
} // namespace

void MmapAllocation::initialize(size_t size) {
    assert(deterministicSpace == nullptr && spaceSize == 0);

    // Page boundary
    void *expectedAddress = (void *)DeterministicStartAddress.getValue();

    char *newSpace =
        (char *)mmap(expectedAddress, size, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (newSpace == MAP_FAILED) {
      klee_error("Couldn't mmap() memory for deterministic allocations");
    }
    if (expectedAddress != newSpace && expectedAddress != 0) {
      klee_error("Could not allocate memory deterministically");
    }

    klee_message("Deterministic memory allocation starting from %p", newSpace);
    deterministicSpace = newSpace;
    nextFreeSlot = newSpace;
    spaceSize = size;
}

MmapAllocation::~MmapAllocation() {
  if (deterministicSpace)
    munmap(deterministicSpace, spaceSize);
}

void *MmapAllocation::getNextFreeSlot(size_t alignment) const {
  return (void *)llvm::alignTo((uint64_t)nextFreeSlot + alignment - 1, alignment);
}

bool MmapAllocation::hasSpace(size_t size, size_t alignment) const {
  return ((unsigned char *)getNextFreeSlot(alignment) + size
           < (unsigned char *)deterministicSpace + spaceSize);
}

void *MmapAllocation::allocate(size_t size, size_t alignment) {
  auto address = getNextFreeSlot(alignment);
  nextFreeSlot = ((unsigned char *)address) + size;
  return address;
}

size_t MmapAllocation::getUsedSize() const {
    return (unsigned char *)nextFreeSlot - (unsigned char *)deterministicSpace;
}

/***/
MemoryManager::MemoryManager(ArrayCache *_arrayCache)
    : arrayCache(_arrayCache), lastSegment(0) {
  if (DeterministicAllocation) {
    deterministicMem.initialize(DeterministicAllocationSize.getValue() * 1024 * 1024);
  }
}

MemoryManager::~MemoryManager() {
  while (!objects.empty()) {
    MemoryObject *mo = *objects.begin();
    if (!mo->isFixed && !DeterministicAllocation)
      free((void *)mo->address);
    objects.erase(mo);
    delete mo;
  }
}

MemoryObject *MemoryManager::allocate(uint64_t size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite,
                                      size_t alignment) {
  ref<Expr> sizeExpr = ConstantExpr::alloc(size, Context::get().getPointerWidth());
  return allocate(sizeExpr, isLocal, isGlobal, allocSite, alignment);
}

MemoryObject *MemoryManager::allocate(ref<Expr> size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite,
                                      size_t alignment) {
  uint64_t concreteSize = 0;
  bool hasConcreteSize = false;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    hasConcreteSize = true;
    concreteSize = CE->getZExtValue();
  }

  if (concreteSize > 10 * 1024 * 1024)
    klee_warning_once(0, "Large alloc: %" PRIu64
                         " bytes.  KLEE may run out of memory.",
                      concreteSize);

  // Return NULL if size is zero, this is equal to error during allocation
  if (NullOnZeroMalloc && hasConcreteSize && concreteSize == 0)
    return 0;

  if (!llvm::isPowerOf2_64(alignment)) {
    klee_warning("Only alignment of power of two is supported");
    return 0;
  }

  uint64_t address = 0;
  if (DeterministicAllocation) {
    // Handle the case of 0-sized allocations as 1-byte allocations.
    // This way, we make sure we have this allocation between its own red zones
    size_t alloc_size = std::max(concreteSize, (uint64_t)1);
    if (deterministicMem.hasSpace(alloc_size + RedzoneSize, alignment)) {
        address = (uint64_t)deterministicMem.allocate(alloc_size + RedzoneSize,
                                                      alignment);
    } else {
      klee_warning_once(0, "Couldn't allocate %" PRIu64
                           " bytes. Not enough deterministic space left.",
                        concreteSize);
      address = 0;
    }
  } else {
    // allocate 1 byte for symbolic-size allocation, just so we get an address
    size_t alloc_size = hasConcreteSize ? concreteSize : 1;
    // Use malloc for the standard case
    if (alignment <= 8)
      address = (uint64_t)malloc(alloc_size);
    else {
      int res = posix_memalign((void **)&address, alignment, alloc_size);
      if (res < 0) {
        klee_warning("Allocating aligned memory failed.");
        address = 0;
      }
    }
  }

  if (!address)
    return 0;

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(++lastSegment, address, size, isLocal,
                                       isGlobal, false, allocSite, this);
  objects.insert(res);
  return res;
}

MemoryObject *MemoryManager::allocateFixed(uint64_t address, uint64_t size,
                                           const llvm::Value *allocSite) {
#ifndef NDEBUG
  for (objects_ty::iterator it = objects.begin(), ie = objects.end(); it != ie;
       ++it) {
    MemoryObject *mo = *it;
    // symbolic size objects can overlap
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
      unsigned moSize = CE->getZExtValue();
      if (address + moSize > mo->address && address < mo->address + moSize)
        klee_error("Trying to allocate an overlapping object");
    }
  }
#endif

  ++stats::allocations;
  ref<Expr> sizeExpr = ConstantExpr::alloc(size, Context::get().getPointerWidth());
  MemoryObject *res =
      new MemoryObject(++lastSegment, address, sizeExpr, false, true, true, allocSite, this);
  objects.insert(res);
  return res;
}

void MemoryManager::deallocate(const MemoryObject *mo) { assert(0); }

void MemoryManager::markFreed(MemoryObject *mo) {
  if (objects.find(mo) != objects.end()) {
    if (!mo->isFixed && !DeterministicAllocation)
      free((void *)mo->address);
    objects.erase(mo);
  }
}

size_t MemoryManager::getUsedDeterministicSize() const {
  return deterministicMem.getUsedSize();
}
