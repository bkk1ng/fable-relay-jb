use std::fmt;
use std::ops::{Add, Sub, Mul};

/// Fixed-size matrix of f64 with R rows and C columns.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    data: [[f64; C]; R],
}

// ─── Construction ────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Create a matrix from a row-major 2-D array.
    pub fn new(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Create a matrix with every element set to `v`.
    pub fn filled(v: f64) -> Self {
        Self { data: [[v; C]; R] }
    }

    /// Create a zero matrix.
    pub fn zeros() -> Self {
        Self::filled(0.0)
    }

    /// Element access (row-major, 0-indexed).
    #[inline]
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access.
    #[inline]
    pub fn get_mut(&mut self, r: usize, c: usize) -> &mut f64 {
        &mut self.data[r][c]
    }

    /// Set an element.
    #[inline]
    pub fn set(&mut self, r: usize, c: usize, v: f64) {
        self.data[r][c] = v;
    }
}

// ─── Square helpers ───────────────────────────────────────────────────────────

impl<const N: usize> Mat<N, N> {
    /// Identity matrix (only valid for square matrices).
    pub fn identity() -> Self {
        let mut m = Self::zeros();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }

    /// LU decomposition with partial (row) pivoting.
    ///
    /// Decomposes `self` into P·A = L·U where P is a permutation captured in
    /// `perm`.  Returns the determinant.  Call [`LuDecomp::solve`] to solve a
    /// linear system.
    ///
    /// Returns `None` when the matrix is singular (a pivot is exactly zero).
    pub fn lu(&self) -> Option<LuDecomp<N>> {
        LuDecomp::new(self)
    }
}

// ─── Transpose ───────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Return the transpose (dimensions flip from R×C to C×R).
    pub fn transpose(&self) -> Mat<C, R> {
        let mut out = Mat::<C, R>::zeros();
        for r in 0..R {
            for c in 0..C {
                out.data[c][r] = self.data[r][c];
            }
        }
        out
    }
}

// ─── Arithmetic ──────────────────────────────────────────────────────────────

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

impl<const R: usize, const C: usize> Add for &Mat<R, C> {
    type Output = Mat<R, C>;

    fn add(self, rhs: Self) -> Self::Output {
        let mut out = self.clone();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] += rhs.data[r][c];
            }
        }
        out
    }
}

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

impl<const R: usize, const C: usize> Sub for &Mat<R, C> {
    type Output = Mat<R, C>;

    fn sub(self, rhs: Self) -> Self::Output {
        let mut out = self.clone();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] -= rhs.data[r][c];
            }
        }
        out
    }
}

/// Matrix multiplication: (R×K) × (K×C) → (R×C).
///
/// The inner dimension K is enforced at compile time via const generics.
impl<const R: usize, const K: usize, const C: usize> Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zeros();
        for r in 0..R {
            for c in 0..C {
                let mut acc = 0.0_f64;
                for k in 0..K {
                    acc += self.data[r][k] * rhs.data[k][c];
                }
                out.data[r][c] = acc;
            }
        }
        out
    }
}

impl<const R: usize, const K: usize, const C: usize> Mul<&Mat<K, C>> for &Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: &Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zeros();
        for r in 0..R {
            for c in 0..C {
                let mut acc = 0.0_f64;
                for k in 0..K {
                    acc += self.data[r][k] * rhs.data[k][c];
                }
                out.data[r][c] = acc;
            }
        }
        out
    }
}

// ─── Display ─────────────────────────────────────────────────────────────────

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

// ─── LU decomposition ────────────────────────────────────────────────────────

/// Result of LU decomposition with partial pivoting.
///
/// Stores the combined L\U matrix (L has implicit unit diagonal), the
/// permutation vector, and the determinant of the original matrix.
pub struct LuDecomp<const N: usize> {
    /// Combined L and U in-place (L strictly lower, U upper including diagonal).
    lu: Mat<N, N>,
    /// Row permutation: `perm[i]` is the original row now at position `i`.
    perm: [usize; N],
    /// Determinant of the original matrix.
    pub det: f64,
}

