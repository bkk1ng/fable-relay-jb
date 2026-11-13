"""
kmeans.py – k-means clustering, stdlib only, Python 3.12.

Features
--------
* k-means++ initialisation
* Lloyd iterations with convergence detection
* Empty-cluster rescue: the centroid is reassigned to the point that
  currently has the largest squared error, so no centroid is ever wasted
* Inertia tracked per Lloyd step and per k for an elbow plot
* ASCII elbow plot printed to stdout
* Deterministic seeding via random.Random
* __main__ demo on three 2-D Gaussian blobs with a labelling self-check
"""

from __future__ import annotations

import math
import random
from typing import NamedTuple


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

Point = tuple[float, ...]   # any fixed dimension ≥ 1


class KMeansResult(NamedTuple):
    centroids: list[Point]
    labels: list[int]
    inertia: float
    inertia_history: list[float]   # one entry per completed Lloyd step


# ---------------------------------------------------------------------------
# Distance helpers
# ---------------------------------------------------------------------------

def _sq_dist(a: Point, b: Point) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b))


def _dist(a: Point, b: Point) -> float:
    return math.sqrt(_sq_dist(a, b))


# ---------------------------------------------------------------------------
# k-means++ initialisation
# ---------------------------------------------------------------------------

def _kmeanspp_init(points: list[Point], k: int, rng: random.Random) -> list[Point]:
    """
    Choose k initial centroids with k-means++ probability weighting.

    The first centroid is chosen uniformly at random.  Each subsequent
    centroid is sampled with probability proportional to the squared
    distance from a point to its nearest already-chosen centroid.
    """
    centroids: list[Point] = [rng.choice(points)]

    for _ in range(k - 1):
        d2 = [min(_sq_dist(p, c) for c in centroids) for p in points]
        total = sum(d2)
        if total == 0.0:
            # All remaining candidates coincide with existing centroids;
            # fall back to uniform sampling.
            centroids.append(rng.choice(points))
            continue

        threshold = rng.random() * total
        cumulative = 0.0
        chosen = points[-1]
        for p, w in zip(points, d2):
            cumulative += w
            if cumulative >= threshold:
                chosen = p
                break
        centroids.append(chosen)

    return centroids


# ---------------------------------------------------------------------------
# Assignment step
# ---------------------------------------------------------------------------

def _assign(points: list[Point], centroids: list[Point]) -> tuple[list[int], float]:
    """
    Assign each point to its nearest centroid.

    Returns
    -------
    labels  : cluster index for each point
    inertia : total within-cluster sum of squared distances
    """
    labels: list[int] = []
    inertia = 0.0
    for p in points:
        best_idx = 0
        best_d2 = _sq_dist(p, centroids[0])
        for i, c in enumerate(centroids[1:], 1):
            d2 = _sq_dist(p, c)
            if d2 < best_d2:
                best_d2 = d2
                best_idx = i
        labels.append(best_idx)
        inertia += best_d2
    return labels, inertia


# ---------------------------------------------------------------------------
# Centroid update step
# ---------------------------------------------------------------------------

def _recompute_centroids(
    points: list[Point],
    labels: list[int],
    k: int,
    dim: int,
    old_centroids: list[Point],
    rng: random.Random,
) -> list[Point]:
    """
    Compute the mean of each cluster.

    Empty-cluster rescue
    --------------------
    When a cluster has no members its centroid is replaced by the point
    currently carrying the largest squared error (i.e. the point that
    most needs its own centroid).  If multiple empty clusters exist, each
    steals the next-highest-error point, so no two empty clusters end up
    at the same location.
    """
    sums = [[0.0] * dim for _ in range(k)]
    counts = [0] * k
    for p, lbl in zip(points, labels):
        counts[lbl] += 1
        for d, v in enumerate(p):
            sums[lbl][d] += v

    # Pre-compute per-point squared errors for the rescue heuristic.
    sq_errors: list[float] = [
        _sq_dist(p, old_centroids[lbl]) for p, lbl in zip(points, labels)
    ]

    new_centroids: list[Point] = []
    for i in range(k):
        if counts[i] > 0:
            new_centroids.append(tuple(s / counts[i] for s in sums[i]))
        else:
            # Rescue: take the highest-error point as the new centroid.
            rescue = max(range(len(points)), key=lambda j: sq_errors[j])
            new_centroids.append(points[rescue])
            # Zero out so the same point is not stolen twice.
            sq_errors[rescue] = -1.0

    return new_centroids


