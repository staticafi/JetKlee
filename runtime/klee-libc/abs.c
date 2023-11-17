#include <stdint.h>

int abs(int j)
{
	return (j >= 0) ? j : -j;
}

long int labs(long int j)
{
	return (j >= 0) ? j : -j;
}

intmax_t imaxabs(intmax_t j)
{
	return (j >= 0) ? j : -j;
}
