"""
mlp.py — tiny neural network from scratch.

Python 3.12, stdlib only (math + random).
- Dense layers with He initialization
- ReLU and sigmoid activations
- MSE loss
- Manual backprop (forward + backward, gradients computed by hand)
- SGD with momentum
- Trains on XOR until the mean loss falls below a tolerance
- Prints predictions rounded to 3 decimals

Run: python3.12 mlp.py
"""

import math
import random


# ---------------------------------------------------------------------------
# Activations and their derivatives
# ---------------------------------------------------------------------------

def relu(x: float) -> float:
    return x if x > 0.0 else 0.0


def relu_deriv(x: float) -> float:
    # gradient is 1 for the positive half, 0 otherwise; treat 0 as inactive
    return 1.0 if x > 0.0 else 0.0


def sigmoid(x: float) -> float:
    # numerically stable so it never overflows for large |x|
    if x >= 0.0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


def sigmoid_deriv_from_output(y: float) -> float:
    # derivative of sigmoid expressed in terms of its own output y = sigmoid(x)
    return y * (1.0 - y)


# ---------------------------------------------------------------------------
# Dense layer
# ---------------------------------------------------------------------------

class Dense:
    """Fully connected layer.

    Weights w[i][j] connect input i to output j (fan_in x fan_out matrix).
    Biases b[j] are per output. Weights use He initialization, which scales
    Gaussian noise by sqrt(2 / fan_in) so ReLU layers keep activation
    variance roughly constant across depth.
    """

    def __init__(self, fan_in: int, fan_out: int, activation: str,
                 rng: random.Random):
        self.fan_in = fan_in
        self.fan_out = fan_out
        self.activation = activation  # "relu" or "sigmoid"

        scale = math.sqrt(2.0 / fan_in)
        self.w = [[rng.gauss(0.0, 1.0) * scale for _ in range(fan_out)]
                  for _ in range(fan_in)]
        self.b = [0.0 for _ in range(fan_out)]

        # momentum buffers
        self.vw = [[0.0 for _ in range(fan_out)] for _ in range(fan_in)]
        self.vb = [0.0 for _ in range(fan_out)]

        # forward cache (filled during forward(), read during backward())
        self.last_input = None
        self.last_z = None  # pre-activation vector
        self.last_a = None   # post-activation vector

        # gradient placeholders (filled during backward())
        self.grad_w = None
        self.grad_b = None

    def forward(self, x):
        """x: list of length fan_in -> output list of length fan_out."""
        self.last_input = x
        z = [
            self.b[j] + sum(x[i] * self.w[i][j] for i in range(self.fan_in))
            for j in range(self.fan_out)
        ]
        self.last_z = z
        if self.activation == "relu":
            a = [relu(v) for v in z]
        elif self.activation == "sigmoid":
            a = [sigmoid(v) for v in z]
        else:
            raise ValueError(f"unknown activation: {self.activation}")
        self.last_a = a
        return a

    def backward(self, grad_a):
        """grad_a: gradient of loss w.r.t. this layer's output.

        Returns grad_input: gradient w.r.t. this layer's input, to pass
        to the preceding layer. Also stores weight grads in self.grad_w
        and bias grads in self.grad_b for the optimizer step.
        """
        if self.activation == "relu":
            grad_z = [grad_a[j] * relu_deriv(self.last_z[j])
                      for j in range(self.fan_out)]
        else:  # sigmoid
            grad_z = [grad_a[j] * sigmoid_deriv_from_output(self.last_a[j])
                      for j in range(self.fan_out)]

        x = self.last_input
        # grad_w[i][j] = x[i] * grad_z[j]
        self.grad_w = [[x[i] * grad_z[j] for j in range(self.fan_out)]
                       for i in range(self.fan_in)]
        self.grad_b = list(grad_z)

        # grad_input[i] = sum_j w[i][j] * grad_z[j]
        grad_input = [
            sum(self.w[i][j] * grad_z[j] for j in range(self.fan_out))
            for i in range(self.fan_in)
        ]
        return grad_input

    def step(self, lr: float, momentum: float):
        """SGD with momentum: v <- momentum*v - lr*grad ; param <- param + v."""
        for i in range(self.fan_in):
            row_v = self.vw[i]
            row_g = self.grad_w[i]
            row_w = self.w[i]
            for j in range(self.fan_out):
                row_v[j] = momentum * row_v[j] - lr * row_g[j]
                row_w[j] += row_v[j]
        for j in range(self.fan_out):
            self.vb[j] = momentum * self.vb[j] - lr * self.grad_b[j]
            self.b[j] += self.vb[j]


