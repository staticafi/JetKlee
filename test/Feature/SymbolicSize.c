// RUN: %llvmgcc %s -g -emit-llvm -O0 -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t1.bc


#include <assert.h>

int main() {
    int size;
    klee_make_symbolic(&size, sizeof(size), "size");
    int array[size];
    if (size >= 0 && size < 5) {
        for (int i = size - 1; i >= 0; i--)
            array[i] = 3;
        for (int i = 0; i < size; i++)
            assert(array[i] == 3);
    }
    return 0;
}
