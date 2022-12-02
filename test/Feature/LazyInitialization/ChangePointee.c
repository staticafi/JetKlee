// RUN: %clang %s -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --lazy-init=true --ignore-lazy-oob -exit-on-error --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <assert.h>

extern int** fst;
extern int* snd;

int main() {
  *fst[0] = 1;
  *fst[10] = 10;
  *snd = 2;
  snd = fst[0];
  if (snd != fst[0]) {
    assert(0);
  }
  if (*snd != *fst[0]) {
    assert(0);
  }
  snd = fst[10];
  if (snd != fst[10]) {
    assert(0);
  }
  if (*snd != *fst[10]) {
    assert(0);
  }
  //CHECK: Should be reachable
  klee_warning("Should be reachable");
  return 0;
}