# ---------------------------------------------------------------------------
# Network
# ---------------------------------------------------------------------------

class MLP:
    """A small stack of Dense layers trained with MSE + SGD-momentum."""

    def __init__(self, layers, seed: int = 0):
        """layers: list of (fan_in, fan_out, activation) tuples."""
        self.rng = random.Random(seed)
        self.layers = [
            Dense(fan_in, fan_out, act, self.rng)
            for (fan_in, fan_out, act) in layers
        ]

    def forward(self, x):
        for layer in self.layers:
            x = layer.forward(x)
        return x

    def backward(self, grad_out):
        grad = grad_out
        for layer in reversed(self.layers):
            grad = layer.backward(grad)

    def step(self, lr: float, momentum: float):
        for layer in self.layers:
            layer.step(lr, momentum)

    def loss_and_grad(self, x, target):
        """MSE for a single sample. Returns (loss, grad_wrt_output, output)."""
        out = self.forward(x)
        n = len(out)
        diff = [out[k] - target[k] for k in range(n)]
        loss = sum(d * d for d in diff) / n
        # dL/dout[k] = (2/n) * (out[k] - target[k])
        grad = [2.0 * diff[k] / n for k in range(n)]
        return loss, grad, out

    def train(self, X, Y, lr=0.5, momentum=0.9, tol=1e-4,
              max_epochs=200000, seed=42, log_every=2000):
        rng = random.Random(seed)
        epoch = 0
        final_loss = float("inf")
        while epoch < max_epochs:
            order = list(range(len(X)))
            rng.shuffle(order)
            total_loss = 0.0
            for idx in order:
                loss, grad, _ = self.loss_and_grad(X[idx], Y[idx])
                total_loss += loss
                self.backward(grad)
                self.step(lr, momentum)
            total_loss /= len(X)
            final_loss = total_loss
            if epoch % log_every == 0 or total_loss < tol:
                print(f"epoch {epoch:6d}  loss {total_loss:.6f}")
            if total_loss < tol:
                print(f"converged at epoch {epoch} with loss {total_loss:.6f}")
                return epoch
            epoch += 1
        print(f"stopped at max_epochs {max_epochs}, final loss {final_loss:.6f}")
        return epoch


# ---------------------------------------------------------------------------
# XOR
# ---------------------------------------------------------------------------

def main():
    # XOR truth table, one target value per sample
    X = [[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]]
    Y = [[0.0], [1.0], [1.0], [0.0]]

    # 2 -> 8 (ReLU) -> 1 (sigmoid). ReLU hidden gives the nonlinear capacity
    # XOR needs; sigmoid output keeps predictions in (0, 1) to match targets.
    net = MLP([(2, 8, "relu"), (8, 1, "sigmoid")], seed=1)
    net.train(X, Y, lr=0.5, momentum=0.9, tol=1e-4,
              max_epochs=200000, seed=7, log_every=2000)

    print("\nXOR predictions (3 decimals):")
    for x, y in zip(X, Y):
        pred = net.forward(x)
        print(f"  input {x}  target {y[0]:.0f}  pred {pred[0]:.3f}")


if __name__ == "__main__":
    main()
