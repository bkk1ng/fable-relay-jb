#!/usr/bin/env python3
"""
linreg.py - Multivariate linear regression by gradient descent.

Pure Python 3.12 standard library (no numpy).

Features
--------
- CSV loading (header row expected; the target column is configurable).
- Feature standardisation (z-score) fitted on the training split only, so the
  same transform is applied to the held-out split and to new data.
- Full-batch gradient descent with learning-rate validation: the optimiser
  detects divergence (non-finite loss or a loss that increases) and raises a
  clear error pointing at the learning rate.
- Complete loss history per epoch.
- R^2 and RMSE computed on the held-out test split.
- A self-contained __main__ demo on a generated dataset, plus an optional path
  argument to run the whole pipeline on a real CSV file.
"""

from __future__ import annotations

import csv
import math
import random
import sys
from typing import List, Sequence, Tuple


# ---------------------------------------------------------------------------
# Small linear-algebra helpers (plain lists, no external deps)
# ---------------------------------------------------------------------------

def _dot_vec(a: Sequence[float], b: Sequence[float]) -> float:
    """Dot product of two equal-length vectors."""
    return sum(x * y for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# Data loading and splitting
# ---------------------------------------------------------------------------

def load_csv(path: str, target_col: int = -1, skip_header: bool = True
             ) -> Tuple[List[List[float]], List[float]]:
    """Load a CSV file into a feature matrix X and target vector y.

    Parameters
    ----------
    path : str
        Path to the CSV file.
    target_col : int
        Index of the target column. Negative indices count from the end, so
        -1 (the default) means the last column is the target.
    skip_header : bool
        If True, the first non-empty row is treated as a header and skipped.

    Returns
    -------
    (X, y) : X is a list of feature rows, y is the list of targets.
    """
    X: List[List[float]] = []
    y: List[float] = []
    header_seen = not skip_header
    with open(path, "r", newline="") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if not row:
                continue
            if not header_seen:
                header_seen = True
                continue
            values = [float(v) for v in row]
            idx = target_col if target_col >= 0 else len(values) + target_col
            if not 0 <= idx < len(values):
                raise ValueError(
                    f"target_col {target_col} is out of range for a row with "
                    f"{len(values)} columns."
                )
            y.append(values.pop(idx))
            X.append(values)
    return X, y


def train_test_split(X: Sequence[Sequence[float]], y: Sequence[float],
                     test_size: float = 0.2, seed: int | None = None
                     ) -> Tuple[List[List[float]], List[List[float]],
                                List[float], List[float]]:
    """Shuffle and split data into train and test partitions.

    Returns (X_train, X_test, y_train, y_test).
    """
    if not 0.0 < test_size < 1.0:
        raise ValueError("test_size must be in the open interval (0, 1).")
    if len(X) != len(y):
        raise ValueError("X and y must have the same number of rows.")
    indices = list(range(len(X)))
    random.Random(seed).shuffle(indices)
    cut = int(round(len(indices) * (1.0 - test_size)))
    train_idx, test_idx = indices[:cut], indices[cut:]
    return (
        [X[i] for i in train_idx],
        [X[i] for i in test_idx],
        [y[i] for i in train_idx],
        [y[i] for i in test_idx],
    )


# ---------------------------------------------------------------------------
# Feature standardisation
# ---------------------------------------------------------------------------

class StandardScaler:
    """Z-score standardisation fitted on a training set.

    Stores per-feature mean and standard deviation so the exact same transform
    can be replayed on the held-out split and on unseen data. Features with zero
    variance are left centred (std defaults to 1.0 so the division is a no-op).
    """

    def __init__(self) -> None:
        self.mean_: List[float] = []
        self.std_: List[float] = []
        self.n_features_: int = 0

    def fit(self, X: Sequence[Sequence[float]]) -> "StandardScaler":
        if not X:
            raise ValueError("Cannot fit on an empty dataset.")
        self.n_features_ = len(X[0])
        self.mean_ = [0.0] * self.n_features_
        self.std_ = [0.0] * self.n_features_
        n = len(X)
        for row in X:
            for j in range(self.n_features_):
                self.mean_[j] += row[j]
        for j in range(self.n_features_):
            self.mean_[j] /= n
        for row in X:
            for j in range(self.n_features_):
                d = row[j] - self.mean_[j]
                self.std_[j] += d * d
        for j in range(self.n_features_):
            variance = self.std_[j] / n
            self.std_[j] = math.sqrt(variance) if variance > 0.0 else 1.0
        return self

    def transform(self, X: Sequence[Sequence[float]]) -> List[List[float]]:
        if len(X) and len(X[0]) != self.n_features_:
            raise ValueError(
                f"Expected {self.n_features_} features, got {len(X[0])}."
            )
        return [
            [(row[j] - self.mean_[j]) / self.std_[j]
             for j in range(self.n_features_)]
            for row in X
        ]

    def fit_transform(self, X: Sequence[Sequence[float]]) -> List[List[float]]:
        self.fit(X)
        return self.transform(X)


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def mean_squared_error(y_true: Sequence[float], y_pred: Sequence[float]) -> float:
    if len(y_true) != len(y_pred):
        raise ValueError("y_true and y_pred must have the same length.")
    n = len(y_true)
    return sum((t - p) ** 2 for t, p in zip(y_true, y_pred)) / n


def root_mean_squared_error(y_true: Sequence[float], y_pred: Sequence[float]) -> float:
    return math.sqrt(mean_squared_error(y_true, y_pred))


def r2_score(y_true: Sequence[float], y_pred: Sequence[float]) -> float:
    """Coefficient of determination: 1 - SS_res / SS_tot."""
    if len(y_true) != len(y_pred):
        raise ValueError("y_true and y_pred must have the same length.")
    n = len(y_true)
    if n == 0:
        raise ValueError("Cannot score an empty set.")
    mean_y = sum(y_true) / n
    ss_tot = sum((t - mean_y) ** 2 for t in y_true)
    ss_res = sum((t - p) ** 2 for t, p in zip(y_true, y_pred))
    if ss_tot == 0.0:
        # No variance in the target: nothing to explain.
        return 0.0
    return 1.0 - ss_res / ss_tot


# ---------------------------------------------------------------------------
# Linear regression by gradient descent
# ---------------------------------------------------------------------------

class LinearRegression:
    """Multivariate linear regression trained with full-batch gradient descent.

    The objective is the mean squared error. Because MSE is convex and the
    optimiser uses full-batch updates, the loss must decrease monotonically for
    any learning rate below the stability threshold. The optimiser validates
    the supplied learning rate by watching for divergence:

      * non-finite loss (NaN / inf) -> the step exploded;
      * a loss that increases beyond floating-point noise -> the step is too
        large to be stable.

    In both cases it raises a ValueError naming the learning rate as the cause.

    Attributes
    ----------
    weights_ : list[float]   coefficients in the (standardised) feature space.
    bias_ : float            intercept in the standardised space.
    loss_history_ : list[float]  MSE recorded at the start of every epoch.
    """

    def __init__(self, learning_rate: float = 0.01, n_iter: int = 1000,
                 tol: float = 1e-9, seed: int | None = None) -> None:
        if learning_rate <= 0.0:
            raise ValueError("learning_rate must be positive.")
        if n_iter <= 0:
            raise ValueError("n_iter must be positive.")
        if tol < 0.0:
            raise ValueError("tol must be non-negative.")
        self.learning_rate = learning_rate
        self.n_iter = n_iter
        self.tol = tol
        self.seed = seed
        self.weights_: List[float] = []
        self.bias_: float = 0.0
        self.loss_history_: List[float] = []
        self.n_features_: int = 0

    # -- internals ----------------------------------------------------------

    def _forward(self, X: Sequence[Sequence[float]]) -> List[float]:
        return [_dot_vec(self.weights_, row) + self.bias_ for row in X]

    def _gradient_step(self, X: Sequence[Sequence[float]],
                       y: Sequence[float]) -> float:
        """One full-batch gradient-descent step.

        Returns the MSE computed with the weights *before* this update, so the
        recorded loss history lines up with the parameters that produced it.
        """
        preds = self._forward(X)
        loss = mean_squared_error(y, preds)
        n = len(X)
        errors = [preds[i] - y[i] for i in range(n)]

        # d MSE / d w_j = (2 / n) * sum_i x_ij * error_i
        grad_w = [0.0] * self.n_features_
        for i, row in enumerate(X):
            e = errors[i]
            for j in range(self.n_features_):
                grad_w[j] += row[j] * e
        scale = 2.0 / n
        for j in range(self.n_features_):
            grad_w[j] *= scale

        # d MSE / d b = (2 / n) * sum_i error_i
        grad_b = scale * sum(errors)

        # parameter update
        for j in range(self.n_features_):
            self.weights_[j] -= self.learning_rate * grad_w[j]
        self.bias_ -= self.learning_rate * grad_b
        return loss

    # -- public API ---------------------------------------------------------

    def fit(self, X: Sequence[Sequence[float]], y: Sequence[float]
            ) -> "LinearRegression":
        if not X or not y:
            raise ValueError("Cannot fit on empty data.")
        if len(X) != len(y):
            raise ValueError("X and y must have the same number of rows.")
        self.n_features_ = len(X[0])
        rng = random.Random(self.seed)
        # Small random init breaks symmetry between features; bias starts at 0.
        self.weights_ = [rng.uniform(-0.01, 0.01) for _ in range(self.n_features_)]
        self.bias_ = 0.0
        self.loss_history_ = []

        for epoch in range(self.n_iter):
            loss = self._gradient_step(X, y)
            self.loss_history_.append(loss)

            # --- learning-rate validation ---------------------------------
            if not math.isfinite(loss):
                raise ValueError(
                    f"Learning rate {self.learning_rate} is too high: loss "
                    f"diverged to {loss} at epoch {epoch}. "
                    "Try a smaller learning rate (e.g. halve it)."
                )
            if epoch > 0:
                prev = self.loss_history_[epoch - 1]
                # On a convex objective with full-batch GD the loss cannot
                # increase for a stable step size. Allow only floating-point
                # noise; anything beyond that is a diverging learning rate.
                if loss > prev * (1.0 + 1e-9) + 1e-12:
                    raise ValueError(
                        f"Learning rate {self.learning_rate} is too high: loss "
                        f"increased from {prev} to {loss} at epoch {epoch}. "
                        "Try a smaller learning rate (e.g. halve it)."
                    )
                # early stopping once the loss stops meaningfully improving
                improvement = prev - loss
                if 0.0 <= improvement < self.tol:
                    break
        return self

    def predict(self, X: Sequence[Sequence[float]]) -> List[float]:
        if not self.weights_:
            raise RuntimeError("Model must be fitted before predicting.")
        if len(X) and len(X[0]) != self.n_features_:
            raise ValueError(
                f"Expected {self.n_features_} features, got {len(X[0])}."
            )
        return self._forward(X)

    def score(self, X: Sequence[Sequence[float]], y: Sequence[float]) -> float:
        """R^2 on the given data."""
        return r2_score(y, self.predict(X))

    def rmse(self, X: Sequence[Sequence[float]], y: Sequence[float]) -> float:
        """Root mean squared error on the given data."""
        return root_mean_squared_error(y, self.predict(X))


def find_learning_rate(X: Sequence[Sequence[float]], y: Sequence[float],
                       start: float = 1.0, decay: float = 0.5,
                       n_trials: int = 12, n_epochs: int = 50,
                       seed: int | None = 0) -> float:
    """Search a geometric sequence of learning rates from high to low.

    Returns the largest learning rate that trains for n_epochs without the
    optimiser flagging divergence. This is a cheap, model-aware way to pick a
    step size instead of guessing.
    """
    if not 0.0 < decay < 1.0:
        raise ValueError("decay must be in the open interval (0, 1).")
    lr = start
    for _ in range(n_trials):
        model = LinearRegression(learning_rate=lr, n_iter=n_epochs, seed=seed)
        try:
            model.fit(X, y)
        except ValueError:
            lr *= decay
            continue
        return lr
    # Every candidate diverged; return the smallest tried as a safe fallback.
    return lr


# ---------------------------------------------------------------------------
# Synthetic dataset for the demo
# ---------------------------------------------------------------------------

def generate_dataset(n: int = 400, noise: float = 2.0,
                     seed: int = 42) -> Tuple[List[List[float]], List[float],
                                              List[float], float]:
    """Generate a regression dataset with known coefficients.

    Features are drawn from deliberately different scales so that
    standardisation is clearly needed. The true relationship is linear with a
    fixed intercept plus Gaussian noise.

    Returns (X, y, true_weights, true_bias).
    """
    rng = random.Random(seed)
    # Chosen with varied magnitudes and signs, and varied feature scales.
    true_weights = [3.0, -2.0, 4.0]
    true_bias = 5.0
    X: List[List[float]] = []
    y: List[float] = []
    for _ in range(n):
        # Different ranges per feature: [-10,10], [0,5], N(0,3).
        x1 = rng.uniform(-10.0, 10.0)
        x2 = rng.uniform(0.0, 5.0)
        x3 = rng.gauss(0.0, 3.0)
        row = [x1, x2, x3]
        target = true_bias + _dot_vec(true_weights, row) + rng.gauss(0.0, noise)
        X.append(row)
        y.append(target)
    return X, y, true_weights, true_bias


def _unscale_weights(scaler: StandardScaler, weights: List[float],
                    bias: float) -> Tuple[List[float], float]:
    """Map coefficients from standardised feature space back to the original
    feature space.

    With z = (x - mean) / std, the model predicts
        y = w_std . z + b_std
          = sum_j (w_std_j / std_j) * x_j + (b_std - sum_j w_std_j * mean_j / std_j)
    """
    w_orig = [weights[j] / scaler.std_[j] for j in range(scaler.n_features_)]
    b_orig = bias - sum(
        weights[j] * scaler.mean_[j] / scaler.std_[j]
        for j in range(scaler.n_features_)
    )
    return w_orig, b_orig


# ---------------------------------------------------------------------------
# Demo entry point
# ---------------------------------------------------------------------------

def main() -> None:
    using_generated = len(sys.argv) <= 1
    if using_generated:
        X, y, true_w, true_b = generate_dataset(n=400, noise=2.0, seed=42)
        print("Generated dataset: 400 samples, 3 features, true bias 5.0")
        print(f"True weights: {true_w}")
    else:
        path = sys.argv[1]
        X, y = load_csv(path)
        print(f"Loaded {len(X)} rows, {len(X[0])} features from {path}")
        true_w = None
        true_b = None

    # Holdout split ----------------------------------------------------------
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, seed=0
    )
    print(f"Split: {len(X_train)} train / {len(X_test)} test")

    # Standardise using training statistics only ----------------------------
    scaler = StandardScaler().fit(X_train)
    X_train_s = scaler.transform(X_train)
    X_test_s = scaler.transform(X_test)

    # Learning-rate validation ---------------------------------------------
    lr = find_learning_rate(X_train_s, y_train, start=1.0, decay=0.5,
                            n_trials=12, n_epochs=50, seed=0)
    print(f"Validated learning rate: {lr}")

    # Train -----------------------------------------------------------------
    model = LinearRegression(learning_rate=lr, n_iter=2000, tol=1e-10, seed=0)
    model.fit(X_train_s, y_train)

    # Evaluate on the held-out split ---------------------------------------
    preds = model.predict(X_test_s)
    r2 = r2_score(y_test, preds)
    rmse = root_mean_squared_error(y_test, preds)
    print()
    print("Holdout metrics")
    print(f"  R^2 : {r2:.6f}")
    print(f"  RMSE: {rmse:.6f}")

    # Loss history summary --------------------------------------------------
    print()
    print("Loss history")
    print(f"  epochs run: {len(model.loss_history_)}")
    print(f"  initial loss: {model.loss_history_[0]:.6f}")
    print(f"  final loss:   {model.loss_history_[-1]:.6f}")

    # Learned parameters back in the original feature space ----------------
    w_orig, b_orig = _unscale_weights(scaler, model.weights_, model.bias_)
    print()
    print("Learned parameters (original feature space)")
    print(f"  weights: {[round(w, 4) for w in w_orig]}")
    print(f"  bias:    {round(b_orig, 4)}")
    if true_w is not None:
        print(f"  true weights: {true_w}")
        print(f"  true bias:    {true_b}")

    # Demonstrate that an absurd learning rate is caught -------------------
    print()
    print("Learning-rate validation check (lr=10.0 should be rejected):")
    try:
        LinearRegression(learning_rate=10.0, n_iter=100, seed=0).fit(
            X_train_s, y_train
        )
        print("  no error raised (unexpected)")
    except ValueError as exc:
        print(f"  rejected: {exc}")


if __name__ == "__main__":
    main()
