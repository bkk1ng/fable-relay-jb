"""
linreg.py - Multivariate linear regression by gradient descent.

Python 3.12, standard library only (no numpy).

Features:
  - Feature standardisation (z-score) with stored parameters for reuse
  - Learning-rate validation (positive check, divergence detection)
  - Loss history tracking (MSE per epoch)
  - R^2 and RMSE metrics on a holdout split
  - CSV loading (header-optional, comma-delimited)
  - __main__ demo on a generated synthetic dataset

Usage:
    python linreg.py                 # run the demo on generated data
    python linreg.py data.csv        # load CSV (last column = target, 20% test)
    python linreg.py data.csv 0.25   # custom test ratio
"""

import csv
import math
import random
import sys

# Type aliases for readability
Matrix = list[list[float]]
Vector = list[float]


# ---------------------------------------------------------------------------
# Data utilities
# ---------------------------------------------------------------------------

def load_csv(path: str, has_header: bool = True) -> tuple[Matrix, Vector]:
    """Load a CSV file and return (X, y).

    The last column is the target; every preceding column is a feature.
    All cells must parse as float.  Set has_header=False for headerless files.
    """
    rows: Matrix = []
    with open(path, newline="") as f:
        all_rows = list(csv.reader(f))

    if has_header and all_rows:
        all_rows = all_rows[1:]

    for raw in all_rows:
        if not raw:
            continue
        rows.append([float(v) for v in raw])

    if not rows:
        raise ValueError(f"No data rows found in {path!r}")

    X = [r[:-1] for r in rows]
    y = [r[-1] for r in rows]
    return X, y


def train_test_split(
    X: Matrix,
    y: Vector,
    test_ratio: float = 0.2,
    seed: int | None = None,
) -> tuple[Matrix, Vector, Matrix, Vector]:
    """Shuffle and split into (X_train, y_train, X_test, y_test)."""
    if not 0.0 < test_ratio < 1.0:
        raise ValueError("test_ratio must be in (0, 1)")

    n = len(X)
    idx = list(range(n))
    rng = random.Random(seed)
    rng.shuffle(idx)

    cut = int(n * (1.0 - test_ratio))
    train_idx, test_idx = idx[:cut], idx[cut:]

    X_train = [X[i] for i in train_idx]
    y_train = [y[i] for i in train_idx]
    X_test = [X[i] for i in test_idx]
    y_test = [y[i] for i in test_idx]
    return X_train, y_train, X_test, y_test


# ---------------------------------------------------------------------------
# Feature standardisation
# ---------------------------------------------------------------------------

class StandardScaler:
    """Z-score standardisation: (x - mean) / std.

    Fit on training data, then transform any data with the same parameters.
    Constant columns (std == 0) are centred but left unscaled (std -> 1).
    """

    def __init__(self) -> None:
        self.means: Vector = []
        self.stds: Vector = []

    def fit(self, X: Matrix) -> "StandardScaler":
        n = len(X)
        if n == 0:
            raise ValueError("Cannot fit on empty matrix")
        k = len(X[0])
        self.means = [sum(row[j] for row in X) / n for j in range(k)]
        self.stds = [
            # population std; guard against divide-by-zero on constant columns
            math.sqrt(sum((row[j] - self.means[j]) ** 2 for row in X) / n) or 1.0
            for j in range(k)
        ]
        return self

    def transform(self, X: Matrix) -> Matrix:
        return [
            [(row[j] - self.means[j]) / self.stds[j] for j in range(len(row))]
            for row in X
        ]

    def fit_transform(self, X: Matrix) -> Matrix:
        self.fit(X)
        return self.transform(X)


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def mean_squared_error(y_true: Vector, y_pred: Vector) -> float:
    n = len(y_true)
    return sum((yt - yp) ** 2 for yt, yp in zip(y_true, y_pred)) / n


def root_mean_squared_error(y_true: Vector, y_pred: Vector) -> float:
    return math.sqrt(mean_squared_error(y_true, y_pred))


