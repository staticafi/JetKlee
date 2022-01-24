// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee -entry-point=check_val_eq -lazy-init=true --exit-on-error --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

typedef struct node {
  char name[5];
  struct node *next;
} node_t;

int check_val_eq(node_t* node) {
  if (node->name[0] == 'a') {
    if (node->name[0] == 'b') {
      assert(0 && "this should be an error");
    }
  }
  if (node->name[1] == 'b') {
    // CHECK: KLEE: WARNING: check_val_eq: Should be reachable
    klee_warning("Should be reachable");
  }
  return 0;
}
