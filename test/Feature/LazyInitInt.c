// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee -lazy-init=true --exit-on-error --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

typedef struct node {
  int val;
  struct node *next;
} node_t;

extern node_t* node;

int main() {
  if (node->val == 1) {
    if (node->val == 3) {
      assert("this should be an error");
    }
  }
  if (node->next->val == 1) {
    if (node->next->val == 3) {
      assert("this should also be an error");
    }
  }

  if (node->val == 1) {
    if (node->val == 1) {
      // CHECK: KLEE: WARNING: main: Should be reachable
      klee_warning("Should be reachable");
    }
  }
  return 0;
}
