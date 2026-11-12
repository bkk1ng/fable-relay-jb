use std::fmt;
use std::ops::{Add, Mul, Sub};

/// Fixed-size matrix of f64 with R rows and C columns.
#[derive(Debug, Clone, PartialEq)]
pub struct Mat<const R: usize, const C: usize> {
    data: [[f64; C]; R],
}

// ─── Construction ────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Create a matrix from a 2-D array.
    pub fn new(data: [[f64; C]; R]) -> Self {
        Self { data }
    }

    /// Create a zero matrix.
    pub fn zeros() -> Self {
        Self { data: [[0.0; C]; R] }
    }

    /// Element access (row, col).
    #[inline]
    pub fn get(&self, r: usize, c: usize) -> f64 {
        self.data[r][c]
    }

    /// Mutable element access.
    #[inline]
    pub fn set(&mut self, r: usize, c: usize, v: f64) {
        self.data[r][c] = v;
    }
}

impl<const N: usize> Mat<N, N> {
    /// Identity matrix (square matrices only).
    pub fn identity() -> Self {
        let mut m = Self::zeros();
        for i in 0..N {
            m.data[i][i] = 1.0;
        }
        m
    }
}

// ─── Transpose ───────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Mat<R, C> {
    /// Return the transpose as a Mat<C, R>.
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

// ─── Add ─────────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Add for Mat<R, C> {
    type Output = Mat<R, C>;
    fn add(self, rhs: Self) -> Self::Output {
        let mut out = Mat::<R, C>::zeros();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] + rhs.data[r][c];
            }
        }
        out
    }
}

// ─── Sub ─────────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> Sub for Mat<R, C> {
    type Output = Mat<R, C>;
    fn sub(self, rhs: Self) -> Self::Output {
        let mut out = Mat::<R, C>::zeros();
        for r in 0..R {
            for c in 0..C {
                out.data[r][c] = self.data[r][c] - rhs.data[r][c];
            }
        }
        out
    }
}

// ─── Mul ─────────────────────────────────────────────────────────────────────

/// Matrix multiplication: (R×K) × (K×C) → (R×C).
/// The inner dimension K is enforced at compile time by the const generics.
impl<const R: usize, const K: usize, const C: usize> Mul<Mat<K, C>> for Mat<R, K> {
    type Output = Mat<R, C>;
    fn mul(self, rhs: Mat<K, C>) -> Self::Output {
        let mut out = Mat::<R, C>::zeros();
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

// ─── LU decomposition with partial pivoting ──────────────────────────────────

/// Result of LU decomposition with partial (row) pivoting.
///
/// The factorisation satisfies P·A = L·U where P is encoded as `piv`.
/// `singular` is set when a zero pivot is encountered; in that case
/// `determinant` is 0 and `solve` will return an error.
#[derive(Debug)]
pub struct LU<const N: usize> {
    /// Combined L\U storage (L has implicit unit diagonal).
    lu: Mat<N, N>,
    /// Row permutation: row i of P·A is row `piv[i]` of A.
    piv: [usize; N],
    /// Number of row swaps, used to track the sign of the determinant.
    parity: i32,
    /// True when the matrix is (numerically) singular.
    pub singular: bool,
    /// det(A).
    pub determinant: f64,
}

impl<const N: usize> LU<N> {
    /// Decompose a square matrix.  Always succeeds; check `singular` before
    /// calling `solve`.
    pub fn new(a: &Mat<N, N>) -> Self {
        let mut lu = a.clone();
        let mut piv: [usize; N] = std::array::from_fn(|i| i);
        let mut parity = 1_i32;

        for k in 0..N {
            // Find the row with the largest absolute value in column k.
            let mut max_val = lu.data[k][k].abs();
            let mut max_row = k;
            for i in (k + 1)..N {
                let v = lu.data[i][k].abs();
                if v > max_val {
                    max_val = v;
                    max_row = i;
                }
            }

            if max_row != k {
                lu.data.swap(k, max_row);
                piv.swap(k, max_row);
                parity = -parity;
            }

            let pivot = lu.data[k][k];
            if pivot.abs() < f64::EPSILON {
                // Singular matrix; fill what we can and bail out.
                return LU {
                    lu,
                    piv,
                    parity,
                    singular: true,
                    determinant: 0.0,
                };
            }

            for i in (k + 1)..N {
                lu.data[i][k] /= pivot;
                for j in (k + 1)..N {
                    let factor = lu.data[i][k];
                    lu.data[i][j] -= factor * lu.data[k][j];
                }
            }
        }

        // det(A) = parity × ∏ U_ii
        let det = {
            let mut d = parity as f64;
            for i in 0..N {
                d *= lu.data[i][i];
            }
            d
        };

        LU {
            lu,
            piv,
            parity,
            singular: false,
            determinant: det,
        }
    }

    /// Solve A·x = b using the stored LU factorisation.
    ///
    /// Returns `Err` when the matrix is singular.
    pub fn solve(&self, b: &[f64; N]) -> Result<[f64; N], &'static str> {
        if self.singular {
            return Err("matrix is singular; system has no unique solution");
        }

        // Apply the same row permutation to b.
        let mut x: [f64; N] = std::array::from_fn(|i| b[self.piv[i]]);

        // Forward substitution: solve L·y = Pb  (L has unit diagonal).
        for k in 0..N {
            for i in (k + 1)..N {
                x[i] -= self.lu.data[i][k] * x[k];
            }
        }

        // Back substitution: solve U·x = y.
        for k in (0..N).rev() {
            x[k] /= self.lu.data[k][k];
            for i in 0..k {
                x[i] -= self.lu.data[i][k] * x[k];
            }
        }

        Ok(x)
    }
}

// ─── Display ─────────────────────────────────────────────────────────────────

impl<const R: usize, const C: usize> fmt::Display for Mat<R, C> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for row in &self.data {
            write!(f, "[")?;
            for (j, v) in row.iter().enumerate() {
                if j > 0 {
                    write!(f, ", ")?;
                }
                write!(f, "{v:8.4}")?;
            }
            writeln!(f, "]")?;
        }
        Ok(())
    }
}

