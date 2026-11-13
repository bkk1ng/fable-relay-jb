/// Fixed-size generic matrix over f64.
///
/// `R` is the number of rows, `C` the number of columns.
/// All storage is row-major on the stack.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    data: [[f64; C]; R],
}

// ── Construction ─────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// All-zero matrix.
    pub fn zero() -> Self {
        Self { data: [[0.0; C]; R] }
    }

    /// Build from a row-major 2-D array.
    pub fn from_array(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Element access (row `r`, column `c`).
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access.
    pub fn get_mut(&mut self, r: usize, c: usize) -> &mut f64 {
        &mut self.data[r][c]
    }

    /// Set element at (row `r`, column `c`).
    pub fn set(&mut self, r: usize, c: usize, v: f64) {
        self.data[r][c] = v;
    }
}

// ── Identity (square matrices only) ──────────────────────────────────────────

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

// ── Transpose ────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Return the transpose as a `Mat<C, R>`.
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

// ── Add ───────────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::ops::Add for Mat<R, C> {
    type Output = Mat<R, C>;

    fn add(self, rhs: Mat<R, C>) -> Mat<R, C> {
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

impl<const R: usize, const C: usize> std::ops::Sub for Mat<R, C> {
    type Output = Mat<R, C>;

    fn sub(self, rhs: Mat<R, C>) -> Mat<R, C> {
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
//
// A: R×K  *  B: K×C  →  R×C
// The shared inner dimension K is encoded in both operand types, so the
// constraint is satisfied entirely at compile time.

impl<const R: usize, const K: usize, const C: usize> std::ops::Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;

    fn mul(self, rhs: Mat<K, C>) -> Mat<R, C> {
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

// ── LU decomposition with partial pivoting ───────────────────────────────────
//
// Operates on square N×N matrices.
// Returns `(L, U, perm, det)` where:
//   • `L` is lower-triangular with unit diagonal,
//   • `U` is upper-triangular,
//   • `perm` is the pivot permutation vector (perm[i] = original row),
//   • `det` is the determinant of the original matrix (0.0 for singular).
//
// The decomposition satisfies  P * A = L * U  where P is the permutation
// matrix encoded by `perm`.

pub struct LuDecomposition<const N: usize> {
    pub l: Mat<N, N>,
    pub u: Mat<N, N>,
    /// perm[i] is the original row index now at row i after pivoting.
    pub perm: [usize; N],
    pub det: f64,
}

impl<const N: usize> Mat<N, N> {
    /// LU decomposition with partial (row) pivoting.
    ///
    /// Returns `None` if the matrix is structurally singular (a zero column is
    /// encountered that cannot be pivoted away).  For a numerically singular
    /// matrix the returned `det` will be 0.0 or very close to it.
    pub fn lu(&self) -> Option<LuDecomposition<N>> {
        // Work on a mutable copy of the data; we build U in-place and
        // accumulate L separately.
        let mut a = self.data;
        let mut perm: [usize; N] = [0; N];
        for i in 0..N {
            perm[i] = i;
        }

        let mut sign = 1_i32; // tracks row-swap parity for determinant sign
        let mut l = [[0.0_f64; N]; N];

        for col in 0..N {
            // Find the row with the largest absolute value in this column
            // (partial pivoting).
            let mut max_val = a[col][col].abs();
            let mut max_row = col;
            for row in (col + 1)..N {
                let v = a[row][col].abs();
                if v > max_val {
                    max_val = v;
                    max_row = row;
                }
            }

            if max_val == 0.0 {
                // Truly singular — no pivot available.
                return None;
            }

            // Swap rows in `a`, `l`, and `perm`.
            if max_row != col {
                a.swap(col, max_row);
                l.swap(col, max_row);
                perm.swap(col, max_row);
                sign = -sign;
            }

            // Eliminate entries below the pivot.
            for row in (col + 1)..N {
                let factor = a[row][col] / a[col][col];
                l[row][col] = factor;
                for k in col..N {
                    a[row][k] -= factor * a[col][k];
                }
            }
        }

        // Build L and U as Mat values.
        let mut mat_l = Mat::<N, N>::zero();
        let mut mat_u = Mat::<N, N>::zero();

        for i in 0..N {
            mat_l.data[i][i] = 1.0; // unit diagonal
            for j in 0..i {
                mat_l.data[i][j] = l[i][j];
            }
            for j in i..N {
                mat_u.data[i][j] = a[i][j];
            }
        }

        // Determinant = sign * product of U diagonal.
        let mut det = sign as f64;
        for i in 0..N {
            det *= mat_u.data[i][i];
        }

        Some(LuDecomposition { l: mat_l, u: mat_u, perm, det })
    }

    /// Solve the linear system `A * x = b` using LU with partial pivoting.
    ///
    /// Returns `None` if `A` is singular.
    /// `b` is a column vector represented as `[f64; N]`.
    pub fn solve(&self, b: &[f64; N]) -> Option<[f64; N]> {
        let lu = self.lu()?;

        // Apply row permutation to b.
        let mut pb = [0.0_f64; N];
        for i in 0..N {
            pb[i] = b[lu.perm[i]];
        }

        // Forward substitution: solve L * y = Pb.
        let mut y = [0.0_f64; N];
        for i in 0..N {
            let mut s = pb[i];
            for j in 0..i {
                s -= lu.l.data[i][j] * y[j];
            }
            // L has unit diagonal, so no division needed.
            y[i] = s;
        }

        // Back substitution: solve U * x = y.
        let mut x = [0.0_f64; N];
        for i in (0..N).rev() {
            let mut s = y[i];
            for j in (i + 1)..N {
                s -= lu.u.data[i][j] * x[j];
            }
            let pivot = lu.u.data[i][i];
            if pivot == 0.0 {
                return None;
            }
            x[i] = s / pivot;
        }

        Some(x)
    }
}

// ── Display ───────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> std::fmt::Display for Mat<R, C> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
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

    const EPS: f64 = 1e-10;

    fn approx_eq(a: f64, b: f64) -> bool {
        (a - b).abs() < EPS
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
    fn add_sub_roundtrip() {
        let a = Mat::<2, 2>::from_array([[1.0, 2.0], [3.0, 4.0]]);
        let b = Mat::<2, 2>::from_array([[5.0, 6.0], [7.0, 8.0]]);
        let sum = a.clone() + b.clone();
        let diff = sum - b.clone();
        assert!(mat_approx_eq(&diff, &a));
    }

    // ── mul (dimension-checked at compile time) ───────────────────────────────

    #[test]
    fn mul_2x3_3x2() {
        // [ 1 2 3 ]   [ 7  8  ]   [ 58  64  ]
        // [ 4 5 6 ] * [ 9  10 ] = [ 139 154 ]
        //             [ 11 12 ]
        let a = Mat::<2, 3>::from_array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let b = Mat::<3, 2>::from_array([
            [7.0, 8.0],
            [9.0, 10.0],
            [11.0, 12.0],
        ]);
        let c = a * b;
        let expected =
            Mat::<2, 2>::from_array([[58.0, 64.0], [139.0, 154.0]]);
        assert!(mat_approx_eq(&c, &expected));
    }

    #[test]
    fn mul_identity() {
        let a = Mat::<3, 3>::from_array([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        let i = Mat::<3, 3>::identity();
        let ai = a.clone() * i.clone();
        let ia = i * a.clone();
        assert!(mat_approx_eq(&ai, &a));
        assert!(mat_approx_eq(&ia, &a));
    }

    // ── transpose ─────────────────────────────────────────────────────────────

    #[test]
    fn transpose_2x3() {
        let a = Mat::<2, 3>::from_array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t = a.transpose();
        assert_eq!(t.get(0, 0), 1.0);
        assert_eq!(t.get(1, 0), 2.0);
        assert_eq!(t.get(2, 0), 3.0);
        assert_eq!(t.get(0, 1), 4.0);
        assert_eq!(t.get(1, 1), 5.0);
        assert_eq!(t.get(2, 1), 6.0);
    }

    #[test]
    fn transpose_involution() {
        let a = Mat::<3, 2>::from_array([
            [1.0, 2.0],
            [3.0, 4.0],
            [5.0, 6.0],
        ]);
        // (A^T)^T == A
        assert!(mat_approx_eq(&a.transpose().transpose(), &a));
    }

    // ── identity ──────────────────────────────────────────────────────────────

    #[test]
    fn identity_diagonal() {
        let i = Mat::<4, 4>::identity();
        for r in 0..4 {
            for c in 0..4 {
                let expected = if r == c { 1.0 } else { 0.0 };
                assert_eq!(i.get(r, c), expected);
            }
        }
    }

    // ── LU decomposition ─────────────────────────────────────────────────────

    #[test]
    fn lu_det_3x3() {
        // Known determinant: det = 1*(5*9 - 6*8) - 2*(4*9 - 6*7) + 3*(4*8 - 5*7)
        //                       = 1*(-3) - 2*(-6) + 3*(-3) = -3 + 12 - 9 = 0
        // Use a matrix with det != 0 instead.
        let a = Mat::<3, 3>::from_array([
            [2.0, 1.0, 1.0],
            [4.0, 3.0, 3.0],
            [8.0, 7.0, 9.0],
        ]);
        let lu = a.lu().expect("should be invertible");
        // det(A) can be computed as product of U diagonal * swap parity.
        // Verify L*U == P*A by reconstructing P*A.
        let lu_prod = lu.l.clone() * lu.u.clone();
        let mut pa = Mat::<3, 3>::zero();
        for i in 0..3 {
            for j in 0..3 {
                pa.set(i, j, a.get(lu.perm[i], j));
            }
        }
        assert!(mat_approx_eq(&lu_prod, &pa));
        // det should be non-zero.
        assert!(!approx_eq(lu.det, 0.0));
    }

    #[test]
    fn lu_reconstruct_lu_eq_pa() {
        let a = Mat::<4, 4>::from_array([
            [ 6.0,  1.0,  1.0,  4.0],
            [ 4.0,  2.0,  5.0,  2.0],
            [ 2.0,  8.0,  7.0,  1.0],
            [ 1.0,  3.0, 11.0,  9.0],
        ]);
        let lu = a.lu().expect("should be invertible");
        let lu_prod = lu.l.clone() * lu.u.clone();
        let mut pa = Mat::<4, 4>::zero();
        for i in 0..4 {
            for j in 0..4 {
                pa.set(i, j, a.get(lu.perm[i], j));
            }
        }
        assert!(mat_approx_eq(&lu_prod, &pa));
    }

    // ── solver ────────────────────────────────────────────────────────────────

    #[test]
    fn solve_3x3() {
        // A * x = b,  x = [1, 2, 3] by construction.
        let a = Mat::<3, 3>::from_array([
            [2.0, 1.0, -1.0],
            [-3.0, -1.0, 2.0],
            [-2.0, 1.0, 2.0],
        ]);
        let b = [1.0_f64 * 2.0 + 2.0 * 1.0 + 3.0 * -1.0,
                 1.0 * -3.0 + 2.0 * -1.0 + 3.0 * 2.0,
                 1.0 * -2.0 + 2.0 * 1.0 + 3.0 * 2.0];
        let x = a.solve(&b).expect("should have a solution");
        assert!(approx_eq(x[0], 1.0));
        assert!(approx_eq(x[1], 2.0));
        assert!(approx_eq(x[2], 3.0));
    }

    // ── non-invertible matrix ─────────────────────────────────────────────────

    /// A matrix whose rows are linearly dependent (row 2 = row 0 + row 1).
    /// LU should return None and solve should return None.
    #[test]
    fn non_invertible_returns_none() {
        let a = Mat::<3, 3>::from_array([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [5.0, 7.0, 9.0], // = row0 + row1
        ]);
        let b = [1.0, 2.0, 3.0];

        // LU may succeed structurally (pivot search) but det should be ~0,
        // or it returns None directly on a zero-column encounter.
        // Either outcome is acceptable; solve must return None.
        let result = a.solve(&b);
        assert!(
            result.is_none(),
            "expected None for a singular system, got {result:?}"
        );
    }

    /// All-zero matrix is definitely singular.
    #[test]
    fn zero_matrix_is_singular() {
        let a = Mat::<2, 2>::zero();
        assert!(a.lu().is_none());
    }
}
