#ifndef MATHLIB_H
#define MATHLIB_H

/*
 * mathlib.h — simple integer math utilities
 * C11, no external dependencies.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ml_add — return a + b
 */
int ml_add(int a, int b);

/**
 * ml_mul — return a * b
 */
int ml_mul(int a, int b);

/**
 * ml_clamp — clamp value to [lo, hi] (inclusive)
 * Behaviour is undefined when lo > hi.
 */
int ml_clamp(int value, int lo, int hi);

#ifdef __cplusplus
}
#endif

#endif /* MATHLIB_H */
