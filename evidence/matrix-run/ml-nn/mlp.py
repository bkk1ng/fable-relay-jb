"""
Tiny MLP from scratch — Python 3.12, stdlib only.

Architecture : Dense(2->4, ReLU) -> Dense(4->1, Sigmoid)
Loss         : MSE
Optimizer    : SGD with momentum
Task         : XOR
"""

import math
import random


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def dot(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def relu(x: float) -> float:
    return max(0.0, x)


def relu_grad(x: float) -> float:
    """Derivative of ReLU w.r.t. its pre-activation input."""
    return 1.0 if x > 0.0 else 0.0


def sigmoid(x: float) -> float:
    # Clamp to avoid overflow in exp.
    x = max(-500.0, min(500.0, x))
    return 1.0 / (1.0 + math.exp(-x))


def sigmoid_grad(a: float) -> float:
    """Derivative of sigmoid w.r.t. its pre-activation input (given a = sigmoid(x))."""
    return a * (1.0 - a)


# ---------------------------------------------------------------------------
# He initialisation
# ---------------------------------------------------------------------------

def he_weights(fan_in: int, fan_out: int) -> list[list[float]]:
    """Return a [fan_out][fan_in] weight matrix using He normal init."""
    std = math.sqrt(2.0 / fan_in)
    return [
        [random.gauss(0.0, std) for _ in range(fan_in)]
        for _ in range(fan_out)
    ]


def zero_matrix(rows: int, cols: int) -> list[list[float]]:
    return [[0.0] * cols for _ in range(rows)]


def zero_vector(n: int) -> list[float]:
    return [0.0] * n


# ---------------------------------------------------------------------------
# Dense layer
# ---------------------------------------------------------------------------

class Dense:
    """
    Fully-connected layer.

    Parameters
    ----------
    in_features  : int
    out_features : int
    activation   : 'relu' | 'sigmoid'
    """

    def __init__(self, in_features: int, out_features: int, activation: str) -> None:
        self.in_features = in_features
        self.out_features = out_features
        self.activation = activation

        # Weights: [out_features][in_features], He init
        self.W: list[list[float]] = he_weights(in_features, out_features)
        self.b: list[float] = zero_vector(out_features)

        # Momentum buffers (same shape as parameters)
        self.vW: list[list[float]] = zero_matrix(out_features, in_features)
        self.vb: list[float] = zero_vector(out_features)

        # Cached values needed for backprop
        self._x: list[float] = []        # layer input
        self._z: list[float] = []        # pre-activations
        self._a: list[float] = []        # post-activations (layer output)

    # ------------------------------------------------------------------
    # Forward pass
    # ------------------------------------------------------------------

    def forward(self, x: list[float]) -> list[float]:
        self._x = x
        self._z = [dot(self.W[j], x) + self.b[j] for j in range(self.out_features)]

        if self.activation == 'relu':
            self._a = [relu(z) for z in self._z]
        else:  # sigmoid
            self._a = [sigmoid(z) for z in self._z]

        return self._a

    # ------------------------------------------------------------------
    # Backward pass
    # ------------------------------------------------------------------

    def backward(self, d_out: list[float]) -> list[float]:
        """
        Receives gradient of loss w.r.t. this layer's output (d_out).
        Returns gradient of loss w.r.t. this layer's input (d_in).
        Accumulates weight / bias gradients internally.
        """
        # Gradient through activation function -> d_z[j] = d_out[j] * act'(z[j])
        if self.activation == 'relu':
            d_z = [d_out[j] * relu_grad(self._z[j]) for j in range(self.out_features)]
        else:
            d_z = [d_out[j] * sigmoid_grad(self._a[j]) for j in range(self.out_features)]

        # Gradient w.r.t. weights: dL/dW[j][k] = d_z[j] * x[k]
        self._dW = [[d_z[j] * self._x[k]
                     for k in range(self.in_features)]
                    for j in range(self.out_features)]

        # Gradient w.r.t. biases: dL/db[j] = d_z[j]
        self._db = list(d_z)

        # Gradient w.r.t. input: dL/dx[k] = sum_j d_z[j] * W[j][k]
        d_in = [
            sum(d_z[j] * self.W[j][k] for j in range(self.out_features))
            for k in range(self.in_features)
        ]

        return d_in

    # ------------------------------------------------------------------
    # SGD with momentum update
    # ------------------------------------------------------------------

    def step(self, lr: float, momentum: float) -> None:
        """Apply accumulated gradients to parameters."""
        for j in range(self.out_features):
            for k in range(self.in_features):
                self.vW[j][k] = momentum * self.vW[j][k] + self._dW[j][k]
                self.W[j][k] -= lr * self.vW[j][k]
            self.vb[j] = momentum * self.vb[j] + self._db[j]
            self.b[j] -= lr * self.vb[j]


# ---------------------------------------------------------------------------
# MLP
# ---------------------------------------------------------------------------

class MLP:
    """
    Two-layer MLP for binary classification.

    Dense(2 -> hidden, ReLU) -> Dense(hidden -> 1, Sigmoid)
    """

    def __init__(self, hidden: int = 4) -> None:
        self.l1 = Dense(2, hidden, activation='relu')
        self.l2 = Dense(hidden, 1, activation='sigmoid')

    def forward(self, x: list[float]) -> float:
        a1 = self.l1.forward(x)
        a2 = self.l2.forward(a1)
        return a2[0]  # scalar output

    def backward(self, y_pred: float, y_true: float) -> float:
        """
        MSE loss:  L = (y_pred - y_true)^2
        dL/dy_pred = 2 * (y_pred - y_true)
        """
        loss = (y_pred - y_true) ** 2
        d_out2 = [2.0 * (y_pred - y_true)]   # gradient into l2 output
        d_out1 = self.l2.backward(d_out2)
        self.l1.backward(d_out1)
        return loss

    def step(self, lr: float, momentum: float) -> None:
        self.l1.step(lr, momentum)
        self.l2.step(lr, momentum)


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

def train() -> None:
    random.seed(42)

    # XOR dataset
    X: list[list[float]] = [[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]]
    Y: list[float]       = [0.0,          1.0,          1.0,          0.0]

    model    = MLP(hidden=4)
    lr       = 0.1
    momentum = 0.9
    epochs   = 10_000
    log_every = 1_000

    for epoch in range(1, epochs + 1):
        total_loss = 0.0

        # Full-batch pass (four samples)
        for x, y in zip(X, Y):
            y_pred      = model.forward(x)
            loss        = model.backward(y_pred, y)
            total_loss += loss
            model.step(lr, momentum)

        if epoch % log_every == 0:
            avg_loss = total_loss / len(X)
            print(f"epoch {epoch:>6}  avg_mse={avg_loss:.6f}")

    # Final predictions
    print("\n--- XOR predictions after training ---")
    print(f"{'input':<15} {'target':>8} {'predicted':>12}")
    print("-" * 37)
    for x, y in zip(X, Y):
        pred = model.forward(x)
        xi = [int(v) for v in x]
        print(f"{str(xi):<15} {int(y):>8} {pred:>12.3f}")


if __name__ == "__main__":
    train()
