// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --entry-point=check_void_ptr --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

typedef struct {
  int val1;
  int val2;
} int_struct_t;

int check_void_ptr(int_struct_t* ptr) {
  void* void_ptr = ptr;
  int* pInt = (int*)void_ptr + 0;
  if (*pInt == 0) {
    if (*pInt != ptr->val1) {
      assert(0 && "void ptr value eq test failed");
    } else {
      // CHECK: KLEE: WARNING: check_void_ptr: Should be reachable
      klee_warning("Should be reachable");
    }
  }
}
