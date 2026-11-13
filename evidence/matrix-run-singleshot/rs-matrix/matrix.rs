/// Fixed-size generic matrix over f64.
///
/// `R` is the number of rows, `C` is the number of columns.
/// All arithmetic is done on `f64` values.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    /// Row-major storage: `data[r][c]` is row `r`, column `c`.
    pub data: [[f64; C]; R],
}

// ── Construction ─────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Create a matrix filled with zeros.
    pub fn zeros() -> Self {
        Self { data: [[0.0; C]; R] }
    }

    /// Create a matrix from a row-major 2-D array.
    pub fn new(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Return the transpose: a `Mat<C, R>`.
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

impl<const N: usize> Mat<N, N> {
    /// Return the N×N identity matrix.
    pub fn identity() -> Self {
        let mut m = Self::zeros();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }

    /// LU decomposition with partial (row) pivoting.
    ///
    /// Decomposes `self` into `P * A = L * U` in-place and returns:
    /// - `lu`: the combined L\U matrix (L has implicit unit diagonal),
    /// - `piv`: the pivot permutation vector (`piv[i]` = row that was
    ///   swapped into row `i`),
    /// - `det`: the determinant of the original matrix.
    ///
    /// Returns `None` if the matrix is singular (a pivot of exactly 0.0
    /// is encountered after pivoting).
    pub fn lu_decompose(&self) -> Option<(Mat<N, N>, [usize; N], f64)> {
        let mut lu = self.clone();
        let mut piv: [usize; N] = std::array::from_fn(|i| i);
        let mut det_sign: f64 = 1.0;

        for k in 0..N {
            // Find the row with the largest absolute value in column k
            // at or below the current pivot row.
            let mut max_val = lu.data[k][k].abs();
            let mut max_row = k;
            for i in (k + 1)..N {
                let v = lu.data[i][k].abs();
                if v > max_val {
                    max_val = v;
                    max_row = i;
                }
            }

            // Swap rows if needed.
            if max_row != k {
                lu.data.swap(k, max_row);
                piv.swap(k, max_row);
                det_sign = -det_sign;
            }

            let pivot = lu.data[k][k];
            if pivot == 0.0 {
                return None; // Singular matrix.
            }

            // Eliminate below the pivot.
            for i in (k + 1)..N {
                lu.data[i][k] /= pivot;
                let factor = lu.data[i][k];
                for j in (k + 1)..N {
                    lu.data[i][j] -= factor * lu.data[k][j];
                }
            }
        }

        // Determinant = sign × product of diagonal entries of U.
        let det = det_sign
            * (0..N).map(|i| lu.data[i][i]).product::<f64>();

        Some((lu, piv, det))
    }

    /// Solve `A * x = b` using LU decomposition with partial pivoting.
    ///
    /// Returns `None` if `self` is singular.
    pub fn solve(&self, b: [f64; N]) -> Option<[f64; N]> {
        let (lu, piv, _) = self.lu_decompose()?;

        // Apply the same row permutation to b.
        let mut x: [f64; N] = std::array::from_fn(|i| b[piv[i]]);

        // Forward substitution: solve L * y = Pb.
        // L has an implicit unit diagonal; below-diagonal entries are in `lu`.
        for i in 1..N {
            for j in 0..i {
                x[i] -= lu.data[i][j] * x[j];
            }
        }

        // Back substitution: solve U * x = y.
        for i in (0..N).rev() {
            for j in (i + 1)..N {
                x[i] -= lu.data[i][j] * x[j];
            }
            x[i] /= lu.data[i][i];
        }

        Some(x)
    }
}

// ── Operator: Add ─────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::ops::Add for Mat<R, C> {
    type Output = Self;
    fn add(self, rhs: Self) -> Self {
        let mut out = Self::zeros();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] + rhs.data[r][c];
            }
        }
        out
    }
}

// ── Operator: Sub ─────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::ops::Sub for Mat<R, C> {
    type Output = Self;
    fn sub(self, rhs: Self) -> Self {
        let mut out = Self::zeros();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] - rhs.data[r][c];
            }
        }
        out
    }
}

// ── Operator: Mul ─────────────────────────────────────────────────────────────
//
// Mat<R, K> * Mat<K, C> → Mat<R, C>
//
// The inner dimension `K` is enforced at compile time by the const-generic
// bounds.  No runtime assert is needed.

