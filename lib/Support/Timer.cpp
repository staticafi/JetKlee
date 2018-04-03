//===-- Timer.cpp ---------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Config/Version.h"
#include "klee/Internal/Support/Timer.h"

using namespace klee;

WallTimer::WallTimer() {
  start = Timer::now();
}

WallTimer::Duration::rep WallTimer::check() {
  return std::chrono::duration_cast<Duration>(Timer::now() - start).count();
}
