"""k-means clustering — Python 3.12, standard library only.

Features:
  * k-means++ initialisation (D^2 sampling)
  * Lloyd iterations with convergence tolerance on centroid shift
  * empty-cluster repair (steal the worst-fit point from a multi-member cluster)
  * per-iteration inertia history and best-of-n_init restarts
  * elbow curve rendered as an ASCII bar chart, elbow picked by max distance to chord
  * fully deterministic given a seed

Run:  python3 kmeans.py
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from typing import Sequence

Point = tuple[float, ...]


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def sq_dist(a: Sequence[float], b: Sequence[float]) -> float:
    return sum((x - y) * (x - y) for x, y in zip(a, b))


def mean(points: Sequence[Point], dim: int) -> Point:
    n = len(points)
    sums = [0.0] * dim
    for p in points:
        for d in range(dim):
            sums[d] += p[d]
    return tuple(s / n for s in sums)


def assign(points: Sequence[Point], centroids: Sequence[Point]) -> tuple[list[int], list[float]]:
    """Nearest-centroid label and squared distance for every point."""
    labels: list[int] = []
    dists: list[float] = []
    for p in points:
        best_j, best_d = 0, math.inf
        for j, c in enumerate(centroids):
            d = sq_dist(p, c)
            if d < best_d:
                best_j, best_d = j, d
        labels.append(best_j)
        dists.append(best_d)
    return labels, dists


# ---------------------------------------------------------------------------
# Initialisation
# ---------------------------------------------------------------------------

def kmeans_plus_plus(points: Sequence[Point], k: int, rng: random.Random) -> list[Point]:
    """Pick k initial centroids with probability proportional to D(x)^2."""
    n = len(points)
    centroids: list[Point] = [points[rng.randrange(n)]]
    closest = [sq_dist(p, centroids[0]) for p in points]

    while len(centroids) < k:
        total = sum(closest)
        if total == 0.0:
            # All remaining points coincide with existing centroids; pick uniformly.
            idx = rng.randrange(n)
        else:
            r = rng.random() * total
            acc = 0.0
            idx = n - 1
            for i, d in enumerate(closest):
                acc += d
                if acc >= r:
                    idx = i
                    break
        c = points[idx]
        centroids.append(c)
        for i, p in enumerate(points):
            d = sq_dist(p, c)
            if d < closest[i]:
                closest[i] = d
    return centroids


# ---------------------------------------------------------------------------
# Lloyd's algorithm
# ---------------------------------------------------------------------------

@dataclass
class KMeansResult:
    centroids: list[Point]
    labels: list[int]
    inertia: float
    n_iter: int
    converged: bool
    history: list[float] = field(default_factory=list)  # inertia after each assignment step
    empty_repairs: int = 0


def _repair_empty(
    points: Sequence[Point],
    labels: list[int],
    dists: list[float],
    centroids: list[Point],
    k: int,
) -> int:
    """Reseed each empty cluster with the point currently farthest from its centroid,
    taken only from clusters that would stay non-empty. Mutates labels/dists/centroids."""
    counts = [0] * k
    for lab in labels:
        counts[lab] += 1

    repairs = 0
    for j in range(k):
        if counts[j] > 0:
            continue
        donor, donor_d = -1, -1.0
        for i, d in enumerate(dists):
            if counts[labels[i]] > 1 and d > donor_d:
                donor, donor_d = i, d
        if donor < 0:  # fewer distinct points than k; nothing to steal
            break
        counts[labels[donor]] -= 1
        counts[j] = 1
        labels[donor] = j
        dists[donor] = 0.0
        centroids[j] = points[donor]
        repairs += 1
    return repairs


def _lloyd(
    points: Sequence[Point],
    centroids: list[Point],
    max_iter: int,
    tol: float,
) -> KMeansResult:
    k = len(centroids)
    dim = len(points[0])
    history: list[float] = []
    repairs = 0
    converged = False
    it = 0

    for it in range(1, max_iter + 1):
        labels, dists = assign(points, centroids)
        repairs += _repair_empty(points, labels, dists, centroids, k)
        history.append(sum(dists))

        members: list[list[Point]] = [[] for _ in range(k)]
        for p, lab in zip(points, labels):
            members[lab].append(p)
        new_centroids = [mean(m, dim) if m else centroids[j] for j, m in enumerate(members)]

        shift = max(sq_dist(a, b) for a, b in zip(centroids, new_centroids))
        centroids = new_centroids
        if shift <= tol:
            converged = True
            break

    labels, dists = assign(points, centroids)
    repairs += _repair_empty(points, labels, dists, centroids, k)
    inertia = sum(dists)
    if not history or inertia < history[-1]:
        history.append(inertia)

    return KMeansResult(
        centroids=centroids,
        labels=labels,
        inertia=inertia,
        n_iter=it,
        converged=converged,
        history=history,
        empty_repairs=repairs,
    )


def kmeans(
    points: Sequence[Sequence[float]],
    k: int,
    *,
    seed: int = 0,
    n_init: int = 5,
    max_iter: int = 300,
    tol: float = 1e-10,
) -> KMeansResult:
    """Best-of-n_init k-means. Deterministic for a given seed."""
    pts: list[Point] = [tuple(float(x) for x in p) for p in points]
    if not pts:
        raise ValueError("no points given")
    if not 1 <= k <= len(pts):
        raise ValueError(f"k must be in [1, {len(pts)}], got {k}")
    dim = len(pts[0])
    if any(len(p) != dim for p in pts):
        raise ValueError("all points must have the same dimension")

    rng = random.Random(seed)
    best: KMeansResult | None = None
    for _ in range(max(1, n_init)):
        init = kmeans_plus_plus(pts, k, rng)
        result = _lloyd(pts, list(init), max_iter, tol)
        if best is None or result.inertia < best.inertia:
            best = result
    assert best is not None
    return best


# ---------------------------------------------------------------------------
# Elbow plot
# ---------------------------------------------------------------------------

def elbow_curve(
    points: Sequence[Sequence[float]], ks: Sequence[int], *, seed: int = 0, n_init: int = 5
) -> list[float]:
    return [kmeans(points, k, seed=seed, n_init=n_init).inertia for k in ks]


def find_elbow(ks: Sequence[int], inertias: Sequence[float]) -> int:
    """k whose normalised (k, inertia) point lies farthest from the first-to-last chord."""
    if len(ks) < 3:
        return ks[0]
    k0, k1 = ks[0], ks[-1]
    hi, lo = max(inertias), min(inertias)
    span_y = (hi - lo) or 1.0
    xs = [(k - k0) / (k1 - k0) for k in ks]
    ys = [(v - lo) / span_y for v in inertias]
    x0, y0, x1, y1 = xs[0], ys[0], xs[-1], ys[-1]
    dx, dy = x1 - x0, y1 - y0
    norm = math.hypot(dx, dy) or 1.0
    best_i = max(range(len(ks)), key=lambda i: abs(dy * xs[i] - dx * ys[i] + x1 * y0 - y1 * x0) / norm)
    return ks[best_i]


def ascii_elbow(ks: Sequence[int], inertias: Sequence[float], width: int = 50) -> str:
    elbow = find_elbow(ks, inertias)
    top = max(inertias) or 1.0
    label_w = len(f"{max(inertias):.1f}")
    lines = ["Elbow plot (inertia vs k)", "-" * (width + label_w + 16)]
    for k, v in zip(ks, inertias):
        bar = "#" * max(1 if v > 0 else 0, round(v / top * width))
        mark = "  <- elbow" if k == elbow else ""
        lines.append(f"k={k:>2} | {bar:<{width}} {v:>{label_w}.1f}{mark}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Demo + self-check
# ---------------------------------------------------------------------------

def make_blobs(
    centers: Sequence[Point], n_per: int, std: float, seed: int
) -> tuple[list[Point], list[int]]:
    rng = random.Random(seed)
    pts: list[Point] = []
    truth: list[int] = []
    for label, (cx, cy) in enumerate(centers):
        for _ in range(n_per):
            pts.append((rng.gauss(cx, std), rng.gauss(cy, std)))
            truth.append(label)
    # Shuffle deterministically so cluster order isn't baked into point order.
    order = list(range(len(pts)))
    rng.shuffle(order)
    return [pts[i] for i in order], [truth[i] for i in order]


def labels_match(pred: Sequence[int], truth: Sequence[int]) -> bool:
    """True iff pred equals truth up to a relabelling (a bijection between label sets)."""
    fwd: dict[int, int] = {}
    back: dict[int, int] = {}
    for p, t in zip(pred, truth):
        if fwd.setdefault(t, p) != p or back.setdefault(p, t) != t:
            return False
    return True


def main() -> None:
    centers = [(0.0, 0.0), (10.0, 0.0), (5.0, 8.66)]
    points, truth = make_blobs(centers, n_per=60, std=0.8, seed=42)

    result = kmeans(points, 3, seed=7)
    print(f"k=3  inertia={result.inertia:.3f}  iterations={result.n_iter}  "
          f"converged={result.converged}  empty_repairs={result.empty_repairs}")
    for j, c in enumerate(sorted(result.centroids)):
        print(f"  centroid {j}: ({c[0]:7.3f}, {c[1]:7.3f})")
    print()

    ks = list(range(1, 9))
    inertias = elbow_curve(points, ks, seed=7)
    print(ascii_elbow(ks, inertias))
    print()

    # --- self-checks -------------------------------------------------------
    assert labels_match(result.labels, truth), "labelling does not match ground truth"

    for a, b in zip(result.history, result.history[1:]):
        assert b <= a + 1e-9, f"inertia increased during Lloyd: {a} -> {b}"

    again = kmeans(points, 3, seed=7)
    assert again.labels == result.labels and again.centroids == result.centroids, \
        "same seed produced different results"

    for c in result.centroids:
        nearest = min(sq_dist(c, t) for t in centers)
        assert nearest < 0.5, f"centroid {c} far from any true centre"

    assert find_elbow(ks, inertias) == 3, "elbow heuristic did not pick k=3"

    # Empty-cluster stress: many duplicates force repairs; must still yield k non-empty clusters.
    dupes = [(0.0, 0.0)] * 20 + [(1.0, 1.0)] * 20 + [(5.0, 5.0), (6.0, 5.0), (5.0, 6.0)]
    stress = kmeans(dupes, 5, seed=1, n_init=3)
    assert len(set(stress.labels)) == 5, "empty cluster survived"

    print("self-check passed: labels match ground truth up to permutation, "
          "inertia monotone, deterministic, elbow at k=3, no empty clusters")


if __name__ == "__main__":
    main()
