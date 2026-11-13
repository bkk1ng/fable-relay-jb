"""
linreg.py — Multivariate linear regression by gradient descent.

Features
--------
- Feature standardisation (z-score, fit on train, applied to any split)
- Learning-rate validation with early-divergence guard
- Per-epoch MSE loss history
- R² and RMSE on a held-out test split
- CSV loading / saving (stdlib csv module)
- __main__ demo on a generated synthetic dataset

Python 3.12, stdlib only — no numpy, no third-party packages.
"""

from __future__ import annotations

import csv
import math
import os
import random
import statistics
import warnings
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# Linear algebra helpers (pure Python, list-of-lists)
# ---------------------------------------------------------------------------

def _dot(a: list[float], b: list[float]) -> float:
    """Dot product of two equal-length vectors."""
    return sum(x * y for x, y in zip(a, b))


def _mat_vec_mul(A: list[list[float]], v: list[float]) -> list[float]:
    """Matrix × vector multiply.  A is (m × n), v is (n,) → (m,)."""
    return [_dot(row, v) for row in A]


def _transpose(A: list[list[float]]) -> list[list[float]]:
    """Transpose (m × n) → (n × m)."""
    if not A:
        return []
    m, n = len(A), len(A[0])
    return [[A[i][j] for i in range(m)] for j in range(n)]


def _prepend_ones(X: list[list[float]]) -> list[list[float]]:
    """Add a leading 1 to every row (bias / intercept column)."""
    return [[1.0] + row for row in X]


# ---------------------------------------------------------------------------
# Standardisation
# ---------------------------------------------------------------------------

class StandardScaler:
    """
    Z-score scaler: fit on training data, transform any split.

    Uses population standard deviation so a single-sample feature column
    still produces a defined (zero) result rather than raising.
    """

    def __init__(self) -> None:
        self.means_: list[float] = []
        self.stds_: list[float] = []

    def fit(self, X: list[list[float]]) -> "StandardScaler":
        n_features = len(X[0])
        self.means_ = []
        self.stds_ = []
        for j in range(n_features):
            col = [X[i][j] for i in range(len(X))]
            mu = statistics.mean(col)
            sigma = statistics.pstdev(col)
            self.means_.append(mu)
            self.stds_.append(sigma if sigma > 0.0 else 1.0)
        return self

    def transform(self, X: list[list[float]]) -> list[list[float]]:
        if not self.means_:
            raise RuntimeError("StandardScaler has not been fitted.")
        return [
            [(X[i][j] - self.means_[j]) / self.stds_[j] for j in range(len(X[0]))]
            for i in range(len(X))
        ]

    def fit_transform(self, X: list[list[float]]) -> list[list[float]]:
        return self.fit(X).transform(X)


# ---------------------------------------------------------------------------
# Learning-rate validation
# ---------------------------------------------------------------------------

class LearningRateError(ValueError):
    """Raised when the supplied learning rate is invalid."""


def _validate_learning_rate(lr: float) -> None:
    """
    Raise LearningRateError if lr is non-numeric, non-finite, or ≤ 0.
    Issue a UserWarning if lr > 10 (unusual range, likely to diverge).
    """
    if not isinstance(lr, (int, float)):
        raise LearningRateError(
            f"learning_rate must be a float, got {type(lr).__name__}."
        )
    if math.isnan(lr) or math.isinf(lr):
        raise LearningRateError(
            f"learning_rate must be finite, got {lr!r}."
        )
    if lr <= 0.0:
        raise LearningRateError(
            f"learning_rate must be > 0, got {lr}."
        )
    if lr > 10.0:
        warnings.warn(
            f"learning_rate={lr} is unusually large (> 10); "
            "gradient descent may diverge.",
            UserWarning,
            stacklevel=3,
        )


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def mse(y_true: list[float], y_pred: list[float]) -> float:
    n = len(y_true)
    return sum((a - b) ** 2 for a, b in zip(y_true, y_pred)) / n


def rmse(y_true: list[float], y_pred: list[float]) -> float:
    return math.sqrt(mse(y_true, y_pred))


def r_squared(y_true: list[float], y_pred: list[float]) -> float:
    mean_y = statistics.mean(y_true)
    ss_tot = sum((y - mean_y) ** 2 for y in y_true)
    ss_res = sum((a - b) ** 2 for a, b in zip(y_true, y_pred))
    if ss_tot == 0.0:
        return 1.0 if ss_res == 0.0 else 0.0
    return 1.0 - ss_res / ss_tot


# ---------------------------------------------------------------------------
# Train / test split
# ---------------------------------------------------------------------------

