// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --entry-point=check_va_args --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <stdarg.h>
#include <assert.h>

void check_va_args(int* count, ...) {
  va_list args;
  va_start(args, count);
  int sum = 0;
  if (*count == 2) {
    while (*count != 0) {
      *count -= 1;
      sum += va_arg(args, int);
    }
    if (*count != 0) {
      assert(0 && "*count decrement failed");
    }
  }
  va_end(args);

  if (sum > 10) {
    // CHECK: KLEE: WARNING: check_va_args: Should be reachable
    klee_warning("Should be reachable");
  } else {
    // CHECK: KLEE: WARNING: check_va_args: Should be reachable
    klee_warning("Should be reachable");
  }
}
