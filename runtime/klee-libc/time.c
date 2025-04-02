#include <unistd.h>
#include <time.h>

extern void klee_assume(int);
extern void klee_make_symbolic(void *, size_t, const char *);

time_t time(time_t *tloc) {
	time_t retval;
	klee_make_symbolic(&retval, sizeof(retval), "time");

	if (tloc) {
		if (retval < 0)
			return ((time_t)-1);
		*tloc = retval;
	} else {
		klee_assume(retval >= 0);
	}

	return retval;
}
