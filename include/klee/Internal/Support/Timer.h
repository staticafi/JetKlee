//===-- Timer.h -------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_TIMER_H
#define KLEE_TIMER_H

#include <chrono>

namespace klee {
  class WallTimer {
    using Timer = std::chrono::system_clock;
    using Duration = std::chrono::microseconds;

    Timer::time_point start;
    
  public:
    WallTimer();

    /// check - Return the delta since the timer was created, in microseconds.
    Duration::rep check();
  };
}

#endif

