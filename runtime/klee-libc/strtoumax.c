unsigned long long
strtoull(const char * nptr, char ** endptr, int base);

/*
 * Convert a string to an unsigned long integer.
 *
 * Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
unsigned long long
strtoumax(const char * nptr, char ** endptr, int base)
{
	return strtoull(nptr, endptr, base);
}

