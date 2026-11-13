#include "mathlib.h"

int math_add(int a, int b)
{
    return a + b;
}

int math_mul(int a, int b)
{
    return a * b;
}

int math_clamp(int value, int min_val, int max_val)
{
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}
