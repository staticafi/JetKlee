//===-- Cell.h --------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CELL_H
#define KLEE_CELL_H

#include <klee/KValue.h>

namespace klee {
  class Cell : public KValue {
  public:
    Cell() {}
    Cell(const KValue &other) : KValue(other) {}

    Cell(const Cell &other) = default;
  };
}

#endif /* KLEE_CELL_H */
