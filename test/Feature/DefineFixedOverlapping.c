// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// XFAIL: *
// RUN: %klee --output-dir=%t.klee-out %t1.bc 2> %t1.log
// RUN: FileCheck --input-file %t1.log %s

#include <stdio.h>

#define ADDRESS ((int*) 0x0080)
#define ADDRESS2 ((int*) 0x0084)

int main() {
	klee_define_fixed_object(ADDRESS, 8);
	klee_define_fixed_object(ADDRESS2, 4); // CHECK: Trying to allocate an overlapping object
	int *p = ADDRESS;
	int *r = ADDRESS2; 
}