# ---------------------------------------------------------------------------
# k-means
# ---------------------------------------------------------------------------

def kmeans(
    points: list[Point],
    k: int,
    *,
    seed: int = 42,
    max_iter: int = 300,
    tol: float = 1e-9,
) -> KMeansResult:
    """
    Cluster *points* into *k* groups using Lloyd's algorithm.

    Parameters
    ----------
    points   : non-empty list of equal-length tuples (any dimension ≥ 1)
    k        : number of clusters, 1 ≤ k ≤ len(points)
    seed     : integer seed for the internal RNG – same seed always gives
               the same result regardless of the calling context
    max_iter : hard cap on Lloyd iterations
    tol      : stop early when the total centroid movement (Euclidean) in
               a step falls below this value

    Returns
    -------
    KMeansResult(centroids, labels, inertia, inertia_history)
    """
    if not points:
        raise ValueError("points must not be empty")
    if k < 1:
        raise ValueError("k must be at least 1")
    if k > len(points):
        raise ValueError("k must not exceed the number of points")

    dim = len(points[0])
    rng = random.Random(seed)

    centroids = _kmeanspp_init(points, k, rng)
    inertia_history: list[float] = []

    for _ in range(max_iter):
        labels, inertia = _assign(points, centroids)
        inertia_history.append(inertia)

        new_centroids = _recompute_centroids(points, labels, k, dim, centroids, rng)

        movement = sum(_dist(c, nc) for c, nc in zip(centroids, new_centroids))
        centroids = new_centroids

        if movement < tol:
            break

    # Final assignment against the converged centroids.
    labels, inertia = _assign(points, centroids)
    inertia_history.append(inertia)

    return KMeansResult(
        centroids=centroids,
        labels=labels,
        inertia=inertia,
        inertia_history=inertia_history,
    )


# ---------------------------------------------------------------------------
# Elbow analysis
# ---------------------------------------------------------------------------

def elbow_inertias(
    points: list[Point],
    k_range: range | list[int],
    *,
    seed: int = 42,
    max_iter: int = 300,
) -> dict[int, float]:
    """
    Run k-means for each k in *k_range* and return {k: final_inertia}.
    The same seed is used for every run so the sweep is reproducible.
    """
    return {
        k: kmeans(points, k, seed=seed, max_iter=max_iter).inertia
        for k in k_range
    }


# ---------------------------------------------------------------------------
# ASCII elbow plot
# ---------------------------------------------------------------------------

def _fmt_si(value: float) -> str:
    """Compact axis label with an SI suffix."""
    for threshold, suffix in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if abs(value) >= threshold:
            return f"{value / threshold:.1f}{suffix}"
    return f"{value:.1f}"


