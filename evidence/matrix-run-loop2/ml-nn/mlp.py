"""
Tiny MLP from scratch — Python 3.12, stdlib only.

Architecture:
  Input(2) -> Dense(4, ReLU) -> Dense(1, Sigmoid)

Training:
  MSE loss, manual backprop, SGD with momentum.
  Trains on XOR until convergence.
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


def relu_prime(x: float) -> float:
    return 1.0 if x > 0.0 else 0.0


def sigmoid(x: float) -> float:
    # Clamp to avoid overflow in exp.
    x = max(-500.0, min(500.0, x))
    return 1.0 / (1.0 + math.exp(-x))


def sigmoid_prime(x: float) -> float:
    s = sigmoid(x)
    return s * (1.0 - s)


# ---------------------------------------------------------------------------
# He initialisation
# ---------------------------------------------------------------------------

def he_weights(fan_in: int, fan_out: int) -> list[list[float]]:
    """Return fan_out × fan_in weight matrix with He init."""
    std = math.sqrt(2.0 / fan_in)
    return [
        [random.gauss(0.0, std) for _ in range(fan_in)]
        for _ in range(fan_out)
    ]


def zero_biases(size: int) -> list[float]:
    return [0.0] * size


# ---------------------------------------------------------------------------
# Dense layer (holds parameters and their momentum buffers)
# ---------------------------------------------------------------------------

class Dense:
    def __init__(self, fan_in: int, fan_out: int, activation: str) -> None:
        assert activation in ("relu", "sigmoid")
        self.fan_in = fan_in
        self.fan_out = fan_out
        self.activation = activation

        # Parameters
        self.W: list[list[float]] = he_weights(fan_in, fan_out)
        self.b: list[float] = zero_biases(fan_out)

        # SGD momentum buffers (velocity)
        self.vW: list[list[float]] = [[0.0] * fan_in for _ in range(fan_out)]
        self.vb: list[float] = [0.0] * fan_out

        # Cache for backprop
        self._input: list[float] = []
        self._pre: list[float] = []   # pre-activation z
        self._out: list[float] = []   # post-activation a

    # ------------------------------------------------------------------
    # Forward
    # ------------------------------------------------------------------

    def forward(self, x: list[float]) -> list[float]:
        self._input = x[:]
        act_fn = relu if self.activation == "relu" else sigmoid
        pre = []
        out = []
        for i in range(self.fan_out):
            z = dot(self.W[i], x) + self.b[i]
            pre.append(z)
            out.append(act_fn(z))
        self._pre = pre
        self._out = out
        return out

    # ------------------------------------------------------------------
    # Backward — receives dL/da for this layer's output,
    #             returns dL/da for the previous layer's output.
    # ------------------------------------------------------------------

    def backward(self, d_out: list[float]) -> list[float]:
        act_prime = relu_prime if self.activation == "relu" else sigmoid_prime

        # dL/dz  =  dL/da * da/dz
        d_pre = [d_out[i] * act_prime(self._pre[i]) for i in range(self.fan_out)]

        # dL/dW[i][j]  =  dL/dz[i] * x[j]
        self._dW = [
            [d_pre[i] * self._input[j] for j in range(self.fan_in)]
            for i in range(self.fan_out)
        ]

        # dL/db[i]  =  dL/dz[i]
        self._db = d_pre[:]

        # dL/dx[j]  =  sum_i  dL/dz[i] * W[i][j]
        d_input = [
            sum(d_pre[i] * self.W[i][j] for i in range(self.fan_out))
            for j in range(self.fan_in)
        ]
        return d_input

    # ------------------------------------------------------------------
    # SGD step with momentum:  v = mu*v - lr*grad;  param += v
    # ------------------------------------------------------------------

    def step(self, lr: float, momentum: float) -> None:
        for i in range(self.fan_out):
            for j in range(self.fan_in):
                self.vW[i][j] = momentum * self.vW[i][j] - lr * self._dW[i][j]
                self.W[i][j] += self.vW[i][j]
            self.vb[i] = momentum * self.vb[i] - lr * self._db[i]
            self.b[i] += self.vb[i]


# ---------------------------------------------------------------------------
# MLP
# ---------------------------------------------------------------------------

class MLP:
    def __init__(self) -> None:
        self.layers: list[Dense] = [
            Dense(2, 4, "relu"),
            Dense(4, 1, "sigmoid"),
        ]

    def forward(self, x: list[float]) -> list[float]:
        a = x
        for layer in self.layers:
            a = layer.forward(a)
        return a

    def backward(self, d_loss: list[float]) -> None:
        grad = d_loss
        for layer in reversed(self.layers):
            grad = layer.backward(grad)

    def step(self, lr: float, momentum: float) -> None:
        for layer in self.layers:
            layer.step(lr, momentum)


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

def mse(pred: list[float], target: list[float]) -> float:
    return sum((p - t) ** 2 for p, t in zip(pred, target)) / len(pred)


def mse_grad(pred: list[float], target: list[float]) -> list[float]:
    n = len(pred)
    return [2.0 * (p - t) / n for p, t in zip(pred, target)]


XOR_DATA: list[tuple[list[float], list[float]]] = [
    ([0.0, 0.0], [0.0]),
    ([0.0, 1.0], [1.0]),
    ([1.0, 0.0], [1.0]),
    ([1.0, 1.0], [0.0]),
]


def train(
    net: MLP,
    *,
    lr: float = 0.05,
    momentum: float = 0.9,
    max_epochs: int = 20_000,
    tol: float = 1e-4,
    seed: int = 42,
) -> int:
    random.seed(seed)

    for epoch in range(1, max_epochs + 1):
        total_loss = 0.0
        indices = list(range(len(XOR_DATA)))
        random.shuffle(indices)

        for idx in indices:
            x, y = XOR_DATA[idx]
            pred = net.forward(x)
            total_loss += mse(pred, y)
            grad = mse_grad(pred, y)
            net.backward(grad)
            net.step(lr, momentum)

        avg_loss = total_loss / len(XOR_DATA)
        if avg_loss < tol:
            return epoch

    return max_epochs


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    net = MLP()
    epochs = train(net)

    print(f"Converged in {epochs} epoch(s).\n")
    print(f"{'Input':>12}  {'Target':>8}  {'Prediction':>12}")
    print("-" * 36)
    for x, y in XOR_DATA:
        pred = net.forward(x)
        label = f"[{int(x[0])}, {int(x[1])}]"
        print(f"{label:>12}  {y[0]:>8.3f}  {pred[0]:>12.3f}")


if __name__ == "__main__":
    main()
