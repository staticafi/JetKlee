// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --ignore-lazy-oob --entry-point=foo --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

void foo(int* ptr1, int* ptr2) {
  *ptr1 = 5;
  *ptr2 = 10;
  if (*ptr1 == *ptr2) {
    //CHECK-DAG: Equal reachable
    klee_warning("Equal reachable");
  } else {
    //CHECK-DAG: non-equal reachable
    klee_warning("non-equal reachable");
  }
}