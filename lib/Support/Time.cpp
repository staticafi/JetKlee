//===-- Time.cpp ----------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <chrono>

#include "klee/Config/Version.h"
#include "klee/Internal/System/Time.h"

#include "llvm/Support/Process.h"

using namespace llvm;
using namespace klee;

double util::getUserTime() {
  using namespace std::chrono;

#if LLVM_VERSION_MAJOR < 4
  sys::TimeValue now(0,0),user(0,0),sys(0,0);
  sys::Process::GetTimeUsage(now,user,sys);
  return (user.seconds() + (double) user.nanoseconds() * 1e-9);
#else
  sys::TimePoint<> now;
  nanoseconds user, sys;
  sys::Process::GetTimeUsage(now,user,sys);
  return user.count();
#endif
}

#define MICROSECONDS_PER_SECOND 1000000
#define NANOSECONDS_PER_MICROSECOND  1000

double util::getWallTime() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto nanos = (now.time_since_epoch() % std::chrono::seconds(1)).count();
  auto seconds = system_clock::to_time_t(
    time_point_cast<system_clock::time_point::duration>(now));

  return seconds * MICROSECONDS_PER_SECOND +
               (nanos / NANOSECONDS_PER_MICROSECOND);
}

