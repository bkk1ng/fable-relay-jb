"""
Tiny MLP from scratch — stdlib only, Python 3.12
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
# Dense layer
# ---------------------------------------------------------------------------

class Dense:
    """
    Fully-connected layer.

    Parameters
    ----------
    n_in  : number of input features
    n_out : number of neurons
    activation : 'relu' | 'sigmoid'
    """

    def __init__(self, n_in: int, n_out: int, activation: str) -> None:
        self.n_in  = n_in
        self.n_out = n_out
        self.activation = activation

        # He initialisation — works well for ReLU; acceptable for sigmoid here
        std = math.sqrt(2.0 / n_in)
        self.W: list[list[float]] = [
            [random.gauss(0.0, std) for _ in range(n_in)]
            for _ in range(n_out)
        ]
        self.b: list[float] = [0.0] * n_out

        # Velocity accumulators for SGD + momentum
        self.vW: list[list[float]] = [[0.0] * n_in  for _ in range(n_out)]
        self.vb: list[float]       = [0.0] * n_out

        # Cache for backprop
        self._input:   list[float] = []   # a_{l-1}
        self._pre_act: list[float] = []   # z_l  (before activation)

    # ------------------------------------------------------------------
    # Forward
    # ------------------------------------------------------------------

    def forward(self, x: list[float]) -> list[float]:
        self._input = x
        z: list[float] = []
        for i in range(self.n_out):
            z.append(dot(self.W[i], x) + self.b[i])
        self._pre_act = z

        if self.activation == "relu":
            return [relu(v) for v in z]
        else:  # sigmoid
            return [sigmoid(v) for v in z]

    # ------------------------------------------------------------------
    # Backward
    # ------------------------------------------------------------------

    def backward(self, grad_out: list[float]) -> list[float]:
        """
        grad_out : dL/da_l  (gradient w.r.t. this layer's output)
        Returns  : dL/da_{l-1}  (gradient to pass to the previous layer)
        Also stores dL/dW and dL/db for the update step.
        """
        # dL/dz = dL/da * da/dz
        if self.activation == "relu":
            grad_z = [g * relu_prime(z)    for g, z in zip(grad_out, self._pre_act)]
        else:
            grad_z = [g * sigmoid_prime(z) for g, z in zip(grad_out, self._pre_act)]

        # dL/dW[i][j] = grad_z[i] * input[j]
        self._dW = [
            [grad_z[i] * self._input[j] for j in range(self.n_in)]
            for i in range(self.n_out)
        ]
        # dL/db[i] = grad_z[i]
        self._db = grad_z[:]

        # dL/da_{l-1}[j] = sum_i  W[i][j] * grad_z[i]
        grad_in = [
            sum(self.W[i][j] * grad_z[i] for i in range(self.n_out))
            for j in range(self.n_in)
        ]
        return grad_in

    # ------------------------------------------------------------------
    # Update
    # ------------------------------------------------------------------

    def update(self, lr: float, momentum: float) -> None:
        for i in range(self.n_out):
            for j in range(self.n_in):
                self.vW[i][j] = momentum * self.vW[i][j] - lr * self._dW[i][j]
                self.W[i][j] += self.vW[i][j]
            self.vb[i] = momentum * self.vb[i] - lr * self._db[i]
            self.b[i] += self.vb[i]


# ---------------------------------------------------------------------------
# MLP
# ---------------------------------------------------------------------------

class MLP:
    def __init__(self, lr: float = 0.1, momentum: float = 0.9) -> None:
        self.lr       = lr
        self.momentum = momentum
        self.layers: list[Dense] = [
            Dense(2, 4, activation="relu"),
            Dense(4, 1, activation="sigmoid"),
        ]

    # ------------------------------------------------------------------
    # Forward pass
    # ------------------------------------------------------------------

    def predict(self, x: list[float]) -> float:
        a = x
        for layer in self.layers:
            a = layer.forward(a)
        return a[0]

    # ------------------------------------------------------------------
    # Train on a single sample
    # ------------------------------------------------------------------

    def train_step(self, x: list[float], y: float) -> float:
        # Forward
        y_hat = self.predict(x)

        # MSE loss  L = (y_hat - y)^2
        loss = (y_hat - y) ** 2

        # dL/d_y_hat = 2 * (y_hat - y)
        grad = [2.0 * (y_hat - y)]

        # Backward through layers in reverse
        for layer in reversed(self.layers):
            grad = layer.backward(grad)

        # Update weights
        for layer in self.layers:
            layer.update(self.lr, self.momentum)

        return loss

    # ------------------------------------------------------------------
    # Training loop
    # ------------------------------------------------------------------

    def fit(
        self,
        X: list[list[float]],
        Y: list[float],
        epochs: int = 10_000,
        log_every: int = 1_000,
    ) -> None:
        n = len(X)
        for epoch in range(1, epochs + 1):
            total_loss = 0.0
            # Shuffle indices each epoch
            indices = list(range(n))
            random.shuffle(indices)
            for i in indices:
                total_loss += self.train_step(X[i], Y[i])
            if epoch % log_every == 0:
                avg = total_loss / n
                print(f"epoch {epoch:>6}  avg_loss={avg:.6f}")


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------

def main() -> None:
    random.seed(42)

    # XOR dataset
    X = [[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]]
    Y = [0.0,         1.0,         1.0,         0.0]

    model = MLP(lr=0.1, momentum=0.9)
    model.fit(X, Y, epochs=10_000, log_every=1_000)

    print("\nPredictions after training:")
    for x, y in zip(X, Y):
        pred = model.predict(x)
        print(f"  input={x}  target={y:.1f}  predicted={pred:.3f}")


if __name__ == "__main__":
    main()
