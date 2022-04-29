// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --entry-point=foo --max-ptr-depth=10 --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

void foo(int *array[]) {
  int a,b;
  for (int i = 0; i < 10; ++i) {
    if (i % 2 == 0) {
      array[i] = &a;
      assert(array[i] == &a);
    } else {
      array[i] = &b;
      assert(array[i] == &b);
    }
    //CHECK-DAG: Should be reachable
    klee_warning("Should be reachable");
  }
  //CHECK-DAG: Function ends after few itterations
  klee_warning("Function ends after few itterations");
}
