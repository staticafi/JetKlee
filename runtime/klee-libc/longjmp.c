#include "klee/klee.h"

int _setjmp () {
    klee_warning_once("ignoring");
    return 0;
}

void longjmp() {
    klee_report_error(__FILE__, __LINE__, "longjmp unsupported", "xxx.err");
}
