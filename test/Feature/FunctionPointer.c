// RUN: %clang %s -emit-llvm -g -c -o %t.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --write-no-tests --exit-on-error %t.bc 2>&1 | FileCheck %s

#include "klee/klee.h"

#include <stdio.h>

void foo(const char *msg) { printf("foo: %s\n", msg); }
void baz(const char *msg) { printf("baz: %s\n", msg); }

void (*xx)(const char *) = foo;

void bar(void (*fp)(const char *)) { fp("called via bar"); }

int main(int argc, char **argv) {
  void (*fp)(const char *) = foo;

  printf("going to call through fp\n");
  // CHECK: foo: called via fp
  fp("called via fp");

  printf("calling via pass through\n");
  // CHECK: foo: called via bar
  bar(foo);

  fp = baz;
  // CHECK: baz: called via fp
  fp("called via fp");

  // CHECK: foo: called via xx
  xx("called via xx");

// FIXME: to be fixed in future commits
// do not forget to change the CHECK-TODO labels!!!
#if 0
  klee_make_symbolic(&fp, sizeof fp, "fp");
  if(fp == baz) {
    // CHECK-TODO: baz: calling via simple symbolic!
    printf("fp = %p, baz = %p\n", fp, baz);
    fp("calling via simple symbolic!");
    return 0;
  }

  void (*fp2)(const char *);
  klee_make_symbolic(&fp2, sizeof fp2, "fp2");
  if(fp2 == baz || fp2 == foo) {
    // CHECK-TODO-DAG: baz: calling via symbolic!
    // CHECK-TODO-DAG: foo: calling via symbolic!
    fp2("calling via symbolic!");
  }
#endif

  return 0;
}
