use std::fmt;
use std::ops::{Add, Mul, Sub};

/// A fixed-size R×C matrix of f64 values.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    data: [[f64; C]; R],
}

// ── Construction ─────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Create a matrix from a row-major 2-D array.
    pub fn new(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Create a matrix with every element set to `v`.
    pub fn filled(v: f64) -> Self {
        Self {
            data: [[v; C]; R],
        }
    }

    /// Create a zero matrix.
    pub fn zero() -> Self {
        Self::filled(0.0)
    }

    /// Element access (row `r`, column `c`).
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access.
    pub fn get_mut(&mut self, r: usize, c: usize) -> &mut f64 {
        &mut self.data[r][c]
    }

    /// Set an element.
    pub fn set(&mut self, r: usize, c: usize, v: f64) {
        self.data[r][c] = v;
    }

    /// Return the transpose (C×R).
    pub fn transpose(&self) -> Mat<C, R> {
        let mut out = Mat::<C, R>::zero();
        for r in 0..R {
            for c in 0..C {
                out.data[c][r] = self.data[r][c];
            }
        }
        out
    }
}

// ── Square-only operations ────────────────────────────────────────────────────

impl<const N: usize> Mat<N, N> {
    /// Return the N×N identity matrix.
    pub fn identity() -> Self {
        let mut m = Self::zero();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }

    /// LU decomposition with partial (row) pivoting.
    ///
    /// Decomposes `self` into P·A = L·U where:
    /// - `L` is unit lower triangular,
    /// - `U` is upper triangular,
    /// - `P` is encoded as a permutation vector.
    ///
    /// Returns `(lu, perm, det)`:
    /// - `lu`   – the compact LU storage (L below diagonal, U on and above),
    /// - `perm` – row permutation (`perm[i]` = original row now at row `i`),
    /// - `det`  – determinant of `self` (0.0 when singular / near-singular).
    ///
    /// Uses an absolute pivot threshold of `1e-12`.
    pub fn lu(&self) -> (Mat<N, N>, [usize; N], f64) {
        const EPS: f64 = 1e-12;

        let mut lu = self.clone();
        let mut perm: [usize; N] = std::array::from_fn(|i| i);
        let mut det_sign = 1.0_f64;

        for k in 0..N {
            // Find the pivot row (largest absolute value in column k, rows k..N).
            let mut pivot_row = k;
            let mut pivot_val = lu.data[k][k].abs();
            for i in (k + 1)..N {
                let v = lu.data[i][k].abs();
                if v > pivot_val {
                    pivot_val = v;
                    pivot_row = i;
                }
            }

            if pivot_row != k {
                lu.data.swap(k, pivot_row);
                perm.swap(k, pivot_row);
                det_sign = -det_sign;
            }

            let diag = lu.data[k][k];
            if diag.abs() < EPS {
                // Singular (or effectively singular) — determinant is 0.
                return (lu, perm, 0.0);
            }

            // Eliminate below the pivot.
            for i in (k + 1)..N {
                let factor = lu.data[i][k] / diag;
                lu.data[i][k] = factor; // store L multiplier
                for j in (k + 1)..N {
                    let sub = factor * lu.data[k][j];
                    lu.data[i][j] -= sub;
                }
            }
        }

        // Determinant = sign × product of U diagonal entries.
        let mut det = det_sign;
        for i in 0..N {
            det *= lu.data[i][i];
        }

        (lu, perm, det)
    }

    /// Solve A·x = b using the LU decomposition returned by [`Mat::lu`].
    ///
    /// # Panics
    /// Panics if `self` is singular (det ≈ 0).
    pub fn solve(&self, b: [f64; N]) -> [f64; N] {
        const EPS: f64 = 1e-12;

        let (lu, perm, det) = self.lu();
        assert!(
            det.abs() >= EPS,
            "matrix is singular (det = {det}); system has no unique solution"
        );

        // Apply the row permutation to b.
        let mut pb = [0.0_f64; N];
        for i in 0..N {
            pb[i] = b[perm[i]];
        }

        // Forward substitution: solve L·y = Pb.
        // L is unit lower triangular (1s on diagonal, stored multipliers below).
        let mut y = pb;
        for i in 1..N {
            for j in 0..i {
                y[i] -= lu.data[i][j] * y[j];
            }
        }

        // Back substitution: solve U·x = y.
        let mut x = y;
        for i in (0..N).rev() {
            for j in (i + 1)..N {
                x[i] -= lu.data[i][j] * x[j];
            }
            x[i] /= lu.data[i][i];
        }

        x
    }
}

// ── Arithmetic ────────────────────────────────────────────────────────────────

/// Element-wise addition — dimensions are enforced by the type system.
impl<const R: usize, const C: usize> Add for Mat<R, C> {
    type Output = Mat<R, C>;

