//===-- MemoryManager.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <sys/mman.h>

#include "Common.h"

#include "CoreStats.h"
#include "Memory.h"
#include "MemoryManager.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Solver.h"

#include "llvm/Support/CommandLine.h"

using namespace klee;

/***/

MemoryManager::~MemoryManager() { 
  while (!objects.empty()) {
    MemoryObject *mo = *objects.begin();
    if (!mo->isFixed)
      munmap((void *)mo->address, mo->size);
    objects.erase(mo);
    delete mo;
  }
}

// @low_address means 32-bit address
static uint64_t allocate_memory(uint64_t size, bool low_address = false)
{
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (low_address)
    flags |= MAP_32BIT;

  void *mem = mmap(0, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (mem == MAP_FAILED)
    return 0;

  return (uint64_t) mem;
}

MemoryObject *MemoryManager::allocate(uint64_t size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite) {
  if (size>10*1024*1024)
    klee_warning_once(0, "Large alloc: %u bytes.  KLEE may run out of memory.", (unsigned) size);

  uint64_t address = allocate_memory(size, true /* 32-bit */);
  if (!address)
    return 0;

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(address, size, isLocal, isGlobal, false,
                                       allocSite, this);
  objects.insert(res);
  return res;
}

MemoryObject *MemoryManager::allocateFixed(uint64_t address, uint64_t size,
                                           const llvm::Value *allocSite) {
#ifndef NDEBUG
  for (objects_ty::iterator it = objects.begin(), ie = objects.end();
       it != ie; ++it) {
    MemoryObject *mo = *it;
    if (address+size > mo->address && address < mo->address+mo->size)
      klee_error("Trying to allocate an overlapping object");
  }
#endif

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(address, size, false, true, true,
                                       allocSite, this);
  objects.insert(res);
  return res;
}

void MemoryManager::deallocate(const MemoryObject *mo) {
  assert(0);
}

void MemoryManager::markFreed(MemoryObject *mo) {
  if (objects.find(mo) != objects.end())
  {
    if (!mo->isFixed)
      munmap((void *)mo->address, mo->size);
    objects.erase(mo);
  }
}
