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

MmapAllocation::MmapAllocation(size_t spacesize, void *expectedAddr, int flgs)
  : dataSize(spacesize), expectedAddress(expectedAddr), flags(flgs) {
      initialize();
}

void MmapAllocation::initialize(size_t spacesize, void *expectedAddr, int flgs) {
    dataSize = spacesize;
    expectedAddress = expectedAddr;
    flags = flgs;
    initialize();
}

void MmapAllocation::initialize() {
    assert(data == nullptr && dataSize > 0);

    char *newSpace =
        (char *)mmap(expectedAddress, dataSize, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | flags, -1, 0);

    if (newSpace == MAP_FAILED) {
      klee_error("Couldn't mmap() memory for deterministic allocations");
    }
    if (expectedAddress != newSpace && expectedAddress != nullptr) {
      klee_error("Could not allocate memory deterministically");
    }

    if (expectedAddress != nullptr) {
      klee_message("Deterministic memory allocation starting from %p", newSpace);
    }

    data = newSpace;
    nextFreeSlot = newSpace;

    assert(data != nullptr && dataSize > 0);
}

MmapAllocation::MmapAllocation(MmapAllocation&& rhs)
 : data(rhs.data),
   nextFreeSlot(rhs.nextFreeSlot),
   dataSize(rhs.dataSize) {
  rhs.data = rhs.nextFreeSlot = nullptr;
  rhs.dataSize = 0;
}

MmapAllocation::~MmapAllocation() {
  if (data)
    munmap(data, dataSize);
}

void *MmapAllocation::getNextFreeSlot(size_t alignment) const {
  return (void *)llvm::alignTo((uint64_t)nextFreeSlot + alignment - 1, alignment);
}

bool MmapAllocation::hasSpace(size_t size, size_t alignment) const {
  return ((unsigned char *)getNextFreeSlot(alignment) + size
           < (unsigned char *)data + dataSize);
}

void *MmapAllocation::allocate(size_t size, size_t alignment) {
  auto address = getNextFreeSlot(alignment);
  nextFreeSlot = ((unsigned char *)address) + size;
  return address;
}

size_t MmapAllocation::getUsedSize() const {
    return (unsigned char *)nextFreeSlot - (unsigned char *)data;
}

size_t AllocatorMap::getAppropriateBlockSize(size_t allocationSize) const {
  return BASE_BLOCK_SIZE * (allocationSize/BASE_BLOCK_SIZE + 1);
}

MmapAllocator &AllocatorMap::getOrCreateAllocator(size_t blockSize) {
  auto it = allocators.find(blockSize);
  if (it == allocators.end()) {
    it = allocators.emplace_hint(it, blockSize, MmapAllocator{blockSize, flags});
  }
  return it->second;
}

void *AllocatorMap::allocate(size_t size, size_t alignment) {
  size_t blockSize = getAppropriateBlockSize(size);
  auto &alloc = getOrCreateAllocator(blockSize);
  return alloc.allocate(size, alignment);
}

MemoryAllocator::MemoryAllocator(bool determ,
                                 bool lowmem,
                                 size_t determ_size,
                                 void *expectedAddr)
  : deterministic(determ), lowmemAllocator(MAP_32BIT) {
  if (deterministic) {
      klee_message("Allocating memory deterministically");
      deterministicMem.initialize(determ_size, expectedAddr);
  }

  if (lowmem) {
      klee_message("Allocating memory with 32-bits addresses");
  }
}

void *MemoryAllocator::allocate(size_t size, size_t alignment) {
  if (deterministic) {
    auto address = deterministicMem.allocate(size + RedzoneSize, alignment);
    if (!address) {
      klee_warning_once(0, "Couldn't allocate %" PRIu64
                           " bytes. Not enough deterministic space left.",
                        size);
    }
    return address;
  } else if (lowmem) {
    return lowmemAllocator.allocate(size, alignment);
  } else {
    // Use malloc for the standard case
    if (alignment <= 8)
      return malloc(size);
    else {
      void *address = nullptr;
      int res = posix_memalign(&address, alignment, size);
      if (res < 0) {
        klee_warning("Allocating aligned memory failed.");
        return nullptr;
      }

      return address;
    }
  }
}

void MemoryAllocator::deallocate(void *mem) {
  // deterministic memory will be munmap'ed
  if (!deterministic && !lowmem)
    free(mem);
}

void MemoryAllocator::useLowMemory(bool lm) {
  lowmem = lm;
  // deterministic memory will be munmap'ed
  if (lowmem) {
    klee_message("Allocating memory with 32-bits addresses");
  }
}

void *MmapAllocator::allocate(size_t size, size_t alignment) {
  for (auto& block : blocks) {
    if (block.hasSpace(size, alignment))
      return block.allocate(size, alignment);
  }

  blocks.emplace_back(blockSize, nullptr, flags);
  return blocks.back().allocate(size, alignment);
}

size_t MmapAllocator::getUsedSize() const {
    size_t size = 0;
    for (const auto& block : blocks)
        size += block.getUsedSize();
    return size;
}

/***/
MemoryManager::MemoryManager(ArrayCache *_arrayCache, unsigned pointerWidth)
    : arrayCache(_arrayCache),
      allocator(DeterministicAllocation,
                pointerWidth == 32,
                DeterministicAllocationSize.getValue() * 1024 * 1024,
                (void *)DeterministicStartAddress.getValue()),
      lastSegment(FIRST_ORDINARY_SEGMENT) {}

MemoryManager::~MemoryManager() {
  while (!objects.empty()) {
    MemoryObject *mo = *objects.begin();
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

  if (concreteSize > 10 * 1024 * 1024) {
    klee_warning_once(0, "Large alloc: %" PRIu64 " bytes. "
                         "Not allocating this memory in real.",
                      concreteSize);
    hasConcreteSize = false;
  }

  // Return NULL if size is zero, this is equal to error during allocation
  if (NullOnZeroMalloc && hasConcreteSize && concreteSize == 0)
    return 0;

  if (!llvm::isPowerOf2_64(alignment)) {
    klee_warning("Only alignment of power of two is supported");
    return 0;
  }

  if (DeterministicAllocation) {
    // Handle the case of 0-sized allocations as 1-byte allocations.
    // This way, we make sure we have this allocation between its own red zones
    concreteSize = std::max(concreteSize, (uint64_t)1);
  } else {
    // allocate 1 byte for symbolic-size allocation, just so we get an address
    concreteSize = hasConcreteSize ? concreteSize : 1;
  }

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(++lastSegment,
                                       size, concreteSize,
                                       isLocal, isGlobal, false, allocSite, this);
  objects.insert(res);
  return res;
}

MemoryObject *MemoryManager::allocateFixed(uint64_t size,
                                           const llvm::Value *allocSite,
                                           uint64_t specialSegment) {
  ++stats::allocations;
  ref<Expr> sizeExpr = ConstantExpr::alloc(size, Context::get().getPointerWidth());
  MemoryObject *res;
  if (!specialSegment) {
    res =
        new MemoryObject(++lastSegment, sizeExpr, size,
                         false, true, true, allocSite, this);
  }
  else {
    res =
        new MemoryObject(specialSegment, sizeExpr, size,
                         false, true, true, allocSite, this);
  }
  objects.insert(res);
  return res;
}

void MemoryManager::deallocate(const MemoryObject *mo) { assert(0); }

void MemoryManager::markFreed(MemoryObject *mo) {
  objects.erase(mo);
}

size_t MemoryManager::getUsedDeterministicSize() const {
  return allocator.getUsedDeterministicSize();
}