def print_elbow_plot(
    inertias: dict[int, float],
    *,
    width: int = 60,
    height: int = 20,
    title: str = "Elbow Plot — Inertia vs k",
) -> None:
    """
    Print a character-art line chart of inertia vs k to stdout.

    Parameters
    ----------
    inertias : {k: inertia} mapping as returned by elbow_inertias()
    width    : plot area width in characters
    height   : plot area height in character rows
    title    : chart title
    """
    if not inertias:
        print("(no data)")
        return

    ks = sorted(inertias)
    values = [inertias[k] for k in ks]
    n = len(ks)

    min_v, max_v = min(values), max(values)
    v_range = max_v - min_v if max_v != min_v else 1.0

    def x_col(i: int) -> int:
        return 0 if n == 1 else round(i * (width - 1) / (n - 1))

    def y_row(v: float) -> int:
        # row 0 is the top (highest inertia); row height-1 is the bottom.
        return round((max_v - v) / v_range * (height - 1))

    grid: list[list[str]] = [[" "] * width for _ in range(height)]

    for i, (k, v) in enumerate(zip(ks, values)):
        x = x_col(i)
        y = y_row(v)
        grid[y][x] = "●"

        if i < n - 1:
            x2 = x_col(i + 1)
            y2 = y_row(values[i + 1])
            steps = x2 - x
            for s in range(1, steps):
                xi = x + s
                t = s / steps
                yi = round(y + t * (y2 - y))
                if 0 <= yi < height and 0 <= xi < width and grid[yi][xi] == " ":
                    grid[yi][xi] = "·"

    label_w = 12
    print()
    print(f"{title:^{label_w + width + 2}}")
    print()

    for ri, row in enumerate(grid):
        if ri == 0 or ri == height - 1 or ri % 4 == 0:
            lbl = _fmt_si(max_v - (ri / (height - 1)) * v_range)
        else:
            lbl = ""
        print(f"{lbl:>{label_w}} │{''.join(row)}")

    # X-axis rule.
    print(" " * label_w + " └" + "─" * width)

    # k-value tick labels.
    tick_line = [" "] * width
    for i, k in enumerate(ks):
        x = x_col(i)
        s = str(k)
        half = len(s) // 2
        for j, ch in enumerate(s):
            xi = x - half + j
            if 0 <= xi < width:
                tick_line[xi] = ch
    print(" " * (label_w + 2) + "".join(tick_line))
    print(" " * (label_w + 2 + width // 2) + "k")
    print()


# ---------------------------------------------------------------------------
# __main__ demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    # ------------------------------------------------------------------
    # 1.  Synthetic dataset: three well-separated Gaussian blobs in 2-D.
    # ------------------------------------------------------------------
    DATA_SEED = 0
    rng_data = random.Random(DATA_SEED)

    BLOB_CENTRES = [(0.0, 0.0), (8.0, 0.0), (4.0, 7.0)]
    N_PER_BLOB = 40
    SIGMA = 0.8

    data: list[Point] = []
    true_labels: list[int] = []

    for cls, (cx, cy) in enumerate(BLOB_CENTRES):
        for _ in range(N_PER_BLOB):
            data.append((cx + rng_data.gauss(0, SIGMA), cy + rng_data.gauss(0, SIGMA)))
            true_labels.append(cls)

    print(f"Dataset: {len(data)} points in {len(BLOB_CENTRES)} blobs "
          f"(σ={SIGMA}, {N_PER_BLOB} pts/blob)")

    # ------------------------------------------------------------------
    # 2.  Elbow analysis for k = 1 … 8.
    # ------------------------------------------------------------------
    k_range = range(1, 9)
    inertias = elbow_inertias(data, k_range, seed=42)
    print_elbow_plot(inertias)

    # Also show the numeric table for reference.
    print(f"  {'k':>3}  {'inertia':>14}")
    print(f"  {'─'*3}  {'─'*14}")
    for k_val, iner in inertias.items():
        print(f"  {k_val:>3}  {iner:>14.2f}")
    print()

    # ------------------------------------------------------------------
    # 3.  Run k-means with the true k.
    # ------------------------------------------------------------------
    k = len(BLOB_CENTRES)
    result = kmeans(data, k, seed=42)

    print(f"k={k}  final inertia : {result.inertia:.4f}")
    print(f"      iterations    : {len(result.inertia_history)}")
    for i, c in enumerate(result.centroids):
        print(f"      centroid {i}    : ({c[0]:+.3f}, {c[1]:+.3f})")
    print()

    # Inertia history (shows convergence speed).
    print("Inertia per Lloyd step:")
    for step, iner in enumerate(result.inertia_history):
        bar_len = round(iner / result.inertia_history[0] * 40)
        print(f"  step {step:>2}: {iner:10.2f}  {'█' * bar_len}")
    print()

    # ------------------------------------------------------------------
    # 4.  Self-check: predicted labels should match true labels up to a
    #     permutation of cluster indices.
    #
    #     Strategy: for each predicted cluster, vote for the true label
    #     that the majority of its members carry.  Then remap predictions
    #     and measure accuracy.  With tight, well-separated blobs this
    #     should be ≥ 95 %.
    # ------------------------------------------------------------------
    vote: list[list[int]] = [[0] * k for _ in range(k)]
    for pred, true in zip(result.labels, true_labels):
        vote[pred][true] += 1

    # Greedy bijection: assign each predicted cluster to its top true label
    # in descending order of confidence to avoid collisions.
    perm: dict[int, int] = {}
    used: set[int] = set()
    for pred in sorted(range(k), key=lambda p: max(vote[p]), reverse=True):
        best = max(
            (j for j in range(k) if j not in used),
            key=lambda j, p=pred: vote[p][j],
        )
        perm[pred] = best
        used.add(best)

    remapped = [perm[lbl] for lbl in result.labels]
    correct = sum(r == t for r, t in zip(remapped, true_labels))
    accuracy = correct / len(data)

    print(f"Self-check: {correct}/{len(data)} correct  ({accuracy:.1%})")

    if accuracy < 0.95:
        print("FAIL — labelling accuracy below 95 %", file=sys.stderr)
        sys.exit(1)

    print("PASS ✓")