def r_squared(y_true: Vector, y_pred: Vector) -> float:
    """Coefficient of determination R^2."""
    n = len(y_true)
    y_bar = sum(y_true) / n
    ss_res = sum((yt - yp) ** 2 for yt, yp in zip(y_true, y_pred))
    ss_tot = sum((yt - y_bar) ** 2 for yt in y_true)
    if ss_tot == 0.0:
        return 0.0
    return 1.0 - ss_res / ss_tot


# ---------------------------------------------------------------------------
# Linear regression via gradient descent
# ---------------------------------------------------------------------------

class LinearRegressionGD:
    """Multivariate linear regression trained by batch gradient descent.

    Parameters
    ----------
    learning_rate : float
        Step size for gradient updates.  Must be strictly positive.
    n_epochs : int
        Number of full passes over the training data.
    tolerance : float
        Early-stop threshold: stop when the absolute change in loss between
        consecutive epochs falls below this value.

    Attributes
    ----------
    weights : list[float]
        Learned coefficients with weights[0] as the bias / intercept.
    loss_history : list[float]
        MSE recorded after each epoch (length <= n_epochs).
    converged : bool
        True if training stopped early due to tolerance.
    diverged : bool
        True if training aborted due to loss explosion (bad learning rate).
    """

    def __init__(
        self,
        learning_rate: float = 0.01,
        n_epochs: int = 1000,
        tolerance: float = 1e-8,
    ) -> None:
        # Learning-rate validation
        if learning_rate <= 0:
            raise ValueError(
                f"learning_rate must be positive, got {learning_rate}"
            )
        if not math.isfinite(learning_rate):
            raise ValueError("learning_rate must be a finite number")
        if n_epochs <= 0:
            raise ValueError(f"n_epochs must be positive, got {n_epochs}")

        self.learning_rate = learning_rate
        self.n_epochs = n_epochs
        self.tolerance = tolerance

        self.weights: Vector = []
        self.loss_history: list[float] = []
        self.converged = False
        self.diverged = False

    # -- core training --------------------------------------------------------

    def fit(self, X: Matrix, y: Vector) -> "LinearRegressionGD":
        n = len(X)
        if n == 0:
            raise ValueError("Training data is empty")
        k = len(X[0])

        # weights[0] = bias, weights[1:] = per-feature coefficients
        self.weights = [0.0] * (k + 1)
        self.loss_history = []
        self.converged = False
        self.diverged = False

        prev_loss = float("inf")

        for epoch in range(self.n_epochs):
            # Forward pass
            preds = [self._predict_row(row) for row in X]
            errors = [preds[i] - y[i] for i in range(n)]

            # MSE loss
            loss = sum(e * e for e in errors) / n
            self.loss_history.append(loss)

            # Divergence detection (learning-rate validation at runtime)
            if math.isnan(loss) or math.isinf(loss):
                self.diverged = True
                raise ValueError(
                    f"Loss became non-finite at epoch {epoch}. "
                    "Learning rate is too high - reduce it and retry."
                )
            if epoch > 10 and loss > prev_loss * 10:
                self.diverged = True
                raise ValueError(
                    f"Loss increased sharply at epoch {epoch} "
                    f"(from {prev_loss:.6e} to {loss:.6e}). "
                    "Learning rate is too high - reduce it and retry."
                )

            # Convergence check
            if epoch > 0 and abs(prev_loss - loss) < self.tolerance:
                self.converged = True
                break

            # Gradients: dMSE/dw = (2/n) * X^T @ errors
            grad_bias = (2.0 / n) * sum(errors)
            grad_w = [
                (2.0 / n) * sum(errors[i] * X[i][j] for i in range(n))
                for j in range(k)
            ]

            # Parameter update
            self.weights[0] -= self.learning_rate * grad_bias
            for j in range(k):
                self.weights[j + 1] -= self.learning_rate * grad_w[j]

            prev_loss = loss

        return self

    # -- inference ------------------------------------------------------------

    def _predict_row(self, row: list[float]) -> float:
        return self.weights[0] + sum(
            self.weights[j + 1] * row[j] for j in range(len(row))
        )

    def predict(self, X: Matrix) -> Vector:
        return [self._predict_row(row) for row in X]

    def score(self, X: Matrix, y: Vector) -> tuple[float, float]:
        """Return (R^2, RMSE) for the given data."""
        preds = self.predict(X)
        return r_squared(y, preds), root_mean_squared_error(y, preds)