def train_test_split(
    X: list[list[float]],
    y: list[float],
    test_size: float = 0.2,
    seed: int | None = None,
) -> tuple[list[list[float]], list[list[float]], list[float], list[float]]:
    """
    Randomly shuffle then split (X, y) into train and test subsets.

    Parameters
    ----------
    X         : feature matrix
    y         : target vector
    test_size : fraction of samples to place in the test set (0 < test_size < 1)
    seed      : random seed for reproducibility

    Returns
    -------
    X_train, X_test, y_train, y_test
    """
    if not (0.0 < test_size < 1.0):
        raise ValueError(f"test_size must be in (0, 1), got {test_size}.")
    n = len(X)
    indices = list(range(n))
    random.Random(seed).shuffle(indices)
    n_test = max(1, int(n * test_size))
    test_idx  = set(indices[:n_test])
    train_idx = [i for i in indices if i not in test_idx]
    test_idx  = indices[:n_test]
    return (
        [X[i] for i in train_idx],
        [X[i] for i in test_idx],
        [y[i] for i in train_idx],
        [y[i] for i in test_idx],
    )


# ---------------------------------------------------------------------------
# CSV I/O
# ---------------------------------------------------------------------------

def load_csv(
    path: str,
    target_col: int = -1,
    has_header: bool = True,
) -> tuple[list[list[float]], list[float], list[str]]:
    """
    Load a CSV file into a feature matrix and target vector.

    Parameters
    ----------
    path       : path to the CSV file
    target_col : column index of the target variable; -1 = last column
    has_header : True if the first row contains column names

    Returns
    -------
    X              : list[list[float]] — feature matrix (all columns except target)
    y              : list[float]       — target vector
    feature_names  : list[str]         — feature column names (empty if no header)
    """
    with open(path, newline="", encoding="utf-8") as fh:
        rows = list(csv.reader(fh))

    if not rows:
        raise ValueError(f"CSV file '{path}' is empty.")

    feature_names: list[str] = []
    start = 0

    if has_header:
        header = rows[0]
        start = 1
        n_cols = len(header)
        tc = target_col if target_col >= 0 else n_cols + target_col
        feature_names = [header[j] for j in range(n_cols) if j != tc]

    X: list[list[float]] = []
    y: list[float] = []

    for lineno, row in enumerate(rows[start:], start=start + 1):
        if not any(cell.strip() for cell in row):   # skip blank lines
            continue
        try:
            vals = [float(cell.strip()) for cell in row]
        except ValueError as exc:
            raise ValueError(f"Non-numeric value at line {lineno}: {exc}") from exc
        n_cols = len(vals)
        tc = target_col if target_col >= 0 else n_cols + target_col
        y.append(vals[tc])
        X.append([vals[j] for j in range(n_cols) if j != tc])

    if not X:
        raise ValueError(f"CSV file '{path}' contains no data rows.")

    return X, y, feature_names


def save_csv(
    path: str,
    X: list[list[float]],
    y: list[float],
    feature_names: list[str] | None = None,
    target_name: str = "target",
) -> None:
    """Write (X, y) to a CSV file with an optional header row."""
    n_features = len(X[0]) if X else 0
    if feature_names is None:
        feature_names = [f"x{i}" for i in range(n_features)]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(feature_names + [target_name])
        for xi, yi in zip(X, y):
            writer.writerow(list(xi) + [yi])


# ---------------------------------------------------------------------------
# Linear regression model
# ---------------------------------------------------------------------------

