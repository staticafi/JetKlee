// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --exit-on-error --output-dir=%t.klee-out %t1.bc 2>&1

// this test compares pointers with symbolic offsets. If p+i has greater address than p2+j, it means that i has to be greater than j

#include <assert.h>

int main() {
  int *p, *p2;

  int i = klee_int("i");
  int j = klee_int("j");

  if (p == p2 && p+i > p2+j) {
    assert(i > j);
  }
  return 0;
}