// ─── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    const EPS: f64 = 1e-10;

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < EPS
    }

    fn slice_close(a: &[f64], b: &[f64]) -> bool {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| close(*x, *y))
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

    // ── Identity ─────────────────────────────────────────────────────────────

    #[test]
    fn identity_3x3() {
        let id = Mat::<3, 3>::identity();
        for i in 0..3 {
            for j in 0..3 {
                let expected = if i == j { 1.0 } else { 0.0 };
                assert!(close(id.get(i, j), expected), "id[{i}][{j}]");
            }
        }
    }

    // ── Add / Sub ─────────────────────────────────────────────────────────────

    #[test]
    fn add_sub_roundtrip() {
        let a = Mat::<2, 3>::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let b = Mat::<2, 3>::new([[9.0, 8.0, 7.0], [6.0, 5.0, 4.0]]);
        let sum = a.clone() + b.clone();
        let diff = sum - b;
        assert!(mat_close(&diff, &a));
    }

    // ── Mul ───────────────────────────────────────────────────────────────────

    #[test]
    fn mul_identity() {
        let a = Mat::<3, 3>::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [7.0, 8.0, 9.0],
        ]);
        let id = Mat::<3, 3>::identity();
        let result = a.clone() * id;
        assert!(mat_close(&result, &a));
    }

    #[test]
    fn mul_rectangular() {
        // (2×3) × (3×2) → (2×2)
        let a = Mat::<2, 3>::new([[1.0, 0.0, 2.0], [0.0, 3.0, -1.0]]);
        let b = Mat::<3, 2>::new([[1.0, 1.0], [0.0, 1.0], [4.0, 0.0]]);
        let c = a * b;
        let expected = Mat::<2, 2>::new([[9.0, 1.0], [-4.0, 3.0]]);
        assert!(mat_close(&c, &expected));
    }

    // ── Transpose ─────────────────────────────────────────────────────────────

    #[test]
    fn transpose_2x3() {
        let a = Mat::<2, 3>::new([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]);
        let t = a.transpose();
        for r in 0..2 {
            for c in 0..3 {
                assert!(close(t.get(c, r), a.get(r, c)));
            }
        }
    }

    #[test]
    fn transpose_involution() {
        // (Aᵀ)ᵀ = A
        let a = Mat::<3, 2>::new([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]);
        let tt = a.transpose().transpose();
        assert!(mat_close(&tt, &a));
    }

    // ── LU: invertible matrix ─────────────────────────────────────────────────

    #[test]
    fn lu_determinant_3x3() {
        let a = Mat::<3, 3>::new([
            [2.0, 1.0, -1.0],
            [-3.0, -1.0, 2.0],
            [-2.0, 1.0, 2.0],
        ]);
        let lu = LU::new(&a);
        assert!(!lu.singular);
        // det computed by hand = 2*((-1*2)-(2*1)) - 1*((-3*2)-(2*-2)) + (-1)*((-3*1)-(-1*-2))
        //                      = 2*(-4) - 1*(-2) + (-1)*(-5) = -8 + 2 + 5 = -1
        assert!(close(lu.determinant, -1.0), "det = {}", lu.determinant);
    }

    #[test]
    fn lu_solve_3x3() {
        // 2x + y - z = 8
        // -3x - y + 2z = -11
        // -2x + y + 2z = -3   →  x=2, y=3, z=-1
        let a = Mat::<3, 3>::new([
            [2.0, 1.0, -1.0],
            [-3.0, -1.0, 2.0],
            [-2.0, 1.0, 2.0],
        ]);
        let b = [8.0_f64, -11.0, -3.0];
        let lu = LU::new(&a);
        let x = lu.solve(&b).expect("should be invertible");
        assert!(slice_close(&x, &[2.0, 3.0, -1.0]), "x = {x:?}");
    }

    #[test]
    fn lu_solve_4x4() {
        // Known-good 4×4 invertible system.
        let a = Mat::<4, 4>::new([
            [4.0, 3.0, 0.0, 1.0],
            [2.0, 1.0, 5.0, 0.0],
            [0.0, 7.0, 2.0, 3.0],
            [1.0, 0.0, 3.0, 4.0],
        ]);
        // b = A · [1, 2, 3, 4]
        let x_expected = [1.0_f64, 2.0, 3.0, 4.0];
        let b: [f64; 4] = {
            let mut v = [0.0; 4];
            for r in 0..4 {
                for c in 0..4 {
                    v[r] += a.get(r, c) * x_expected[c];
                }
            }
            v
        };
        let lu = LU::new(&a);
        assert!(!lu.singular);
        let x = lu.solve(&b).expect("should be invertible");
        assert!(slice_close(&x, &x_expected), "x = {x:?}");
    }

    // ── LU: non-invertible matrix ─────────────────────────────────────────────

    #[test]
    fn lu_singular_matrix() {
        // Rows 2 and 3 are linearly dependent (row 3 = 2 × row 2).
        let a = Mat::<3, 3>::new([
            [1.0, 2.0, 3.0],
            [4.0, 5.0, 6.0],
            [8.0, 10.0, 12.0],
        ]);
        let lu = LU::new(&a);
        assert!(lu.singular, "matrix should be detected as singular");
        assert!(close(lu.determinant, 0.0), "det should be 0, got {}", lu.determinant);
        assert!(lu.solve(&[1.0, 2.0, 3.0]).is_err(), "solve should fail on singular matrix");
    }

    #[test]
    fn lu_zero_matrix_is_singular() {
        let a = Mat::<2, 2>::zeros();
        let lu = LU::new(&a);
        assert!(lu.singular);
        assert!(close(lu.determinant, 0.0));
    }

    // ── LU: A = L·U·Pᵀ round-trip ─────────────────────────────────────────────

    #[test]
    fn lu_reconstruct() {
        let a = Mat::<3, 3>::new([
            [3.0, 1.0, 2.0],
            [6.0, 3.0, 8.0],
            [9.0, 2.0, 11.0],
        ]);
        let lu = LU::new(&a);
        assert!(!lu.singular);

        // Extract L and U from the combined storage.
        let mut l = Mat::<3, 3>::zeros();
        let mut u = Mat::<3, 3>::zeros();
        for i in 0..3 {
            l.data[i][i] = 1.0; // unit diagonal
            for j in 0..3 {
                if j < i {
                    l.data[i][j] = lu.lu.data[i][j];
                } else {
                    u.data[i][j] = lu.lu.data[i][j];
                }
            }
        }

        // P·A: apply the pivot permutation to rows of A.
        let mut pa = Mat::<3, 3>::zeros();
        for i in 0..3 {
            for j in 0..3 {
                pa.data[i][j] = a.data[lu.piv[i]][j];
            }
        }

        let lu_product = l * u;
        assert!(mat_close(&lu_product, &pa), "L·U should equal P·A");
    }
}
