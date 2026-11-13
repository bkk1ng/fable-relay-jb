"""
Tiny MLP from scratch — Python 3.12, stdlib only.

Architecture : Dense(2→4, ReLU) → Dense(4→1, Sigmoid)
Loss         : MSE
Optimizer    : SGD with momentum
Task         : XOR
"""

import math
import random


# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

def dot(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# Activations
# ---------------------------------------------------------------------------

def relu(x: float) -> float:
    return max(0.0, x)

def relu_prime(x: float) -> float:
    return 1.0 if x > 0.0 else 0.0

def sigmoid(x: float) -> float:
    # Clamp to avoid overflow in exp
    x = max(-500.0, min(500.0, x))
    return 1.0 / (1.0 + math.exp(-x))

def sigmoid_prime(x: float) -> float:
    s = sigmoid(x)
    return s * (1.0 - s)


# ---------------------------------------------------------------------------
# He initialisation
# ---------------------------------------------------------------------------

def he_weights(fan_in: int, fan_out: int) -> list[list[float]]:
    """Return a (fan_out × fan_in) weight matrix initialised with He normal."""
    std = math.sqrt(2.0 / fan_in)
    return [
        [random.gauss(0.0, std) for _ in range(fan_in)]
        for _ in range(fan_out)
    ]

def zero_biases(size: int) -> list[float]:
    return [0.0] * size


# ---------------------------------------------------------------------------
# Dense layer
# ---------------------------------------------------------------------------

class Dense:
    def __init__(self, in_features: int, out_features: int, activation: str):
        assert activation in ("relu", "sigmoid")
        self.activation = activation
        self.W = he_weights(in_features, out_features)   # (out × in)
        self.b = zero_biases(out_features)

        # Momentum buffers (same shape as W and b)
        self.vW = [[0.0] * in_features for _ in range(out_features)]
        self.vb = [0.0] * out_features

        # Cache for backprop
        self._x: list[float] = []   # layer input
        self._z: list[float] = []   # pre-activation

    # -- forward --

    def forward(self, x: list[float]) -> list[float]:
        self._x = x
        z = [dot(self.W[i], x) + self.b[i] for i in range(len(self.b))]
        self._z = z
        act_fn = relu if self.activation == "relu" else sigmoid
        return [act_fn(v) for v in z]

    # -- backward  (returns dL/dx for the previous layer) --

    def backward(self, grad_out: list[float]) -> list[float]:
        """
        grad_out : dL/d(activation output), shape (out_features,)
        Returns  : dL/d(layer input),        shape (in_features,)
        Also stores dL/dW and dL/db in self.dW / self.db for the update step.
        """
        act_prime = relu_prime if self.activation == "relu" else sigmoid_prime

        # dL/dz  =  dL/da * da/dz
        delta = [grad_out[i] * act_prime(self._z[i]) for i in range(len(self._z))]

        # Gradient w.r.t. weights and biases
        self.dW = [
            [delta[i] * self._x[j] for j in range(len(self._x))]
            for i in range(len(delta))
        ]
        self.db = list(delta)

        # Gradient w.r.t. input (passed to previous layer)
        in_features = len(self._x)
        grad_in = [
            sum(self.W[i][j] * delta[i] for i in range(len(delta)))
            for j in range(in_features)
        ]
        return grad_in

    # -- SGD update with momentum --

    def update(self, lr: float, momentum: float) -> None:
        for i in range(len(self.b)):
            for j in range(len(self.W[i])):
                self.vW[i][j] = momentum * self.vW[i][j] - lr * self.dW[i][j]
                self.W[i][j] += self.vW[i][j]
            self.vb[i] = momentum * self.vb[i] - lr * self.db[i]
            self.b[i] += self.vb[i]


# ---------------------------------------------------------------------------
# Network
# ---------------------------------------------------------------------------

class MLP:
    def __init__(self):
        self.layers = [
            Dense(2, 4, "relu"),
            Dense(4, 1, "sigmoid"),
        ]

    def forward(self, x: list[float]) -> list[float]:
        out = x
        for layer in self.layers:
            out = layer.forward(out)
        return out

    def backward(self, grad: list[float]) -> None:
        for layer in reversed(self.layers):
            grad = layer.backward(grad)

    def update(self, lr: float, momentum: float) -> None:
        for layer in self.layers:
            layer.update(lr, momentum)


# ---------------------------------------------------------------------------
# Loss  (MSE)
# ---------------------------------------------------------------------------

def mse(pred: list[float], target: list[float]) -> float:
    return sum((p - t) ** 2 for p, t in zip(pred, target)) / len(pred)

def mse_grad(pred: list[float], target: list[float]) -> list[float]:
    n = len(pred)
    return [2.0 * (p - t) / n for p, t in zip(pred, target)]


# ---------------------------------------------------------------------------
# XOR dataset
# ---------------------------------------------------------------------------

XOR_X = [[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]]
XOR_Y = [[0.0],       [1.0],       [1.0],       [0.0]]


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

def train(
    net: MLP,
    epochs: int = 10_000,
    lr: float = 0.05,
    momentum: float = 0.9,
    log_every: int = 1_000,
) -> None:
    for epoch in range(1, epochs + 1):
        total_loss = 0.0

        # Full-batch gradient descent — iterate all samples each epoch
        for x, y in zip(XOR_X, XOR_Y):
            pred = net.forward(x)
            total_loss += mse(pred, y)
            grad = mse_grad(pred, y)
            net.backward(grad)
            net.update(lr, momentum)

        if epoch % log_every == 0:
            avg = total_loss / len(XOR_X)
            print(f"epoch {epoch:>6}  loss {avg:.6f}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    random.seed(42)

    net = MLP()
    print("Training on XOR …\n")
    train(net, epochs=10_000, lr=0.05, momentum=0.9, log_every=1_000)

    print("\nPredictions after training:")
    for x, y in zip(XOR_X, XOR_Y):
        pred = net.forward(x)
        print(
            f"  input {[int(v) for v in x]}  "
            f"target {y[0]:.1f}  "
            f"pred {pred[0]:.3f}"
        )


if __name__ == "__main__":
    main()
