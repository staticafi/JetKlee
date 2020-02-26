// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

#include <stdlib.h>
#include <assert.h>

int main(void) {
	int * a = malloc(4);
	free(a);
	int * b = malloc(4);
	assert(a != b); // Should fork execution
	klee_warning("assert didn't stop execution"); // CHECK: assert didn't stop
	return 0;
}
