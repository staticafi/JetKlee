// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true -max-ptr-depth=10 --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s


typedef struct node {
  int val;
  struct node *next;
} node_t;

extern node_t* node;

int main() {
  node_t* next = node->next;
  while (next->next) {
    next = next->next;
  }
  klee_warning("Execution continues after cycle"); // CHECK: Execution continues
  return 0;
}
