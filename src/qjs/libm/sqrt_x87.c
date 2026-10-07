// sqrt/sqrtl via the x87 FSQRT instruction (correctly rounded to double
// when the FPU precision control is 53 bits, which wjs sets on entry).
#include <math.h>

long double sqrtl(long double x)
{
	long double r;
	__asm__ ("fsqrt" : "=t"(r) : "0"(x));
	return r;
}

double sqrt(double x)
{
	return (double)sqrtl(x);
}

float sqrtf(float x)
{
	return (float)sqrtl(x);
}
