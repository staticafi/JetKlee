// RUN: %clang -emit-llvm -c -o %t1.bc %s
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t1.bc

#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main() {
	const char* p = "123abc";
	char* endp;
	long num = strtol(p, &endp, 10);
	assert(num == 123);
	assert(endp == p + 3);

	const char* isNull = strpbrk(p, "d");
	const char*	notNull = strpbrk(p, "b");
	assert(isNull == NULL);	
	assert(notNull == p + 4);

	char cpy[7];
	strcpy(cpy, p);
	assert(strcmp(cpy, p) == 0);
}
