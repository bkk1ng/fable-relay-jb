"""
linreg.py — Multivariate linear regression by gradient descent.

Python 3.12, stdlib only (no third-party packages).

Public surface
--------------
  StandardScaler          z-score standardisation fitted on training data
  LinearRegression        gradient-descent multivariate linear model
  train_test_split        random holdout split
  rmse / r2_score         evaluation metrics
  load_csv                CSV → (X, y, feature_names)
  main()                  demo on a generated dataset

Usage
-----
  python linreg.py                 # run the __main__ demo
"""

from __future__ import annotations

import csv
import math
import random
import statistics
import warnings
from dataclasses import dataclass, field
from pathlib import Path


# ---------------------------------------------------------------------------
# Type aliases
# ---------------------------------------------------------------------------

Matrix = list[list[float]]  # shape (n_samples, n_features)
Vector = list[float]        # length n_samples or n_features


# ---------------------------------------------------------------------------
# Minimal pure-Python linear-algebra helpers
# ---------------------------------------------------------------------------

def _dot(a: Vector, b: Vector) -> float:
    """Dot product of two equal-length vectors."""
    return sum(x * y for x, y in zip(a, b))


def _matvec(X: Matrix, w: Vector) -> Vector:
    """Matrix–vector product: X @ w.  X is (n, p), w is (p,)."""
    return [_dot(row, w) for row in X]


def _transpose(X: Matrix) -> Matrix:
    """Transpose of a matrix stored as a list of rows."""
    if not X:
        return []
    return [[row[i] for row in X] for i in range(len(X[0]))]


def _vec_sub(a: Vector, b: Vector) -> Vector:
    return [x - y for x, y in zip(a, b)]


def _vec_scale(v: Vector, s: float) -> Vector:
    return [x * s for x in v]


# ---------------------------------------------------------------------------
# Feature standardisation
# ---------------------------------------------------------------------------

@dataclass
class StandardScaler:
    """
    Z-score scaler: transforms each feature to zero mean, unit variance.

    Fit once on training data, then call transform() on any split to avoid
    data leakage.

    Attributes set after fit()
    --------------------------
    means_ : per-feature means
    stds_  : per-feature standard deviations (constant columns become 1.0)
    """

    means_: Vector = field(default_factory=list, init=False, repr=False)
    stds_: Vector = field(default_factory=list, init=False, repr=False)
    _fitted: bool = field(default=False, init=False, repr=False)

    def fit(self, X: Matrix) -> "StandardScaler":
        """Compute per-column mean and std from X."""
        if not X:
            raise ValueError("Cannot fit on an empty matrix.")
        cols = _transpose(X)
        self.means_ = [statistics.mean(col) for col in cols]
        self.stds_ = []
        for col in cols:
            # pstdev so a single-row column doesn't raise StatisticsError
            s = statistics.pstdev(col)
            self.stds_.append(s if s > 0.0 else 1.0)
        self._fitted = True
        return self

    def transform(self, X: Matrix) -> Matrix:
        """Apply stored mean/std to X."""
        if not self._fitted:
            raise RuntimeError("Call fit() before transform().")
        return [
            [(x - self.means_[j]) / self.stds_[j] for j, x in enumerate(row)]
            for row in X
        ]

    def fit_transform(self, X: Matrix) -> Matrix:
        """Fit on X and immediately return the transformed result."""
        return self.fit(X).transform(X)


# ---------------------------------------------------------------------------
# Learning-rate validation
# ---------------------------------------------------------------------------

def _validate_learning_rate(lr: float) -> None:
    """
    Raise TypeError / ValueError for obviously wrong learning rates,
    warn for unusually large ones.
    """
    if not isinstance(lr, (int, float)):
        raise TypeError(
            f"learning_rate must be a float, got {type(lr).__name__}."
        )
    if math.isnan(lr):
        raise ValueError("learning_rate must not be NaN.")
    if math.isinf(lr):
        raise ValueError("learning_rate must be finite.")
    if lr <= 0.0:
        raise ValueError(f"learning_rate must be positive, got {lr!r}.")
    if lr > 10.0:
        warnings.warn(
            f"learning_rate={lr} is unusually large and may cause divergence.",
            UserWarning,
            stacklevel=3,
        )


# ---------------------------------------------------------------------------
# Linear regression model
# ---------------------------------------------------------------------------

