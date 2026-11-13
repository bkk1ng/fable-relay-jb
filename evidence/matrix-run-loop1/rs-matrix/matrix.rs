use std::fmt;
use std::ops::{Add, Sub, Mul};

/// Fixed-size generic matrix over f64.
///
/// `R` = number of rows, `C` = number of columns.
/// Storage is row-major: `data[r][c]`.
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

    /// Create a zero matrix.
    pub fn zero() -> Self {
        Self {
            data: [[0.0; C]; R],
        }
    }

    /// Element access (row, col).
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access (row, col).
    pub fn get_mut(&mut self, r: usize, c: usize) -> &mut f64 {
        &mut self.data[r][c]
    }

    /// Transpose: returns a new `Mat<C, R>`.
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

// ── Identity (square matrices only) ──────────────────────────────────────────

impl<const N: usize> Mat<N, N> {
    /// Return the N×N identity matrix.
    pub fn identity() -> Self {
        let mut m = Self::zero();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> fmt::Display for Mat<R, C> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for row in &self.data {
            write!(f, "[")?;
            for (i, v) in row.iter().enumerate() {
                if i > 0 {
                    write!(f, ", ")?;
                }
                write!(f, "{v:8.4}")?;
            }
            writeln!(f, "]")?;
        }
        Ok(())
    }
}

// ── Add ───────────────────────────────────────────────────────────────────────

/// Element-wise addition. Dimension match is enforced at compile time
/// because both operands are `Mat<R, C>`.
impl<const R: usize, const C: usize> Add for Mat<R, C> {
    type Output = Self;

    fn add(self, rhs: Self) -> Self::Output {
        let mut out = Self::zero();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] + rhs.data[r][c];
            }
        }
        out
    }
}

impl<const R: usize, const C: usize> Add for &Mat<R, C> {
    type Output = Mat<R, C>;

    fn add(self, rhs: Self) -> Self::Output {
        let mut out = Mat::<R, C>::zero();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] + rhs.data[r][c];
            }
        }
        out
    }
}

// ── Sub ───────────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Sub for Mat<R, C> {
    type Output = Self;

    fn sub(self, rhs: Self) -> Self::Output {
        let mut out = Self::zero();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] - rhs.data[r][c];
            }
        }
        out
    }
}

impl<const R: usize, const C: usize> Sub for &Mat<R, C> {
    type Output = Mat<R, C>;

    fn sub(self, rhs: Self) -> Self::Output {
        let mut out = Mat::<R, C>::zero();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] - rhs.data[r][c];
            }
        }
        out
    }
}

// ── Mul ───────────────────────────────────────────────────────────────────────

/// Matrix multiplication `Mat<R, K> * Mat<K, C> -> Mat<R, C>`.
///
/// The inner dimension `K` is checked at compile time by the trait bound
/// `Mat<K, C>` on the rhs type parameter — the compiler will reject any
/// multiplication whose inner dimensions do not match.
impl<const R: usize, const K: usize, const C: usize> Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: Mat<K, C>) -> Self::Output {
        &self * &rhs
    }
}

impl<const R: usize, const K: usize, const C: usize> Mul<&Mat<K, C>> for &Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: &Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zero();
        for r in 0..R {
            for k in 0..K {
                for c in 0..C {
                    out.data[r][c] += self.data[r][k] * rhs.data[k][c];
                }
            }
        }
        out
    }
}

// ── LU decomposition with partial pivoting ───────────────────────────────────

/// Result of an LU decomposition with partial pivoting.
///
/// The decomposition satisfies `P * A = L * U` where:
/// - `L` is unit-lower-triangular (diagonal ones stored implicitly),
/// - `U` is upper-triangular,
/// - `pivot[i]` records which original row was swapped into position `i`.
///
/// When `singular` is true, `A` is (numerically) singular: the determinant is
/// zero and `solve` will panic.
pub struct LU<const N: usize> {
    /// Combined L\U storage (L below diagonal, U on and above).
    lu: Mat<N, N>,
    /// Row-permutation: `pivot[i]` is the original row index now at row `i`.
    pivot: [usize; N],
    /// Sign of the permutation: +1 or -1, used for the determinant.
    parity: f64,
    /// True when a zero pivot was encountered.
    pub singular: bool,
}

