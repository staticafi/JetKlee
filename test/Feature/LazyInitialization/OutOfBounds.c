// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

static const int kSIZE = 5;

typedef struct node {
  struct node *next;
  char* str[kSIZE];
} node_t;

extern node_t* node;

int main() {
  if (node->str[kSIZE + 1] == 'a') {
    assert(0 && "out of range!");
  } else {
    // CHECK: KLEE: WARNING: main: Should be reachable
    klee_warning("Should be reachable");
  }
}
