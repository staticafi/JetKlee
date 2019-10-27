//===-- MemoryManager.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_MEMORYMANAGER_H
#define KLEE_MEMORYMANAGER_H

#include <cstddef>
#include <set>
#include <vector>
#include <cstdint>

#include "klee/Expr/Expr.h"

namespace llvm {
class Value;
}

namespace klee {
class MemoryObject;
class ArrayCache;

class MmapAllocation {
  void *data{nullptr};
  void *nextFreeSlot{nullptr};
  size_t dataSize{0};
  void *expectedAddress{nullptr};
  int flags{0};

  void *getNextFreeSlot(size_t alignment) const;
  void initialize();

public:
  MmapAllocation() = default;
  MmapAllocation(size_t spacesize, void *expectedAddr = nullptr, int flags = 0);
  MmapAllocation(MmapAllocation&&);
  MmapAllocation(const MmapAllocation&) = delete;
  ~MmapAllocation();

  void initialize(size_t datasize, void *expectedAddr = nullptr, int flags = 0);
  bool hasSpace(size_t size, size_t alignment) const;
  void *allocate(size_t size, size_t alignment);
  size_t getUsedSize() const;
};

class MmapAllocator {
  size_t blockSize{4096};
  int flags{0};
  std::vector<MmapAllocation> blocks;

public:
  MmapAllocator(size_t blksize = 4096, int flags = 0)
   : blockSize(blksize), flags(flags) {}

  void *allocate(size_t size, size_t alignment);
  size_t getUsedSize() const;
};


class MemoryAllocator {
    bool deterministic{false};
    // allocate memory on lower 32bit memory space
    bool lowmem{false};

    MmapAllocation deterministicMem{};
    MmapAllocator lowmemAllocator{};
public:
    MemoryAllocator(bool determ, bool lowmem, size_t determ_size, void *expectedAddr);

    void *allocate(size_t size, size_t alignment);
    void deallocate(void *);
    void useLowMemory(bool lm);

    size_t getUsedDeterministicSize() const {
        return deterministicMem.getUsedSize();
    }
};

class MemoryManager {
private:
  typedef std::set<MemoryObject *> objects_ty;
  objects_ty objects;
  ArrayCache *const arrayCache;

  MemoryAllocator allocator;
  uint64_t lastSegment;
public:
  MemoryManager(ArrayCache *arrayCache,
                unsigned pointerWidth = 64);
  ~MemoryManager();

  /**
   * Returns memory object which contains a handle to real virtual process
   * memory.
   */
  MemoryObject *allocate(uint64_t size, bool isLocal, bool isGlobal,
                         const llvm::Value *allocSite, size_t alignment);
  MemoryObject *allocate(ref<Expr> size, bool isLocal, bool isGlobal,
                         const llvm::Value *allocSite, size_t alignment);
  MemoryObject *allocateFixed(uint64_t address, uint64_t size,
                              const llvm::Value *allocSite);
  void deallocate(const MemoryObject *mo);
  void markFreed(MemoryObject *mo);
  ArrayCache *getArrayCache() const { return arrayCache; }
  void useLowMemory(bool lm) {
    allocator.useLowMemory(lm);
  }

  /*
   * Returns the size used by deterministic allocation in bytes
   */
  size_t getUsedDeterministicSize() const;
};

} // End klee namespace

#endif /* KLEE_MEMORYMANAGER_H */
