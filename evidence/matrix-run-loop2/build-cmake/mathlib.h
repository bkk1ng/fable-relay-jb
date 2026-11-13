#ifndef MATHLIB_H
#define MATHLIB_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns a + b.
 */
int math_add(int a, int b);

/**
 * Returns a * b.
 */
int math_mul(int a, int b);

/**
 * Clamps value to [min_val, max_val].
 * Behaviour is defined for min_val <= max_val.
 */
int math_clamp(int value, int min_val, int max_val);

#ifdef __cplusplus
}
#endif

#endif /* MATHLIB_H */
