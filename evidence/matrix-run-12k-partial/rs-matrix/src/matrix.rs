/// Fixed-size generic matrix over f64.
///
/// `R` is the number of rows, `C` the number of columns.
/// All storage is row-major on the stack.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    data: [[f64; C]; R],
}

// ── construction ──────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Zero matrix.
    pub fn zero() -> Self {
        Self { data: [[0.0; C]; R] }
    }

    /// Build from a row-major 2-D array.
    pub fn new(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Element access (row, column).
    #[inline]
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access.
    #[inline]
    pub fn get_mut(&mut self, r: usize, c: usize) -> &mut f64 {
        &mut self.data[r][c]
    }
}

// ── identity (square matrices only) ──────────────────────────────────────────

impl<const N: usize> Mat<N, N> {
    /// Multiplicative identity.
    pub fn identity() -> Self {
        let mut m = Self::zero();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }
}

// ── transpose ─────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Returns the transpose as a `Mat<C, R>`.
    pub fn transpose(&self) -> Mat<C, R> {
        let mut out = Mat::<C, R>::zero();
        for r in 0..R {
            for c in 0..C {
                *out.get_mut(c, r) = self.data[r][c];
            }
        }
        out
    }
}

// ── addition ──────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::ops::Add for Mat<R, C> {
    type Output = Mat<R, C>;

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

// ── subtraction ───────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::ops::Sub for Mat<R, C> {
    type Output = Mat<R, C>;

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

// ── matrix multiplication ─────────────────────────────────────────────────────
//
// Mat<R, K> * Mat<K, C> → Mat<R, C>
// The inner dimension `K` is enforced at compile time by the const-generic
// signature; no runtime assert is needed.

impl<const R: usize, const K: usize, const C: usize> std::ops::Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zero();
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

// ── LU decomposition with partial pivoting ────────────────────────────────────

/// Result of an LU decomposition with partial (row) pivoting.
///
/// The decomposition satisfies `P * A = L * U` where:
/// * `L` is unit lower-triangular (diagonal is all 1),
/// * `U` is upper-triangular,
/// * `perm[i]` is the row of the original matrix that ended up in row `i`.
///
/// `determinant` is the determinant of the original matrix (`0.0` when
/// the matrix is singular).
pub struct LuDecomp<const N: usize> {
    /// Combined L\U storage (L below diagonal, U on and above).
    lu: Mat<N, N>,
    /// Row permutation produced by partial pivoting.
    pub perm: [usize; N],
    /// Number of row swaps (parity of the permutation).
    swap_count: usize,
    /// Whether the matrix was detected as singular.
    singular: bool,
}

impl<const N: usize> LuDecomp<N> {
    /// Determinant of the original matrix.
    pub fn determinant(&self) -> f64 {
        if self.singular {
            return 0.0;
        }
        let mut det = if self.swap_count % 2 == 0 { 1.0 } else { -1.0 };
        for i in 0..N {
            det *= self.lu.data[i][i];
        }
        det
    }

    /// Solve `A * x = b` for `x`.
    ///
    /// Returns `None` when the matrix is singular.
    pub fn solve(&self, b: &[f64; N]) -> Option<[f64; N]> {
        if self.singular {
            return None;
        }

        // Apply the row permutation to b.
        let mut x = [0.0_f64; N];
        for i in 0..N {
            x[i] = b[self.perm[i]];
        }

        // Forward substitution: solve L * y = Pb.
        for i in 0..N {
            for j in 0..i {
                x[i] -= self.lu.data[i][j] * x[j];
            }
            // L has implicit 1 on the diagonal — no division needed.
        }

        // Back substitution: solve U * x = y.
        for i in (0..N).rev() {
            for j in (i + 1)..N {
                x[i] -= self.lu.data[i][j] * x[j];
            }
            x[i] /= self.lu.data[i][i];
        }

        Some(x)
    }
}

/// Threshold below which a pivot is considered zero (singular matrix).
const PIVOT_EPS: f64 = 1e-12;

impl<const N: usize> Mat<N, N> {
    /// Compute the LU decomposition with partial pivoting.
    ///
    /// Returns an [`LuDecomp`] from which the determinant can be read and
    /// linear systems `A * x = b` can be solved.
    pub fn lu(&self) -> LuDecomp<N> {
        let mut lu = self.clone();
        let mut perm: [usize; N] = std::array::from_fn(|i| i);
        let mut swap_count = 0_usize;
        let mut singular = false;

        for col in 0..N {
            // Find the row with the largest absolute value in this column
            // at or below the current diagonal (partial pivoting).
            let mut pivot_row = col;
            let mut pivot_val = lu.data[col][col].abs();
            for row in (col + 1)..N {
                let v = lu.data[row][col].abs();
                if v > pivot_val {
                    pivot_val = v;
                    pivot_row = row;
                }
            }

            if pivot_val < PIVOT_EPS {
                singular = true;
                // Continue so that the LU arrays are still filled in a
                // consistent way, but mark the result as singular.
            }

            // Swap rows in lu and perm.
            if pivot_row != col {
                lu.data.swap(col, pivot_row);
                perm.swap(col, pivot_row);
                swap_count += 1;
            }

            // Eliminate entries below the pivot.
            if !singular || lu.data[col][col].abs() >= PIVOT_EPS {
                for row in (col + 1)..N {
                    lu.data[row][col] /= lu.data[col][col];
                    let factor = lu.data[row][col];
                    for k in (col + 1)..N {
                        lu.data[row][k] -= factor * lu.data[col][k];
                    }
                }
            }
        }

        LuDecomp { lu, perm, swap_count, singular }
    }