# ---------------------------------------------------------------------------
# Synthetic dataset generator (for the demo)
# ---------------------------------------------------------------------------

def generate_dataset(
    n_samples: int = 500,
    n_features: int = 3,
    noise: float = 2.0,
    seed: int = 42,
) -> tuple[Matrix, Vector]:
    """Create a synthetic multivariate regression dataset.

    True model: y = 3.5*x0 - 2.0*x1 + 1.0*x2 + 5.0 + N(0, noise)
    Features are drawn from N(0, 3.0).
    """
    rng = random.Random(seed)
    true_w = [3.5, -2.0, 1.0][:n_features]
    # Pad if more features are requested than the base weights define
    while len(true_w) < n_features:
        true_w.append(rng.uniform(-3.0, 3.0))
    true_b = 5.0

    X: Matrix = []
    y: Vector = []
    for _ in range(n_samples):
        row = [rng.gauss(0.0, 3.0) for _ in range(n_features)]
        target = true_b + sum(true_w[j] * row[j] for j in range(n_features))
        target += rng.gauss(0.0, noise)
        X.append(row)
        y.append(target)
    return X, y


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def main() -> None:
    args = sys.argv[1:]

    if len(args) >= 1:
        # CSV mode: last column is the target
        path = args[0]
        test_ratio = float(args[1]) if len(args) >= 2 else 0.2
        has_header = True
        print(f"Loading {path} ...")
        X, y = load_csv(path, has_header=has_header)
        print(f"  {len(X)} rows, {len(X[0])} features")
    else:
        # Generated-dataset demo
        print("No CSV specified - generating a synthetic dataset.")
        X, y = generate_dataset(n_samples=500, n_features=3, noise=2.0, seed=42)
        print(f"  {len(X)} rows, {len(X[0])} features")
        print("  true weights = [3.5, -2.0, 1.0], bias = 5.0")

    # Holdout split
    X_train, y_train, X_test, y_test = train_test_split(
        X, y, test_ratio=0.2, seed=42
    )
    print(f"  Train: {len(X_train)}   Test: {len(X_test)}")

    # Standardise features (fit on train only, apply to both)
    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X_train)
    X_test_s = scaler.transform(X_test)

    # Train
    lr = 0.01
    n_epochs = 2000
    print(f"\nTraining: lr={lr}, epochs={n_epochs}")
    model = LinearRegressionGD(learning_rate=lr, n_epochs=n_epochs)
    model.fit(X_train_s, y_train)

    if model.converged:
        print(f"  Converged after {len(model.loss_history)} epochs")
    else:
        print(f"  Ran all {len(model.loss_history)} epochs "
              f"({'converged' if model.converged else 'not converged'})")

    print(f"  Initial loss: {model.loss_history[0]:.6f}")
    print(f"  Final loss:   {model.loss_history[-1]:.6f}")

    # Evaluate on holdout
    r2, rmse = model.score(X_test_s, y_test)
    print(f"\nHoldout metrics:")
    print(f"  R^2:   {r2:.6f}")
    print(f"  RMSE:  {rmse:.6f}")

    # Learned coefficients (standardised space)
    print(f"\nLearned weights (standardised space):")
    print(f"  bias  = {model.weights[0]:.4f}")
    for j, w in enumerate(model.weights[1:]):
        print(f"  w[{j}]  = {w:.4f}")

    # Recover coefficients in the original feature scale
    orig_w = [model.weights[j + 1] / scaler.stds[j] for j in range(len(scaler.stds))]
    orig_b = (
        model.weights[0]
        - sum(
            model.weights[j + 1] * scaler.means[j] / scaler.stds[j]
            for j in range(len(scaler.stds))
        )
    )
    print(f"\nRecovered weights (original scale):")
    print(f"  bias  = {orig_b:.4f}")
    for j, w in enumerate(orig_w):
        print(f"  w[{j}]  = {w:.4f}")


if __name__ == "__main__":
    main()