impl<const N: usize> LuDecomp<N> {
    /// Perform partial-pivoting LU decomposition on `a`.
    ///
    /// Returns `None` when a zero pivot is encountered (singular matrix).
    pub fn new(a: &Mat<N, N>) -> Option<Self> {
        let mut lu = a.clone();
        let mut perm: [usize; N] = {
            let mut p = [0usize; N];
            for (i, v) in p.iter_mut().enumerate() {
                *v = i;
            }
            p
        };
        let mut sign = 1_i32; // tracks row-swap parity for determinant sign

        for k in 0..N {
            // Find the row with the largest absolute value in column k (at or below k).
            let pivot_row = (k..N)
                .max_by(|&i, &j| {
                    lu.data[i][k]
                        .abs()
                        .partial_cmp(&lu.data[j][k].abs())
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                .unwrap();

            if lu.data[pivot_row][k].abs() < f64::EPSILON {
                return None; // singular
            }

            // Swap rows k and pivot_row in the working matrix and in perm.
            if pivot_row != k {
                lu.data.swap(k, pivot_row);
                perm.swap(k, pivot_row);
                sign = -sign;
            }

            let pivot = lu.data[k][k];

            // Eliminate below the pivot.
            for i in (k + 1)..N {
                lu.data[i][k] /= pivot;
                let factor = lu.data[i][k];
                for j in (k + 1)..N {
                    let sub = factor * lu.data[k][j];
                    lu.data[i][j] -= sub;
                }
            }
        }

        // Determinant = sign × product of diagonal entries of U.
        let det = (sign as f64)
            * (0..N).fold(1.0_f64, |acc, i| acc * lu.data[i][i]);

        Some(Self { lu, perm, det })
    }

    /// Solve A·x = b and return x.
    ///
    /// Uses forward substitution on L and back substitution on U, applying
    /// the stored row permutation to b before solving.
    pub fn solve(&self, b: &[f64; N]) -> [f64; N] {
        // Apply row permutation to b.
        let mut x = [0.0_f64; N];
        for i in 0..N {
            x[i] = b[self.perm[i]];
        }

        // Forward substitution: L·y = Pb  (L has unit diagonal).
        for i in 1..N {
            for j in 0..i {
                x[i] -= self.lu.data[i][j] * x[j];
            }
        }

        // Back substitution: U·x = y.
        for i in (0..N).rev() {
            for j in (i + 1)..N {
                x[i] -= self.lu.data[i][j] * x[j];
            }
            x[i] /= self.lu.data[i][i];
        }

        x
    }
}

// ─── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    const EPS: f64 = 1e-10;

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < EPS
    }

    fn slice_close(a: &[f64], b: &[f64]) -> bool {
        a.len() == b.len() && a.iter().zip(b.iter()).all(|(&x, &y)| close(x, y))
    }

    fn mat_close<const R: usize, const C: usize>(a: &Mat<R, C>, b: &Mat<R, C>) -> bool {
        for r in 0..R {
            for c in 0..C {
                if !close(a.get(r, c), b.get(r, c)) {
                    return false;
                }
            }
        }
        true
    }

    // ── add ──────────────────────────────────────────────────────────────────

