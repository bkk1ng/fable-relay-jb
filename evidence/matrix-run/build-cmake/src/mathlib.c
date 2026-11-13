#include "mathlib.h"

int ml_add(int a, int b)
{
    return a + b;
}

int ml_mul(int a, int b)
{
    return a * b;
}

int ml_clamp(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}