    fn add(mut self, rhs: Self) -> Self::Output {
        for r in 0..R {
            for c in 0..C {
                self.data[r][c] += rhs.data[r][c];
            }
        }
        self
    }
}

/// Element-wise subtraction — dimensions enforced by the type system.
impl<const R: usize, const C: usize> Sub for Mat<R, C> {
    type Output = Mat<R, C>;

    fn sub(mut self, rhs: Self) -> Self::Output {
        for r in 0..R {
            for c in 0..C {
                self.data[r][c] -= rhs.data[r][c];
            }
        }
        self
    }
}

/// Matrix multiplication: (R×K) × (K×C) → (R×C).
/// Inner dimension K is enforced by the type system.
impl<const R: usize, const K: usize, const C: usize> Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zero();
        for r in 0..R {
            for c in 0..C {
                let mut sum = 0.0_f64;
                for k in 0..K {
                    sum += self.data[r][k] * rhs.data[k][c];
                }
                out.data[r][c] = sum;
            }
        }
        out
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> fmt::Display for Mat<R, C> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for r in 0..R {
            write!(f, "[")?;
            for c in 0..C {
                if c > 0 {
                    write!(f, ", ")?;
                }
                write!(f, "{:8.4}", self.data[r][c])?;
            }
            writeln!(f, "]")?;
        }
        Ok(())
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    fn vec_approx_eq<const N: usize>(a: [f64; N], b: [f64; N]) -> bool {
        a.iter().zip(b.iter()).all(|(x, y)| approx_eq(*x, *y))
    }

    fn mat_approx_eq<const R: usize, const C: usize>(
        a: &Mat<R, C>,
        b: &Mat<R, C>,
    ) -> bool {
        for r in 0..R {
            for c in 0..C {
                if !approx_eq(a.get(r, c), b.get(r, c)) {
                    return false;
                }
            }
        }
        true
    }

    // ── add / sub ─────────────────────────────────────────────────────────────

    #[test]
    fn test_add() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a + b;
        assert!(mat_approx_eq(
            &c,
            &Mat::new([[6.0, 8.0], [10.0, 12.0]])
        ));
    }

    #[test]
    fn test_sub() {
        let a = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let b = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let c = a - b;
        assert!(mat_approx_eq(
            &c,
            &Mat::new([[4.0, 4.0], [4.0, 4.0]])
        ));
    }

    // ── mul ───────────────────────────────────────────────────────────────────

    #[test]
    fn test_mul_square() {
        // [[1,2],[3,4]] × [[5,6],[7,8]] = [[19,22],[43,50]]
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a * b;
        assert!(mat_approx_eq(
            &c,
            &Mat::new([[19.0, 22.0], [43.0, 50.0]])
        ));
    }

    #[test]
    fn test_mul_non_square() {
        // (2×3) × (3×2)
        let a: Mat<2, 3> = Mat::new([[1.0, 0.0, 2.0], [0.0, 3.0, -1.0]]);
        let b: Mat<3, 2> = Mat::new([[1.0, 2.0], [3.0, 1.0], [0.0, 4.0]]);
        let c = a * b; // should be 2×2
        // row 0: [1*1+0*3+2*0, 1*2+0*1+2*4] = [1, 10]
        // row 1: [0*1+3*3+(-1)*0, 0*2+3*1+(-1)*4] = [9, -1]
        assert!(mat_approx_eq(
            &c,
            &Mat::new([[1.0, 10.0], [9.0, -1.0]])
        ));
    }

    // ── identity ──────────────────────────────────────────────────────────────

    #[test]
    fn test_identity() {
        let i: Mat<3, 3> = Mat::identity();
        assert!(mat_approx_eq(
            &i,
            &Mat::new([
                [1.0, 0.0, 0.0],
                [0.0, 1.0, 0.0],
                [0.0, 0.0, 1.0]
            ])
        ));
    }

    #[test]
    fn test_identity_mul() {
        let a = Mat::new([[2.0, 3.0], [4.0, 5.0]]);
        let i = Mat::<2, 2>::identity();
        let result = a.clone() * i;
        assert!(mat_approx_eq(&result, &a));
    }

    // ── transpose ─────────────────────────────────────────────────────────────

    #[test]
    fn test_transpose_square() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let t = a.transpose();
        assert!(mat_approx_eq(
            &t,
            &Mat::new([[1.0, 3.0], [2.0, 4.0]])
        ));
    }

    #[test]
    fn test_transpose_non_square() {
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t: Mat<3, 2> = a.transpose();
        assert!(mat_approx_eq(
            &t,
            &Mat::new([[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]])
        ));
    }

    #[test]
    fn test_double_transpose_is_identity() {
        let a = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let tt = a.clone().transpose().transpose();
        assert!(mat_approx_eq(&tt, &a));
    }

    // ── LU / determinant ──────────────────────────────────────────────────────

    #[test]
    fn test_det_2x2() {
        // det([[3,8],[4,6]]) = 3*6 - 8*4 = 18 - 32 = -14
        let a = Mat::new([[3.0, 8.0], [4.0, 6.0]]);
        let (_, _, det) = a.lu();
        assert!(approx_eq(det, -14.0));
    }

    #[test]
    fn test_det_3x3() {
        // det([[1,2,3],[0,1,4],[5,6,0]]) = 1*(0-24)-2*(0-20)+3*(0-5) = -24+40-15 = 1
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [0.0, 1.0, 4.0],
            [5.0, 6.0, 0.0],
        ]);
        let (_, _, det) = a.lu();
        assert!(approx_eq(det, 1.0), "det = {det}");
    }

    #[test]
    fn test_det_identity() {
        let i = Mat::<4, 4>::identity();
        let (_, _, det) = i.lu();
        assert!(approx_eq(det, 1.0));
    }

    // ── non-invertible matrix ─────────────────────────────────────────────────

    #[test]
    fn test_non_invertible_det_zero() {
        // Rows 2 and 3 are linearly dependent.
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        let (_, _, det) = a.lu();
        assert!(
            approx_eq(det, 0.0),
            "expected det ≈ 0 for singular matrix, got {det}"
        );
    }

    #[test]
    #[should_panic(expected = "singular")]
    fn test_solve_singular_panics() {
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        let _x = a.solve([1.0, 2.0, 3.0]);
    }

    // ── solver ────────────────────────────────────────────────────────────────

    #[test]
    fn test_solve_2x2() {
        // 2x + y = 5
        // 5x + 7y = 3  →  x = 32/9, y = -13/9
        let a = Mat::new([[2.0, 1.0], [5.0, 7.0]]);
        let b = [5.0, 3.0];
        let x = a.solve(b);
        assert!(
            vec_approx_eq(x, [32.0 / 9.0, -13.0 / 9.0]),
            "got x = {x:?}"
        );
    }

    #[test]
    fn test_solve_3x3() {
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [0.0, 1.0, 4.0],
            [5.0, 6.0, 0.0],
        ]);
        let b = [14.0, 14.0, 10.0]; // solution: x=[1,2,3] → 1+4+9=14, 2+12=14, 5+12=17 … let's compute
        // Actually compute a correct rhs: x=[1,2,3]
        // row0: 1+4+9 = 14 ✓
        // row1: 0+2+12 = 14 ✓
        // row2: 5+12+0 = 17 — fix rhs
        let b2 = [14.0, 14.0, 17.0];
        let x = a.solve(b2);
        assert!(vec_approx_eq(x, [1.0, 2.0, 3.0]), "got x = {x:?}");
    }

    #[test]
    fn test_solve_roundtrip() {
        // A·x = b, then verify A·(solved x) ≈ b
        let a = Mat::new([
            [4.0, 3.0, 0.0],
            [3.0, 4.0, -1.0],
            [0.0, -1.0, 4.0],
        ]);
        let b = [24.0, 30.0, -11.0];
        let x = a.solve(b);
        // Multiply back
        let ax = a.clone() * {
            // Build a 3×1 matrix for the multiplication
            let mut col: Mat<3, 1> = Mat::zero();
            for i in 0..3 {
                col.set(i, 0, x[i]);
            }
            col
        };
        for i in 0..3 {
            assert!(
                approx_eq(ax.get(i, 0), b[i]),
                "row {i}: A·x[{i}] = {} ≠ b[{i}] = {}",
                ax.get(i, 0),
                b[i]
            );
        }
    }

    // ── LU reconstructs A ─────────────────────────────────────────────────────

    #[test]
    fn test_lu_reconstructs_a() {
        // Verify P·A = L·U by rebuilding L, U, P explicitly.
        let a = Mat::new([
            [2.0, 1.0, -1.0],
            [-3.0, -1.0, 2.0],
            [-2.0, 1.0, 2.0],
        ]);
        let (lu, perm, _) = a.lu();

        // Extract L (unit lower triangular) and U (upper triangular).
        let mut l = Mat::<3, 3>::identity();
        let mut u = Mat::<3, 3>::zero();
        for r in 0..3 {
            for c in 0..3 {
                if r > c {
                    l.set(r, c, lu.get(r, c));
                } else {
                    u.set(r, c, lu.get(r, c));
                }
            }
        }

        // Compute L·U
        let lu_product = l * u;

        // Build P·A (permute rows of a according to perm).
        let mut pa = Mat::<3, 3>::zero();
        for r in 0..3 {
            for c in 0..3 {
                pa.set(r, c, a.get(perm[r], c));
            }
        }

        assert!(
            mat_approx_eq(&lu_product, &pa),
            "L·U ≠ P·A\nL·U =\n{lu_product}\nP·A =\n{pa}"
        );
    }
}