@dataclass
class LinearRegression:
    """
    Multivariate linear regression fitted by full-batch gradient descent.

    Parameters
    ----------
    learning_rate : float
        Gradient-descent step size (default 0.01).
    n_iterations : int
        Number of gradient steps (default 1 000).
    fit_intercept : bool
        Whether to learn a bias / intercept term (default True).

    Attributes set after fit()
    --------------------------
    weights_      : learned coefficient vector, shape (n_features,)
    bias_         : learned intercept (0.0 when fit_intercept=False)
    loss_history_ : MSE at every iteration, length == n_iterations
    n_features_in_: number of features seen during fit
    """

    learning_rate: float = 0.01
    n_iterations: int = 1_000
    fit_intercept: bool = True

    weights_: Vector = field(default_factory=list, init=False, repr=False)
    bias_: float = field(default=0.0, init=False)
    loss_history_: Vector = field(default_factory=list, init=False, repr=False)
    n_features_in_: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        _validate_learning_rate(self.learning_rate)
        if not isinstance(self.n_iterations, int) or self.n_iterations < 1:
            raise ValueError("n_iterations must be a positive integer.")

    # ------------------------------------------------------------------
    def fit(self, X: Matrix, y: Vector) -> "LinearRegression":
        """
        Fit the model on design matrix X and target vector y.

        Parameters
        ----------
        X : list of n rows, each a list of p feature values
        y : list of n target values
        """
        n = len(X)
        if n == 0:
            raise ValueError("Training set is empty.")
        p = len(X[0])
        if len(y) != n:
            raise ValueError(
                f"X has {n} rows but y has {len(y)} elements."
            )
        self.n_features_in_ = p

        # Initialise parameters to zero
        self.weights_ = [0.0] * p
        self.bias_ = 0.0
        self.loss_history_ = []

        lr = self.learning_rate
        inv_n = 1.0 / n
        two_over_n = 2.0 / n

        # Pre-transpose X so the inner loop avoids repeated transpositions.
        # For each gradient step we need  X.T @ residuals.
        XT = _transpose(X)  # shape (p, n)

        for _ in range(self.n_iterations):
            # Forward pass: ŷ = X @ w  [+ b]
            y_hat = _matvec(X, self.weights_)
            if self.fit_intercept:
                b = self.bias_
                y_hat = [v + b for v in y_hat]

            # Residuals: r = ŷ − y
            residuals = _vec_sub(y_hat, y)

            # MSE loss = (1/n) Σ rᵢ²
            self.loss_history_.append(inv_n * sum(r * r for r in residuals))

            # Gradients
            # ∂L/∂w = (2/n) · Xᵀ · r
            grad_w = [two_over_n * _dot(col, residuals) for col in XT]
            # ∂L/∂b = (2/n) · Σ rᵢ
            grad_b = two_over_n * sum(residuals) if self.fit_intercept else 0.0

            # Gradient step
            self.weights_ = _vec_sub(self.weights_, _vec_scale(grad_w, lr))
            self.bias_ -= lr * grad_b

        return self

    # ------------------------------------------------------------------
    def predict(self, X: Matrix) -> Vector:
        """
        Return predictions for design matrix X.

        X must be standardised with the same scaler used during fit.
        """
        if not self.weights_:
            raise RuntimeError("Model has not been fitted yet. Call fit() first.")
        y_hat = _matvec(X, self.weights_)
        if self.fit_intercept:
            b = self.bias_
            y_hat = [v + b for v in y_hat]
        return y_hat


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def rmse(y_true: Vector, y_pred: Vector) -> float:
    """Root mean squared error."""
    n = len(y_true)
    if n == 0:
        raise ValueError("Empty vectors passed to rmse().")
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(y_true, y_pred)) / n)


def r2_score(y_true: Vector, y_pred: Vector) -> float:
    """
    Coefficient of determination R².

    Returns 1.0 for a perfect fit, 0.0 for a constant-mean baseline,
    and negative values for predictions worse than the mean.
    """
    n = len(y_true)
    if n == 0:
        raise ValueError("Empty vectors passed to r2_score().")
    mean_y = statistics.mean(y_true)
    ss_tot = sum((y - mean_y) ** 2 for y in y_true)
    ss_res = sum((a - b) ** 2 for a, b in zip(y_true, y_pred))
    if ss_tot == 0.0:
        # Constant target: perfect if predictions also constant, else undefined
        return 1.0 if ss_res == 0.0 else 0.0
    return 1.0 - ss_res / ss_tot


# ---------------------------------------------------------------------------
# Train / test split
# ---------------------------------------------------------------------------

def train_test_split(
    X: Matrix,
    y: Vector,
    test_size: float = 0.2,
    seed: int | None = None,
) -> tuple[Matrix, Matrix, Vector, Vector]:
    """
    Randomly split (X, y) into train and test subsets.

    Parameters
    ----------
    X, y      : dataset
    test_size : fraction of data reserved for testing (default 0.2)
    seed      : random seed for reproducibility

    Returns
    -------
    X_train, X_test, y_train, y_test
    """
    if not 0.0 < test_size < 1.0:
        raise ValueError("test_size must be strictly between 0 and 1.")
    rng = random.Random(seed)
    indices = list(range(len(X)))
    rng.shuffle(indices)
    split = int(len(X) * (1.0 - test_size))
    train_idx, test_idx = indices[:split], indices[split:]
    return (
        [X[i] for i in train_idx],
        [X[i] for i in test_idx],
        [y[i] for i in train_idx],
        [y[i] for i in test_idx],
    )


# ---------------------------------------------------------------------------
# CSV loader
# ---------------------------------------------------------------------------

