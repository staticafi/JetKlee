// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --entry-point=check_null --lazy-init=true --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

extern int* value;

int check_null() {
  if (value == 0) {
    //CHECK: KLEE: WARNING: check_null: null branch reached
    klee_warning("null branch reached");
  } else {
    //CHECK: KLEE: WARNING: check_null: value branch reached
    klee_warning("value branch reached");
  }
  return 0;
}
