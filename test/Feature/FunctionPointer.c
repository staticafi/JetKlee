// RUN: %clang %s -emit-llvm -g -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --write-no-tests --exit-on-error %t1.bc 2>&1 | FileCheck %s

#include <stdio.h>

void foo(const char *msg) { printf("foo: %s\n", msg); }
void baz(const char *msg) { printf("baz: %s\n", msg); }

void (*xx)(const char *) = foo;

void bar(void (*fp)(const char *)) { fp("called via bar"); }

int main(int argc, char **argv) {
  void (*fp)(const char *) = foo;

  printf("going to call through fp\n");
  fp("called via fp");

  printf("calling via pass through\n");
  bar(foo);
        
  fp = baz;
  fp("called via fp");

  xx("called via xx");

  klee_make_symbolic(&fp, sizeof fp, "fp");
  if(fp == baz) {
    // CHECK: Comparison eq
    klee_warning("Comparison eq");
    // fp("calling via symbolic!");
  }

  return 0;
}
