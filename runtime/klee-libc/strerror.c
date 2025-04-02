char *strerror(int errnum) {
	(void) errnum;
	static char err[] = "dummy strerror retval";
	return err;
}
