// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

typedef struct node {
  char name[5];
  struct node *next;
} node_t;

extern node_t* node;

int main() {
  if (node->name[0] == 'a') {
    if (node->name[0] == 'b') {
      assert("this should be an error");
    }
    else {
      // CHECK: KLEE: WARNING: main: Should be reachable
      klee_warning("Should be reachable");
    }
  }
  if (node->name[1] == 'b') {
    // CHECK: KLEE: WARNING: main: Should be reachable
    klee_warning("Should be reachable");
  }
  return 0;
}