impl<const N: usize> LU<N> {
    /// Decompose `a` in-place using partial (row) pivoting.
    ///
    /// Returns the `LU` structure together with the determinant of `a`.
    pub fn decompose(a: &Mat<N, N>) -> (Self, f64) {
        let mut lu = a.clone();
        let mut pivot = [0usize; N];
        for i in 0..N {
            pivot[i] = i;
        }
        let mut parity = 1.0_f64;
        let mut singular = false;

        for k in 0..N {
            // ── find pivot row ────────────────────────────────────────────────
            let mut max_val = lu.data[k][k].abs();
            let mut max_row = k;
            for i in (k + 1)..N {
                if lu.data[i][k].abs() > max_val {
                    max_val = lu.data[i][k].abs();
                    max_row = i;
                }
            }

            // ── swap rows if needed ───────────────────────────────────────────
            if max_row != k {
                lu.data.swap(k, max_row);
                pivot.swap(k, max_row);
                parity = -parity;
            }

            let pivot_val = lu.data[k][k];
            if pivot_val.abs() < f64::EPSILON {
                singular = true;
                // Continue so the rest of U is still filled in.
                continue;
            }

            // ── eliminate below pivot ─────────────────────────────────────────
            for i in (k + 1)..N {
                lu.data[i][k] /= pivot_val;
                for j in (k + 1)..N {
                    let factor = lu.data[i][k];
                    lu.data[i][j] -= factor * lu.data[k][j];
                }
            }
        }

        // ── determinant = parity * product of U's diagonal ───────────────────
        let det = if singular {
            0.0
        } else {
            let mut d = parity;
            for i in 0..N {
                d *= lu.data[i][i];
            }
            d
        };

        (Self { lu, pivot, parity, singular }, det)
    }

    /// Determinant of the original matrix.
    pub fn determinant(&self) -> f64 {
        if self.singular {
            return 0.0;
        }
        let mut d = self.parity;
        for i in 0..N {
            d *= self.lu.data[i][i];
        }
        d
    }

    /// Solve `A * x = b` using the stored LU decomposition.
    ///
    /// # Panics
    /// Panics if the matrix is singular.
    pub fn solve(&self, b: &[f64; N]) -> [f64; N] {
        assert!(!self.singular, "matrix is singular — system has no unique solution");

        // Apply the same row permutation to b.
        let mut x = [0.0_f64; N];
        for i in 0..N {
            x[i] = b[self.pivot[i]];
        }

        // Forward substitution: solve L * y = Pb.
        // L is unit-lower-triangular, so diagonal is implicitly 1.
        for i in 0..N {
            for j in 0..i {
                x[i] -= self.lu.data[i][j] * x[j];
            }
        }

        // Back substitution: solve U * x = y.
        for i in (0..N).rev() {
            for j in (i + 1)..N {
                x[i] -= self.lu.data[i][j] * x[j];
            }
            x[i] /= self.lu.data[i][i];
        }

        x
    }
}

// ── Square-matrix convenience method ─────────────────────────────────────────

impl<const N: usize> Mat<N, N> {
    /// LU-decompose this matrix and return the structure plus the determinant.
    pub fn lu(&self) -> (LU<N>, f64) {
        LU::decompose(self)
    }

    /// Solve `self * x = b` via LU decomposition.
    ///
    /// # Panics
    /// Panics if the matrix is singular.
    pub fn solve(&self, b: &[f64; N]) -> [f64; N] {
        let (lu, _) = LU::decompose(self);
        lu.solve(b)
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    const EPS: f64 = 1e-10;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < EPS
    }

