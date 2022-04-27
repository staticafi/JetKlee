// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

int main() {
  int a;
  if (&a != 0x12345678) {
    // CHECK-DAG: KLEE: WARNING: main: Second branch reached
    klee_warning("Second branch reached");
  } else {
    // CHECK-DAG: KLEE: WARNING: main: First branch reached
    klee_warning("First branch reached");
  }
  return 0;
}