@dataclass
class LinearRegression:
    """
    Multivariate linear regression trained by batch gradient descent.

    Parameters
    ----------
    learning_rate : step size η for the weight update rule
    n_epochs      : number of full passes over the training data
    standardize   : z-score features before training (recommended)
    verbose       : print loss every `verbose` epochs; 0 = silent

    Attributes set after fit()
    --------------------------
    weights_       : full weight vector [bias, w₁, w₂, …, wₙ]
    coef_          : feature weights (excludes bias)
    intercept_     : bias term
    loss_history_  : MSE at the end of every epoch
    scaler_        : fitted StandardScaler (None if standardize=False)
    n_features_    : number of input features
    """

    learning_rate: float = 0.01
    n_epochs: int = 1_000
    standardize: bool = True
    verbose: int = 0

    weights_: list[float] = field(default_factory=list, init=False, repr=False)
    loss_history_: list[float] = field(default_factory=list, init=False, repr=False)
    scaler_: StandardScaler | None = field(default=None, init=False, repr=False)
    n_features_: int = field(default=0, init=False, repr=False)

    def __post_init__(self) -> None:
        _validate_learning_rate(self.learning_rate)
        if not isinstance(self.n_epochs, int) or self.n_epochs < 1:
            raise ValueError(f"n_epochs must be a positive integer, got {self.n_epochs}.")

    # ------------------------------------------------------------------
    def fit(self, X: list[list[float]], y: list[float]) -> "LinearRegression":
        """
        Fit the model using batch gradient descent.

        The update rule at each epoch is:
            ŷ = X_aug @ w
            L = (1/n) ‖ŷ − y‖²
            ∇L = (2/n) X_aug.T @ (ŷ − y)
            w  ← w − η · ∇L

        Parameters
        ----------
        X : (n_samples × n_features) feature matrix
        y : (n_samples,) target vector
        """
        if len(X) != len(y):
            raise ValueError(
                f"X has {len(X)} rows but y has {len(y)} elements."
            )
        if len(X) == 0:
            raise ValueError("Training data is empty.")

        self.n_features_ = len(X[0])

        # --- optional standardisation ---
        if self.standardize:
            self.scaler_ = StandardScaler()
            X_proc = self.scaler_.fit_transform(X)
        else:
            self.scaler_ = None
            X_proc = [row[:] for row in X]

        # --- augment with bias column ---
        X_aug = _prepend_ones(X_proc)          # shape: n × (d+1)
        n, d_aug = len(X_aug), len(X_aug[0])

        # --- initialise weights to zero ---
        self.weights_ = [0.0] * d_aug
        self.loss_history_ = []
        lr = self.learning_rate
        epoch_width = len(str(self.n_epochs))

        for epoch in range(1, self.n_epochs + 1):
            # forward pass
            y_pred = _mat_vec_mul(X_aug, self.weights_)

            # residuals and MSE loss
            residuals = [yh - yt for yh, yt in zip(y_pred, y)]
            loss = sum(r * r for r in residuals) / n
            self.loss_history_.append(loss)

            # divergence guard
            if math.isnan(loss) or math.isinf(loss):
                raise RuntimeError(
                    f"Loss became {loss} at epoch {epoch}. "
                    "Reduce learning_rate and re-fit."
                )

            # gradients: (2/n) · Xᵀ · residuals
            X_T = _transpose(X_aug)
            grad = _mat_vec_mul(X_T, residuals)
            scale = 2.0 / n
            self.weights_ = [w - lr * scale * g for w, g in zip(self.weights_, grad)]

            if self.verbose and epoch % self.verbose == 0:
                print(
                    f"  epoch {epoch:{epoch_width}d}/{self.n_epochs}"
                    f"  loss={loss:.6f}"
                )

        return self

    # ------------------------------------------------------------------
    def predict(self, X: list[list[float]]) -> list[float]:
        """Return predictions for the given feature matrix."""
        if not self.weights_:
            raise RuntimeError("Model has not been fitted yet. Call fit() first.")
        X_proc = self.scaler_.transform(X) if self.scaler_ is not None else X
        return _mat_vec_mul(_prepend_ones(X_proc), self.weights_)

    # ------------------------------------------------------------------
    def evaluate(
        self,
        X: list[list[float]],
        y: list[float],
    ) -> dict[str, float]:
        """
        Compute regression metrics on the given split.

        Returns a dict with keys 'rmse', 'r2', and 'mse'.
        """
        y_pred = self.predict(X)
        return {
            "rmse": rmse(y, y_pred),
            "r2":   r_squared(y, y_pred),
            "mse":  mse(y, y_pred),
        }

    # ------------------------------------------------------------------
    @property
    def coef_(self) -> list[float]:
        """Learned feature weights (excluding the bias / intercept)."""
        return self.weights_[1:]

    @property
    def intercept_(self) -> float:
        """Learned bias term (w₀)."""
        return self.weights_[0]


# ---------------------------------------------------------------------------
# Demo helpers
# ---------------------------------------------------------------------------

def _generate_dataset(
    n_samples: int,
    true_weights: list[float],
    true_bias: float = 0.0,
    noise_std: float = 1.0,
    seed: int = 42,
) -> tuple[list[list[float]], list[float]]:
    """
    Generate a synthetic regression dataset:
        y = X @ true_weights + true_bias + ε,   ε ~ N(0, noise_std²)

    Features are drawn from N(0, 1).
    """
    rng = random.Random(seed)
    n_features = len(true_weights)
    X = [
        [rng.gauss(0.0, 1.0) for _ in range(n_features)]
        for _ in range(n_samples)
    ]
    y = [
        _dot(X[i], true_weights) + true_bias + rng.gauss(0.0, noise_std)
        for i in range(n_samples)
    ]
    return X, y