    /// Convenience wrapper: solve `self * x = b`.
    ///
    /// Returns `None` when the matrix is singular.
    pub fn solve(&self, b: &[f64; N]) -> Option<[f64; N]> {
        self.lu().solve(b)
    }
}

// ── display ───────────────────────────────────────────────────────────────────

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

// ── tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    fn vec_approx_eq(a: &[f64], b: &[f64]) -> bool {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| approx_eq(*x, *y))
    }

    fn mat_approx_eq<const R: usize, const C: usize>(a: &Mat<R, C>, b: &Mat<R, C>) -> bool {
        for r in 0..R {
            for c in 0..C {
                if !approx_eq(a.get(r, c), b.get(r, c)) {
                    return false;
                }
            }
        }
        true
    }

    // ── construction & basic ops ──────────────────────────────────────────────

    #[test]
    fn zero_matrix() {
        let m = Mat::<3, 3>::zero();
        for r in 0..3 {
            for c in 0..3 {
                assert_eq!(m.get(r, c), 0.0);
            }
        }
    }

    #[test]
    fn identity_matrix() {
        let id = Mat::<3, 3>::identity();
        for r in 0..3 {
            for c in 0..3 {
                let expected = if r == c { 1.0 } else { 0.0 };
                assert_eq!(id.get(r, c), expected);
            }
        }
    }

    // ── add / sub ─────────────────────────────────────────────────────────────

    #[test]
    fn add_matrices() {
        let a = Mat::<2, 2>::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::<2, 2>::new([[5.0, 6.0], [7.0, 8.0]]);
        let c = a + b;
        assert!(mat_approx_eq(&c, &Mat::new([[6.0, 8.0], [10.0, 12.0]])));
    }

    #[test]
    fn sub_matrices() {
        let a = Mat::<2, 2>::new([[5.0, 6.0], [7.0, 8.0]]);
        let b = Mat::<2, 2>::new([[1.0, 2.0], [3.0, 4.0]]);
        let c = a - b;
        assert!(mat_approx_eq(&c, &Mat::new([[4.0, 4.0], [4.0, 4.0]])));
    }

    // ── multiplication ────────────────────────────────────────────────────────

    #[test]
    fn mul_square() {
        let a = Mat::<2, 2>::new([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::<2, 2>::new([[2.0, 0.0], [1.0, 2.0]]);
        let c = a * b;
        // [[1*2+2*1, 1*0+2*2], [3*2+4*1, 3*0+4*2]] = [[4,4],[10,8]]
        assert!(mat_approx_eq(&c, &Mat::new([[4.0, 4.0], [10.0, 8.0]])));
    }

    #[test]
    fn mul_non_square() {
        // (2×3) * (3×2) → (2×2)
        let a = Mat::<2, 3>::new([[1.0, 0.0, 2.0], [0.0, 3.0, 1.0]]);
        let b = Mat::<3, 2>::new([[1.0, 2.0], [0.0, 1.0], [4.0, 0.0]]);
        let c = a * b;
        // row 0: [1+0+8, 2+0+0] = [9, 2]
        // row 1: [0+0+4, 0+3+0] = [4, 3]
        assert!(mat_approx_eq(&c, &Mat::new([[9.0, 2.0], [4.0, 3.0]])));
    }

    #[test]
    fn identity_mul() {
        let a = Mat::<3, 3>::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        let id = Mat::<3, 3>::identity();
        let result = a.clone() * id;
        assert!(mat_approx_eq(&result, &a));
    }

    // ── transpose ─────────────────────────────────────────────────────────────

    #[test]
    fn transpose_square() {
        let a = Mat::<2, 2>::new([[1.0, 2.0], [3.0, 4.0]]);
        let t = a.transpose();
        assert!(mat_approx_eq(&t, &Mat::new([[1.0, 3.0], [2.0, 4.0]])));
    }

    #[test]
    fn transpose_rectangular() {
        let a = Mat::<2, 3>::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t = a.transpose(); // → Mat<3, 2>
        assert_eq!(t.get(0, 0), 1.0);
        assert_eq!(t.get(0, 1), 4.0);
        assert_eq!(t.get(1, 0), 2.0);
        assert_eq!(t.get(1, 1), 5.0);
        assert_eq!(t.get(2, 0), 3.0);
        assert_eq!(t.get(2, 1), 6.0);
    }

    #[test]
    fn double_transpose_identity() {
        let a = Mat::<3, 2>::new([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]);
        let tt = a.clone().transpose().transpose();
        assert!(mat_approx_eq(&tt, &a));
    }

    // ── LU / determinant ──────────────────────────────────────────────────────

    #[test]
    fn determinant_1x1() {
        let m = Mat::<1, 1>::new([[7.0]]);
        assert!(approx_eq(m.lu().determinant(), 7.0));
    }

    #[test]
    fn determinant_2x2() {
        // det([[3,8],[4,6]]) = 18 - 32 = -14
        let m = Mat::<2, 2>::new([[3.0, 8.0], [4.0, 6.0]]);
        assert!(approx_eq(m.lu().determinant(), -14.0));
    }

    #[test]
    fn determinant_3x3() {
        // Known det = 1*5*9 + 2*6*7 + 3*4*8 − 3*5*7 − 2*4*9 − 1*6*8
        //           = 45+84+96 − 105−72−48 = 225 − 225 = 0  ← NOT this matrix
        // Use a matrix with det = 1 for simplicity:
        // [[2,1,0],[0,3,1],[1,0,2]] → det = 2(6-0) - 1(0-1) + 0 = 12+1 = 13
        let m = Mat::<3, 3>::new([
            [2.0, 1.0, 0.0],
            [0.0, 3.0, 1.0],
            [1.0, 0.0, 2.0],
        ]);
        assert!(approx_eq(m.lu().determinant(), 13.0));
    }

    #[test]
    fn determinant_non_invertible() {
        // Rows are linearly dependent → det should be 0.
        let m = Mat::<3, 3>::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        // Allow small floating-point error around zero.
        assert!(m.lu().determinant().abs() < 1e-6);
    }

    // ── solver ────────────────────────────────────────────────────────────────

    #[test]
    fn solve_2x2() {
        // 2x + y = 5
        // x + 3y = 10  →  x = 1, y = 3
        let a = Mat::<2, 2>::new([[2.0, 1.0], [1.0, 3.0]]);
        let b = [5.0, 10.0];
        let x = a.solve(&b).expect("system should be solvable");
        assert!(vec_approx_eq(&x, &[1.0, 3.0]));
    }

    #[test]
    fn solve_3x3() {
        // System with known solution x=[1,2,3].
        // 2a + b     = 4   → 2*1 + 2    = 4  ✓
        //  a + 3b + c = 10  → 1 + 6 + 3  = 10 ✓
        // 2a +  c    = 5   → 2 + 3       = 5  ✓
        let a = Mat::<3, 3>::new([
            [2.0, 1.0, 0.0],
            [1.0, 3.0, 1.0],
            [2.0, 0.0, 1.0],
        ]);
        let b = [4.0, 10.0, 5.0];
        let x = a.solve(&b).expect("system should be solvable");
        assert!(vec_approx_eq(&x, &[1.0, 2.0, 3.0]));
    }

    #[test]
    fn solve_singular_returns_none() {
        // Singular matrix: rows are multiples of each other.
        let a = Mat::<2, 2>::new([[1.0, 2.0], [2.0, 4.0]]);
        assert!(a.solve(&[1.0, 2.0]).is_none());
    }

    #[test]
    fn solve_non_invertible_3x3_returns_none() {
        let a = Mat::<3, 3>::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        assert!(a.solve(&[1.0, 2.0, 3.0]).is_none());
    }

    #[test]
    fn lu_round_trip() {
        // Verify P*A = L*U by reconstructing L and U from the packed storage.
        let a = Mat::<3, 3>::new([
            [3.0, 1.0, 2.0],
            [6.0, 3.0, 4.0],
            [3.0, 2.0, 5.0],
        ]);
        let lu = a.lu();

        // Extract L (unit lower) and U (upper) from packed storage.
        let mut l = Mat::<3, 3>::zero();
        let mut u = Mat::<3, 3>::zero();
        for r in 0..3 {
            for c in 0..3 {
                if r > c {
                    *l.get_mut(r, c) = lu.lu.get(r, c);
                } else if r == c {
                    l.data[r][c] = 1.0;
                    *u.get_mut(r, c) = lu.lu.get(r, c);
                } else {
                    *u.get_mut(r, c) = lu.lu.get(r, c);
                }
            }
        }

        // Build the permuted version of A.
        let mut pa = Mat::<3, 3>::zero();
        for r in 0..3 {
            pa.data[r] = a.data[lu.perm[r]];
        }

        let lu_product = l * u;
        assert!(mat_approx_eq(&lu_product, &pa));
    }
}
