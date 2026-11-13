"""
linreg.py — Multivariate linear regression via gradient descent.

Features
--------
- Feature standardisation (z-score, fit on train, applied to holdout)
- Learning-rate validation (positive float, convergence guard)
- Loss history (MSE per epoch)
- R² and RMSE on a held-out test split
- CSV loading (last column = target by default)
- __main__ demo on a generated synthetic dataset
"""

from __future__ import annotations

import csv
import math
import random
from typing import Optional


# ---------------------------------------------------------------------------
# Small matrix / vector helpers (stdlib only, no numpy)
# ---------------------------------------------------------------------------

def _dot(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def _mat_vec(A: list[list[float]], v: list[float]) -> list[float]:
    return [_dot(row, v) for row in A]


def _vec_add(a: list[float], b: list[float]) -> list[float]:
    return [x + y for x, y in zip(a, b)]


def _vec_sub(a: list[float], b: list[float]) -> list[float]:
    return [x - y for x, y in zip(a, b)]


def _vec_scale(a: list[float], s: float) -> list[float]:
    return [x * s for x in a]


def _transpose(A: list[list[float]]) -> list[list[float]]:
    if not A:
        return []
    rows, cols = len(A), len(A[0])
    return [[A[r][c] for r in range(rows)] for c in range(cols)]


# ---------------------------------------------------------------------------
# Data utilities
# ---------------------------------------------------------------------------

def load_csv(
    path: str,
    target_col: int = -1,
    skip_header: bool = True,
) -> tuple[list[list[float]], list[float]]:
    """Load a CSV file and return (X, y).

    Parameters
    ----------
    path:
        Path to the CSV file.
    target_col:
        Column index of the target variable.  Negative indices work
        Python-style (-1 = last column).
    skip_header:
        Skip the first row when True.

    Returns
    -------
    X : list of feature rows
    y : target vector
    """
    X: list[list[float]] = []
    y: list[float] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        if skip_header:
            next(reader, None)
        for row in reader:
            if not row:
                continue
            values = [float(v) for v in row]
            t = target_col % len(values)
            y.append(values[t])
            X.append([v for i, v in enumerate(values) if i != t])
    return X, y


def train_test_split(
    X: list[list[float]],
    y: list[float],
    test_size: float = 0.2,
    seed: Optional[int] = 42,
) -> tuple[list[list[float]], list[list[float]], list[float], list[float]]:
    """Randomly split data into train and test sets."""
    if not (0.0 < test_size < 1.0):
        raise ValueError(f"test_size must be in (0, 1), got {test_size}")
    rng = random.Random(seed)
    n = len(y)
    indices = list(range(n))
    rng.shuffle(indices)
    split = int(math.ceil(n * test_size))
    test_idx = set(indices[:split])
    X_train, X_test, y_train, y_test = [], [], [], []
    for i, (xi, yi) in enumerate(zip(X, y)):
        if i in test_idx:
            X_test.append(xi)
            y_test.append(yi)
        else:
            X_train.append(xi)
            y_train.append(yi)
    return X_train, X_test, y_train, y_test


# ---------------------------------------------------------------------------
# Standardisation
# ---------------------------------------------------------------------------

class StandardScaler:
    """Per-feature z-score standardisation.  Fit on train, transform both."""

    def __init__(self) -> None:
        self.means: list[float] = []
        self.stds: list[float] = []

    def fit(self, X: list[list[float]]) -> "StandardScaler":
        n = len(X)
        if n == 0:
            raise ValueError("Cannot fit on empty data.")
        p = len(X[0])
        self.means = [0.0] * p
        self.stds = [0.0] * p
        for row in X:
            for j in range(p):
                self.means[j] += row[j]
        self.means = [m / n for m in self.means]
        for row in X:
            for j in range(p):
                self.stds[j] += (row[j] - self.means[j]) ** 2
        self.stds = [math.sqrt(s / n) if s > 0 else 1.0 for s in self.stds]
        return self

    def transform(self, X: list[list[float]]) -> list[list[float]]:
        return [
            [(x - m) / s for x, m, s in zip(row, self.means, self.stds)]
            for row in X
        ]

    def fit_transform(self, X: list[list[float]]) -> list[list[float]]:
        return self.fit(X).transform(X)


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def mse(y_true: list[float], y_pred: list[float]) -> float:
    n = len(y_true)
    return sum((a - b) ** 2 for a, b in zip(y_true, y_pred)) / n


def rmse(y_true: list[float], y_pred: list[float]) -> float:
    return math.sqrt(mse(y_true, y_pred))


def r2_score(y_true: list[float], y_pred: list[float]) -> float:
    mean_y = sum(y_true) / len(y_true)
    ss_tot = sum((y - mean_y) ** 2 for y in y_true)
    ss_res = sum((y - yh) ** 2 for y, yh in zip(y_true, y_pred))
    return 1.0 - ss_res / ss_tot if ss_tot > 0.0 else 0.0


# ---------------------------------------------------------------------------
# Linear regression model
# ---------------------------------------------------------------------------

class LinearRegression:
    """Multivariate linear regression trained by gradient descent.

    Parameters
    ----------
    learning_rate:
        Step size for gradient descent.  Must be a positive float.
    epochs:
        Number of full passes over the training data.
    fit_intercept:
        When True, an intercept (bias) term is learned.
    tol:
        Early-stopping tolerance.  Training stops when the absolute
        change in loss between epochs falls below this value.
    verbose:
        Print loss every *verbose* epochs (0 = silent).
    """

    def __init__(
        self,
        learning_rate: float = 0.01,
        epochs: int = 1000,
        fit_intercept: bool = True,
        tol: float = 1e-7,
        verbose: int = 0,
    ) -> None:
        self._validate_lr(learning_rate)
        if epochs < 1:
            raise ValueError(f"epochs must be ≥ 1, got {epochs}")
        self.learning_rate = learning_rate
        self.epochs = epochs
        self.fit_intercept = fit_intercept
        self.tol = tol
        self.verbose = verbose

        self.weights: list[float] = []
        self.bias: float = 0.0
        self.loss_history: list[float] = []

    # ------------------------------------------------------------------
    # Validation
    # ------------------------------------------------------------------

    @staticmethod
    def _validate_lr(lr: float) -> None:
        try:
            lr = float(lr)
        except (TypeError, ValueError):
            raise TypeError(f"learning_rate must be a float, got {type(lr).__name__}")
        if not math.isfinite(lr) or lr <= 0.0:
            raise ValueError(
                f"learning_rate must be a finite positive float, got {lr}"
            )

    # ------------------------------------------------------------------
    # Core
    # ------------------------------------------------------------------

    def _predict_raw(self, X: list[list[float]]) -> list[float]:
        preds = _mat_vec(X, self.weights)
        if self.fit_intercept:
            preds = [p + self.bias for p in preds]
        return preds

    def fit(
        self,
        X: list[list[float]],
        y: list[float],
    ) -> "LinearRegression":
        """Fit model weights by gradient descent.

        Parameters
        ----------
        X: 2-D list of shape (n_samples, n_features)
        y: 1-D list of target values, length n_samples
        """
        n, p = len(X), len(X[0])
        self.weights = [0.0] * p
        self.bias = 0.0
        self.loss_history = []

        lr = self.learning_rate
        Xt = _transpose(X)  # shape (p, n) — reused every epoch

        prev_loss = float("inf")
        for epoch in range(1, self.epochs + 1):
            y_pred = self._predict_raw(X)
            residuals = _vec_sub(y_pred, y)          # (y_hat - y), length n

            # Gradients
            # dW[j] = (1/n) * Σ residual_i * X_i[j]
            grad_w = [_dot(Xt[j], residuals) / n for j in range(p)]
            grad_b = sum(residuals) / n if self.fit_intercept else 0.0

            # Update
            self.weights = _vec_sub(self.weights, _vec_scale(grad_w, lr))
            if self.fit_intercept:
                self.bias -= lr * grad_b

            # Loss (MSE)
            loss = sum(r ** 2 for r in residuals) / n
            self.loss_history.append(loss)

            if self.verbose and epoch % self.verbose == 0:
                print(f"  epoch {epoch:>6d}  MSE={loss:.6f}")

            # Early stopping
            if abs(prev_loss - loss) < self.tol:
                if self.verbose:
                    print(f"  Converged at epoch {epoch} (Δloss < {self.tol})")
                break
            prev_loss = loss

        return self

    def predict(self, X: list[list[float]]) -> list[float]:
        """Return predicted values for X."""
        return self._predict_raw(X)

    def score(
        self, X: list[list[float]], y: list[float]
    ) -> dict[str, float]:
        """Return a dict with R² and RMSE for the given data."""
        y_pred = self.predict(X)
        return {
            "r2": r2_score(y, y_pred),
            "rmse": rmse(y, y_pred),
        }


# ---------------------------------------------------------------------------
# Demo / __main__
# ---------------------------------------------------------------------------

def _generate_dataset(
    n: int = 500,
    n_features: int = 4,
    noise: float = 5.0,
    seed: int = 0,
) -> tuple[list[list[float]], list[float]]:
    """Synthetic dataset: y = Xw + bias + noise, known true weights."""
    rng = random.Random(seed)
    true_w = [rng.uniform(-3.0, 3.0) for _ in range(n_features)]
    true_b = rng.uniform(-5.0, 5.0)
    X, y = [], []
    for _ in range(n):
        row = [rng.gauss(0.0, 1.0) for _ in range(n_features)]
        yi = _dot(row, true_w) + true_b + rng.gauss(0.0, noise)
        X.append(row)
        y.append(yi)
    return X, y


def main() -> None:
    print("=" * 60)
    print("Multivariate Linear Regression — gradient descent demo")
    print("=" * 60)

    # 1. Generate data
    n_features = 4
    X, y = _generate_dataset(n=500, n_features=n_features, noise=5.0, seed=0)
    print(f"\nDataset: {len(X)} samples, {n_features} features")

    # 2. Split
    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, seed=42)
    print(f"Train: {len(X_train)}  Test: {len(X_test)}")

    # 3. Standardise (fit on train only)
    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X_train)
    X_test_s = scaler.transform(X_test)

    # 4. Train
    model = LinearRegression(
        learning_rate=0.05,
        epochs=2000,
        fit_intercept=True,
        tol=1e-8,
        verbose=200,
    )
    print("\nTraining …")
    model.fit(X_train_s, y_train)
    print(f"Finished after {len(model.loss_history)} epochs")
    print(f"Final train MSE : {model.loss_history[-1]:.4f}")

    # 5. Evaluate on holdout
    metrics = model.score(X_test_s, y_test)
    print(f"\n--- Holdout evaluation ---")
    print(f"  R²   : {metrics['r2']:.4f}")
    print(f"  RMSE : {metrics['rmse']:.4f}")

    # 6. Learning-rate validation demo
    print("\n--- Learning-rate validation ---")
    for bad_lr in [-0.1, 0.0, float("nan"), "fast"]:
        try:
            LinearRegression(learning_rate=bad_lr)
        except (ValueError, TypeError) as exc:
            print(f"  lr={bad_lr!r:>8}  → {type(exc).__name__}: {exc}")

    # 7. Loss curve summary (first / mid / last)
    history = model.loss_history
    mid = len(history) // 2
    print("\n--- Loss history sample (epoch : MSE) ---")
    for idx in [0, mid, len(history) - 1]:
        print(f"  epoch {idx + 1:>5} : {history[idx]:.4f}")

    print("\nDone.")


if __name__ == "__main__":
    main()