def _hr(title: str = "", width: int = 56) -> None:
    if title:
        pad = max(0, width - len(title) - 4)
        print(f"\n── {title} {'─' * pad}")
    else:
        print("─" * width)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    # ── 1. Generate data ───────────────────────────────────────────────
    _hr("1. Generate synthetic dataset")

    TRUE_WEIGHTS = [3.0, -1.5, 2.8, 0.7]
    TRUE_BIAS    = 5.0
    NOISE_STD    = 2.0
    N_SAMPLES    = 600
    N_FEATURES   = len(TRUE_WEIGHTS)
    CSV_PATH     = "_linreg_demo_data.csv"

    X, y = _generate_dataset(
        n_samples=N_SAMPLES,
        true_weights=TRUE_WEIGHTS,
        true_bias=TRUE_BIAS,
        noise_std=NOISE_STD,
        seed=42,
    )
    print(f"  samples  : {N_SAMPLES}")
    print(f"  features : {N_FEATURES}")
    print(f"  true w   : {TRUE_WEIGHTS}")
    print(f"  true b   : {TRUE_BIAS}")

    # ── 2. CSV round-trip ──────────────────────────────────────────────
    _hr("2. CSV save → load")

    feature_names = [f"feature_{i}" for i in range(N_FEATURES)]
    save_csv(CSV_PATH, X, y, feature_names=feature_names, target_name="target")
    print(f"  saved  → {CSV_PATH}")

    X_loaded, y_loaded, fnames = load_csv(CSV_PATH)
    print(f"  loaded ← {len(X_loaded)} rows × {len(X_loaded[0])} features")
    print(f"  cols   : {fnames + ['target']}")

    # ── 3. Train / test split ──────────────────────────────────────────
    _hr("3. Train / test split  (80 / 20)")

    X_train, X_test, y_train, y_test = train_test_split(
        X_loaded, y_loaded, test_size=0.2, seed=0
    )
    print(f"  train : {len(X_train)} samples")
    print(f"  test  : {len(X_test)} samples")

    # ── 4. Fit ─────────────────────────────────────────────────────────
    _hr("4. Gradient descent  (lr=0.05, 2 000 epochs)")

    model = LinearRegression(
        learning_rate=0.05,
        n_epochs=2_000,
        standardize=True,
        verbose=400,
    )
    model.fit(X_train, y_train)

    # ── 5. Metrics ─────────────────────────────────────────────────────
    _hr("5. Metrics on holdout set")

    m_train = model.evaluate(X_train, y_train)
    m_test  = model.evaluate(X_test,  y_test)

    col = 14
    print(f"  {'metric':<8}  {'train':>{col}}  {'test':>{col}}")
    print(f"  {'──────':<8}  {'─────':>{col}}  {'────':>{col}}")
    for key in ("rmse", "r2", "mse"):
        label = "R²" if key == "r2" else key.upper()
        print(f"  {label:<8}  {m_train[key]:>{col}.5f}  {m_test[key]:>{col}.5f}")

    # ── 6. Learned parameters ──────────────────────────────────────────
    _hr("6. Learned vs. true parameters")

    print(f"  {'param':<10}  {'learned':>10}  {'true':>10}")
    print(f"  {'─────':<10}  {'───────':>10}  {'────':>10}")
    print(f"  {'bias':<10}  {model.intercept_:>10.4f}  {TRUE_BIAS:>10.4f}")
    for i, (learned, true) in enumerate(zip(model.coef_, TRUE_WEIGHTS)):
        print(f"  {'w[' + str(i) + ']':<10}  {learned:>10.4f}  {true:>10.4f}")

    # ── 7. Loss curve ──────────────────────────────────────────────────
    _hr("7. Loss curve (sampled)")

    history = model.loss_history_
    n_hist  = len(history)
    n_steps = 10
    step    = max(1, n_hist // n_steps)

    print(f"  {'epoch':>6}  {'MSE loss':>12}")
    print(f"  {'─────':>6}  {'────────':>12}")
    for i in range(0, n_hist, step):
        print(f"  {i + 1:>6}  {history[i]:>12.6f}")
    # always show the final epoch
    if (n_hist - 1) % step != 0:
        print(f"  {n_hist:>6}  {history[-1]:>12.6f}")

    # ── cleanup ────────────────────────────────────────────────────────
    try:
        os.remove(CSV_PATH)
    except OSError:
        pass

    _hr()
    print("  Done.\n")


if __name__ == "__main__":
    main()
