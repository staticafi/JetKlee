// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>
extern int* arr[2];
extern int* ptr;
extern int* ptr2;
int main(void) {
  *arr[0] = 11;
  *arr[1] = 9;
  arr[0] = ptr;
  arr[1] = ptr2;
  *ptr = 42;
  *ptr2 = 84;
  assert(*arr[0] == *ptr);
  assert(*arr[1] == *ptr2);
  return 0;
}