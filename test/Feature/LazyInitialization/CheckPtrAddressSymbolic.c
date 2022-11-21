// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <stdlib.h>

extern int* extern_ptr;

int main() {
  //compare pointer values (bc thesis check) - both branches should be reachable
  int* ptr_new = (int*)malloc(sizeof(int));
  if (&extern_ptr > &ptr_new) {
    klee_warning("First branch reached"); // CHECK: First branch
  } else {
    klee_warning("Second branch reached"); // CHECK: Second branch
  }
  free(ptr_new);

  return 0;
}