    fn slice_approx_eq(a: &[f64], b: &[f64]) -> bool {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| approx_eq(*x, *y))
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
        assert_eq!(c, Mat::new([[6.0, 8.0], [10.0, 12.0]]));
    }

    #[test]
    fn test_sub() {
        let a = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let b = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let c = a - b;
        assert_eq!(c, Mat::new([[4.0, 4.0], [4.0, 4.0]]));
    }

    // ── mul ───────────────────────────────────────────────────────────────────

    #[test]
    fn test_mul_square() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a * b;
        // [1*5+2*7, 1*6+2*8] = [19, 22]
        // [3*5+4*7, 3*6+4*8] = [43, 50]
        assert_eq!(c, Mat::new([[19.0, 22.0], [43.0, 50.0]]));
    }

    #[test]
    fn test_mul_non_square() {
        // 2×3 * 3×2 -> 2×2
        let a: Mat<2, 3> = Mat::new([[1.0, 0.0, 2.0], [0.0, 3.0, 1.0]]);
        let b: Mat<3, 2> = Mat::new([[1.0, 2.0], [0.0, 1.0], [4.0, 0.0]]);
        let c: Mat<2, 2> = a * b;
        // row0: [1+0+8, 2+0+0] = [9, 2]
        // row1: [0+0+4, 0+3+0] = [4, 3]
        assert_eq!(c, Mat::new([[9.0, 2.0], [4.0, 3.0]]));
    }

    // ── transpose ─────────────────────────────────────────────────────────────

    #[test]
    fn test_transpose() {
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t: Mat<3, 2> = a.transpose();
        assert_eq!(
            t,
            Mat::new([[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]])
        );
    }

    #[test]
    fn test_transpose_square() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let t = a.transpose();
        assert_eq!(t, Mat::new([[1.0, 3.0], [2.0, 4.0]]));
    }

    // ── identity ──────────────────────────────────────────────────────────────

    #[test]
    fn test_identity_3x3() {
        let id: Mat<3, 3> = Mat::identity();
        for r in 0..3 {
            for c in 0..3 {
                let expected = if r == c { 1.0 } else { 0.0 };
                assert_eq!(id.get(r, c), expected);
            }
        }
    }

    #[test]
    fn test_identity_mul() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let id = Mat::<2, 2>::identity();
        assert!(mat_approx_eq(&(&a * &id), &a));
        assert!(mat_approx_eq(&(&id * &a), &a));
    }

    // ── LU: determinant ───────────────────────────────────────────────────────

    #[test]
    fn test_determinant_2x2() {
        // det [[1,2],[3,4]] = -2
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let (_, det) = a.lu();
        assert!(approx_eq(det, -2.0), "det = {det}");
    }

    #[test]
    fn test_determinant_3x3() {
        // det [[2,1,3],[1,2,1],[3,1,2]] = 2*(4-1) - 1*(2-3) + 3*(1-6) = 6+1-15 = -8
        let a = Mat::new([
            [2.0, 1.0, 3.0],
            [1.0, 2.0, 1.0],
            [3.0, 1.0, 2.0],
        ]);
        let (_, det) = a.lu();
        assert!(approx_eq(det, -8.0), "det = {det}");
    }

    // ── LU: non-invertible (singular) matrix ──────────────────────────────────

    #[test]
    fn test_singular_matrix_zero_det() {
        // Rows are linearly dependent: row2 = row0 + row1.
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [5.0, 7.0, 9.0],
        ]);
        let (lu, det) = a.lu();
        assert!(lu.singular, "expected singular flag to be set");
        assert!(approx_eq(det, 0.0), "expected zero determinant, got {det}");
    }

    #[test]
    #[should_panic(expected = "matrix is singular")]
    fn test_solve_singular_panics() {
        let a = Mat::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [5.0, 7.0, 9.0],
        ]);
        // This must panic.
        let _ = a.solve(&[1.0, 2.0, 3.0]);
    }

    // ── LU: solver ────────────────────────────────────────────────────────────

    #[test]
    fn test_solve_2x2() {
        // 2x + y = 5
        // x + 3y = 10
        // solution: x = 1, y = 3
        let a = Mat::new([[2.0, 1.0], [1.0, 3.0]]);
        let b = [5.0, 10.0];
        let x = a.solve(&b);
        assert!(slice_approx_eq(&x, &[1.0, 3.0]), "x = {x:?}");
    }

    #[test]
    fn test_solve_3x3() {
        // A well-conditioned 3×3 system.
        // 2x + 1y + 3z = 1
        // 1x + 2y + 1z = 2
        // 3x + 1y + 2z = 3
        // Computed solution via Cramer / known answer: x=1, y=1, z=-2/3 ... let's just
        // verify A*x == b numerically.
        let a = Mat::new([
            [2.0, 1.0, 3.0],
            [1.0, 2.0, 1.0],
            [3.0, 1.0, 2.0],
        ]);
        let b = [1.0_f64, 2.0, 3.0];
        let x = a.solve(&b);

        // Reconstruct a*x and compare to b.
        let ax = &a * &Mat::new([[x[0]], [x[1]], [x[2]]]);
        for i in 0..3 {
            assert!(approx_eq(ax.get(i, 0), b[i]), "residual at row {i}");
        }
    }

    #[test]
    fn test_solve_identity() {
        let id = Mat::<3, 3>::identity();
        let b = [3.0, 1.0, 4.0];
        let x = id.solve(&b);
        assert!(slice_approx_eq(&x, &b));
    }

    // ── LU: round-trip A = L*U (after permutation) ───────────────────────────

    #[test]
    fn test_lu_reconstruction() {
        // Build L and U explicitly from the lu field and verify P*A == L*U.
        let a = Mat::new([
            [3.0, 1.0, 2.0],
            [6.0, 3.0, 8.0],
            [3.0, 4.0, 10.0],
        ]);
        let (lu_res, _) = a.lu();

        let mut l = Mat::<3, 3>::identity();
        let mut u = Mat::<3, 3>::zero();

        for i in 0..3 {
            for j in 0..3 {
                if i > j {
                    *l.get_mut(i, j) = lu_res.lu.get(i, j);
                } else {
                    *u.get_mut(i, j) = lu_res.lu.get(i, j);
                }
            }
        }

        // Permuted A: row i of PA is row pivot[i] of A.
        let mut pa = Mat::<3, 3>::zero();
        for i in 0..3 {
            for j in 0..3 {
                *pa.get_mut(i, j) = a.get(lu_res.pivot[i], j);
            }
        }

        let lu_product = &l * &u;
        assert!(
            mat_approx_eq(&lu_product, &pa),
            "L*U != P*A\nL*U:\n{lu_product}P*A:\n{pa}"
        );
    }
}