def load_csv(
    path: str | Path,
    target_col: int = -1,
    has_header: bool = True,
    delimiter: str = ",",
) -> tuple[Matrix, Vector, list[str]]:
    """
    Load a rectangular CSV file into a design matrix and target vector.

    Parameters
    ----------
    path       : path to the CSV file
    target_col : column index of the target variable (default -1 = last column)
    has_header : whether the first row contains column names (default True)
    delimiter  : field separator (default ',')

    Returns
    -------
    X             : design matrix — all columns except target_col
    y             : target vector
    feature_names : list of feature column names (synthetic if no header)

    Raises
    ------
    ValueError : file empty, no data rows, or non-numeric value encountered
    """
    with open(path, newline="", encoding="utf-8") as fh:
        rows: list[list[str]] = list(csv.reader(fh, delimiter=delimiter))

    if not rows:
        raise ValueError(f"CSV file is empty: {path}")

    header: list[str] = []
    if has_header:
        header = rows[0]
        rows = rows[1:]

    if not rows:
        raise ValueError("CSV file contains a header but no data rows.")

    n_cols = len(rows[0])
    t = target_col if target_col >= 0 else n_cols + target_col
    if not 0 <= t < n_cols:
        raise ValueError(
            f"target_col={target_col!r} is out of range for {n_cols} columns."
        )
    feature_indices = [i for i in range(n_cols) if i != t]
    feature_names = (
        [header[i] for i in feature_indices]
        if header
        else [f"x{i}" for i in feature_indices]
    )

    X: Matrix = []
    y: Vector = []
    offset = 2 if has_header else 1
    for row_num, row in enumerate(rows, start=offset):
        if len(row) != n_cols:
            raise ValueError(
                f"Row {row_num} has {len(row)} fields; expected {n_cols}."
            )
        try:
            vals = [float(v.strip()) for v in row]
        except ValueError as exc:
            raise ValueError(
                f"Non-numeric value at row {row_num}: {exc}"
            ) from exc
        X.append([vals[i] for i in feature_indices])
        y.append(vals[t])

    return X, y, feature_names


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def _generate_dataset(
    n: int = 400,
    seed: int = 42,
) -> tuple[Matrix, Vector]:
    """
    Synthetic multivariate regression dataset.

    True model: y = 3·x₀ − 1.5·x₁ + 2·x₂ + 0.5·x₃ + 5 + ε,  ε ~ N(0, 0.5)
    Features drawn independently from N(0, 1).
    """
    rng = random.Random(seed)
    true_w = [3.0, -1.5, 2.0, 0.5]
    true_b = 5.0
    sigma = 0.5

    X = [[rng.gauss(0.0, 1.0) for _ in true_w] for _ in range(n)]
    y = [
        sum(w * xi for w, xi in zip(true_w, row)) + true_b + rng.gauss(0.0, sigma)
        for row in X
    ]
    return X, y


def _hr(char: str = "─", width: int = 58) -> None:
    print(char * width)


def main() -> None:
    """Run the gradient-descent linear regression demo."""
    print()
    _hr("═")
    print("  Multivariate Linear Regression  ·  gradient-descent demo")
    _hr("═")

    # ── 1. Data ──────────────────────────────────────────────────────────
    X, y = _generate_dataset(n=400, seed=42)
    print(f"\n  Samples  : 400   Features : 4")
    print(f"  True model : y = 3·x₀ − 1.5·x₁ + 2·x₂ + 0.5·x₃ + 5 + ε(σ=0.5)")

    # ── 2. Split ─────────────────────────────────────────────────────────
    X_tr, X_te, y_tr, y_te = train_test_split(X, y, test_size=0.2, seed=0)
    print(f"\n  Train / test split : {len(X_tr)} / {len(X_te)}")

    # ── 3. Standardise ───────────────────────────────────────────────────
    scaler = StandardScaler()
    X_tr_s = scaler.fit_transform(X_tr)
    X_te_s = scaler.transform(X_te)

    print(f"\n  Scaler means : {[round(m, 3) for m in scaler.means_]}")
    print(f"  Scaler stds  : {[round(s, 3) for s in scaler.stds_]}")

    # ── 4. Train ─────────────────────────────────────────────────────────
    model = LinearRegression(learning_rate=0.05, n_iterations=2_000)
    model.fit(X_tr_s, y_tr)

    # ── 5. Evaluate ───────────────────────────────────────────────────────
    y_pred = model.predict(X_te_s)
    test_rmse = rmse(y_te, y_pred)
    test_r2   = r2_score(y_te, y_pred)

    _hr()
    print(f"  {'Learned weights':<18} : {[round(w, 4) for w in model.weights_]}")
    print(f"  {'Learned bias':<18} : {model.bias_:.4f}")
    _hr()
    print(f"  {'Test RMSE':<18} : {test_rmse:.4f}  (expect ≈ 0.5)")
    print(f"  {'Test R²':<18} : {test_r2:.4f}  (expect ≈ 1.0)")

    # ── 6. Loss curve summary ─────────────────────────────────────────────
    h = model.loss_history_
    _hr()
    print(f"  Loss (MSE) over {len(h)} iterations")
    checkpoints = [0, 99, 499, 999, len(h) - 1]
    for i in checkpoints:
        label = f"iter {i + 1:>5}"
        print(f"    {label} :  {h[i]:.6f}")
    _hr("═")
    print()


if __name__ == "__main__":
    main()