    #[test]
    fn add_2x2() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = &a + &b;
        assert!(mat_close(&c, &Mat::new([[6.0, 8.0], [10.0, 12.0]])));
    }

    // ── sub ──────────────────────────────────────────────────────────────────

    #[test]
    fn sub_2x2() {
        let a = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let b = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let c = &a - &b;
        assert!(mat_close(&c, &Mat::new([[4.0, 4.0], [4.0, 4.0]])));
    }

    // ── mul ──────────────────────────────────────────────────────────────────

    #[test]
    fn mul_2x2() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[2.0, 0.0], [1.0, 2.0]]);
        let c = &a * &b;
        assert!(mat_close(&c, &Mat::new([[4.0, 4.0], [10.0, 8.0]])));
    }

    #[test]
    fn mul_dimension_mismatch_checked_by_types() {
        // 2×3 × 3×2 → 2×2  (must compile; dimension safety is at the type level)
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let b: Mat<3, 2> = Mat::new([[7.0, 8.0], [9.0, 10.0], [11.0, 12.0]]);
        let c: Mat<2, 2> = &a * &b;
        // [ 1·7+2·9+3·11,  1·8+2·10+3·12 ] = [58, 64]
        // [ 4·7+5·9+6·11,  4·8+5·10+6·12 ] = [139, 154]
        assert!(mat_close(&c, &Mat::new([[58.0, 64.0], [139.0, 154.0]])));
    }

    // ── transpose ────────────────────────────────────────────────────────────

    #[test]
    fn transpose_2x3() {
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t: Mat<3, 2> = a.transpose();
        assert!(mat_close(
            &t,
            &Mat::new([[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]])
        ));
    }

    #[test]
    fn transpose_involution() {
        let a: Mat<3, 2> = Mat::new([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]);
        assert!(mat_close(&a.transpose().transpose(), &a));
    }

    // ── identity ─────────────────────────────────────────────────────────────

    #[test]
    fn identity_3x3() {
        let id = Mat::<3, 3>::identity();
        for r in 0..3 {
            for c in 0..3 {
                let expected = if r == c { 1.0 } else { 0.0 };
                assert!(close(id.get(r, c), expected));
            }
        }
    }

    #[test]
    fn identity_is_neutral_for_mul() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let id = Mat::<2, 2>::identity();
        assert!(mat_close(&(&a * &id), &a));
        assert!(mat_close(&(&id * &a), &a));
    }

    // ── LU / determinant ─────────────────────────────────────────────────────

    #[test]
    fn det_2x2() {
        // det([[3,8],[4,6]]) = 3·6 − 8·4 = 18 − 32 = −14
        let a = Mat::new([[3.0, 8.0], [4.0, 6.0]]);
        let lu = a.lu().expect("should be invertible");
        assert!(close(lu.det, -14.0));
    }

    #[test]
    fn det_3x3() {
        // det([[2,1,3],[1,4,2],[3,0,1]]) = 2(4-0) − 1(1-6) + 3(0-12) = 8+5-36 = -23
        let a = Mat::new([[2.0, 1.0, 3.0], [1.0, 4.0, 2.0], [3.0, 0.0, 1.0]]);
        let lu = a.lu().expect("should be invertible");
        assert!(close(lu.det, -23.0));
    }

    #[test]
    fn solve_3x3() {
        // A·x = b  where x = [1, 2, 3]^T
        let a = Mat::new([[2.0, 1.0, -1.0], [-3.0, -1.0, 2.0], [-2.0, 1.0, 2.0]]);
        let b = [8.0_f64, -11.0, -3.0];
        let lu = a.lu().expect("should be invertible");
        let x = lu.solve(&b);
        assert!(slice_close(&x, &[2.0, 3.0, -1.0]));
    }

    #[test]
    fn lu_round_trip() {
        // Verify P·A = L·U by reconstructing from the LU factors.
        let a = Mat::new([[4.0, 3.0], [6.0, 3.0]]);
        let lu_res = a.lu().expect("should be invertible");

        // Extract L and U from the combined store.
        let mut l = Mat::<2, 2>::identity();
        let mut u = Mat::<2, 2>::zeros();
        for r in 0..2 {
            for c in 0..2 {
                if r > c {
                    l.set(r, c, lu_res.lu.get(r, c));
                } else {
                    u.set(r, c, lu_res.lu.get(r, c));
                }
            }
        }

        // Reconstruct A permuted by perm.
        let mut pa = Mat::<2, 2>::zeros();
        for r in 0..2 {
            for c in 0..2 {
                pa.set(r, c, a.get(lu_res.perm[r], c));
            }
        }

        assert!(mat_close(&(&l * &u), &pa));
    }

    // ── non-invertible matrix ─────────────────────────────────────────────────

    #[test]
    fn singular_matrix_returns_none() {
        // Rows are linearly dependent: row 1 = 2 × row 0.
        let a = Mat::new([[1.0, 2.0], [2.0, 4.0]]);
        assert!(a.lu().is_none(), "singular matrix should return None");
    }

    #[test]
    fn singular_3x3_returns_none() {
        // Last row is sum of first two.
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [5.0, 7.0, 9.0],
        ]);
        assert!(a.lu().is_none(), "singular 3×3 should return None");
    }

    // ── zero matrix ──────────────────────────────────────────────────────────

    #[test]
    fn zero_matrix_is_singular() {
        let a = Mat::<3, 3>::zeros();
        assert!(a.lu().is_none());
    }
}