impl<const R: usize, const K: usize, const C: usize> std::ops::Mul<Mat<K, C>>
    for Mat<R, K>
{
    type Output = Mat<R, C>;
    fn mul(self, rhs: Mat<K, C>) -> Mat<R, C> {
        let mut out = Mat::<R, C>::zeros();
        for r in 0..R {
            for c in 0..C {
                for k in 0..K {
                    out.data[r][c] += self.data[r][k] * rhs.data[k][c];
                }
            }
        }
        out
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::fmt::Display for Mat<R, C> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
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

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-10
    }

    fn approx_eq_arr<const N: usize>(a: [f64; N], b: [f64; N]) -> bool {
        a.iter().zip(b.iter()).all(|(x, y)| approx_eq(*x, *y))
    }

    fn approx_eq_mat<const R: usize, const C: usize>(
        a: &Mat<R, C>,
        b: &Mat<R, C>,
    ) -> bool {
        for r in 0..R {
            for c in 0..C {
                if !approx_eq(a.data[r][c], b.data[r][c]) {
                    return false;
                }
            }
        }
        true
    }

    // ── add / sub ──────────────────────────────────────────────────────────

    #[test]
    fn test_add() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a + b;
        assert_eq!(c.data, [[6.0, 8.0], [10.0, 12.0]]);
    }

    #[test]
    fn test_sub() {
        let a = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let b = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let c = a - b;
        assert_eq!(c.data, [[4.0, 4.0], [4.0, 4.0]]);
    }

    // ── mul ───────────────────────────────────────────────────────────────

    #[test]
    fn test_mul_square() {
        // [ 1 2 ] * [ 5 6 ] = [ 19 22 ]
        // [ 3 4 ]   [ 7 8 ]   [ 43 50 ]
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a * b;
        assert_eq!(c.data, [[19.0, 22.0], [43.0, 50.0]]);
    }

    #[test]
    fn test_mul_non_square() {
        // Mat<2,3> * Mat<3,2> → Mat<2,2>
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let b: Mat<3, 2> = Mat::new([[7.0, 8.0], [9.0, 10.0], [11.0, 12.0]]);
        let c: Mat<2, 2> = a * b;
        // Row 0: 1*7+2*9+3*11=58, 1*8+2*10+3*12=64
        // Row 1: 4*7+5*9+6*11=139, 4*8+5*10+6*12=154
        assert_eq!(c.data, [[58.0, 64.0], [139.0, 154.0]]);
    }

    // ── transpose ─────────────────────────────────────────────────────────

    #[test]
    fn test_transpose_square() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let t = a.transpose();
        assert_eq!(t.data, [[1.0, 3.0], [2.0, 4.0]]);
    }

    #[test]
    fn test_transpose_rectangular() {
        let a: Mat<2, 3> = Mat::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t: Mat<3, 2> = a.transpose();
        assert_eq!(t.data, [[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]]);
    }

    // ── identity ──────────────────────────────────────────────────────────

    #[test]
    fn test_identity() {
        let id = Mat::<3, 3>::identity();
        assert_eq!(id.data, [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]);
    }

    #[test]
    fn test_identity_mul() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let id = Mat::<2, 2>::identity();
        let result = a.clone() * id;
        assert!(approx_eq_mat(&result, &a));
    }

    // ── LU / determinant ──────────────────────────────────────────────────

    #[test]
    fn test_lu_det_2x2() {
        // det([[3,8],[4,6]]) = 18 - 32 = -14
        let a = Mat::new([[3.0, 8.0], [4.0, 6.0]]);
        let (_, _, det) = a.lu_decompose().expect("should not be singular");
        assert!(approx_eq(det, -14.0), "det = {det}");
    }

    #[test]
    fn test_lu_det_3x3() {
        // Known: det([[6,1,1],[4,-2,5],[2,8,7]]) = -306
        let a = Mat::new([[6.0, 1.0, 1.0], [4.0, -2.0, 5.0], [2.0, 8.0, 7.0]]);
        let (_, _, det) = a.lu_decompose().expect("should not be singular");
        assert!(approx_eq(det, -306.0), "det = {det}");
    }

    #[test]
    fn test_lu_singular() {
        // Rows 0 and 1 are proportional → singular.
        let a = Mat::new([[1.0, 2.0], [2.0, 4.0]]);
        assert!(a.lu_decompose().is_none(), "expected None for singular matrix");
    }

    // ── solver ────────────────────────────────────────────────────────────

    #[test]
    fn test_solve_2x2() {
        // 2x + y = 5
        // 4x + 3y = 11  →  x=2, y=1
        let a = Mat::new([[2.0, 1.0], [4.0, 3.0]]);
        let b = [5.0, 11.0];
        let x = a.solve(b).expect("should not be singular");
        assert!(approx_eq_arr(x, [2.0, 1.0]), "x = {x:?}");
    }

    #[test]
    fn test_solve_3x3() {
        // x + y + z = 6
        // 2y + 5z = -4
        // 2x + 5y - z = 27  →  x=5, y=3, z=-2
        let a = Mat::new([
            [1.0, 1.0,  1.0],
            [0.0, 2.0,  5.0],
            [2.0, 5.0, -1.0],
        ]);
        let b = [6.0, -4.0, 27.0];
        let x = a.solve(b).expect("should not be singular");
        assert!(approx_eq_arr(x, [5.0, 3.0, -2.0]), "x = {x:?}");
    }

    #[test]
    fn test_solve_singular_returns_none() {
        let a = Mat::new([[1.0, 2.0], [2.0, 4.0]]);
        assert!(a.solve([1.0, 2.0]).is_none());
    }

    // ── A * A⁻¹ = I via solve ─────────────────────────────────────────────

    #[test]
    fn test_inverse_via_solve() {
        let a = Mat::new([[4.0, 7.0], [2.0, 6.0]]);
        // Solve for each column of the identity.
        let col0 = a.solve([1.0, 0.0]).unwrap();
        let col1 = a.solve([0.0, 1.0]).unwrap();
        let inv = Mat::new([[col0[0], col1[0]], [col0[1], col1[1]]]);
        let product = a * inv;
        let id = Mat::<2, 2>::identity();
        assert!(approx_eq_mat(&product, &id));
    }

    // ── zeros / display smoke test ─────────────────────────────────────────

    #[test]
    fn test_zeros() {
        let z = Mat::<2, 3>::zeros();
        for row in &z.data {
            for &v in row {
                assert_eq!(v, 0.0);
            }
        }
    }

    #[test]
    fn test_display_does_not_panic() {
        let a = Mat::new([[1.0, 2.0], [3.0, 4.0]]);
        let _ = format!("{a}");
    }
}
