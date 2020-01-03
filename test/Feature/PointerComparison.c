// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out %t1.bc 2>&1 | FileCheck %s

int main() {
	int a,b;
	if (&a < &b) {
		klee_warning("First branch reached"); // CHECK: First branch
	} else {
		klee_warning("Second branch reached"); // CHECK: Second branch
	}
	return 0;
}
