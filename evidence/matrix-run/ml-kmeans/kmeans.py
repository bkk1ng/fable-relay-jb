"""
k-means clustering — stdlib only, Python 3.12+

Features
--------
* k-means++ initialisation for better starting centroids
* Lloyd's algorithm iterations until convergence or max_iter
* Empty-cluster recovery: reassign to the point farthest from its centroid
* Inertia tracking with ASCII elbow plot
* Deterministic seeding via random.seed / a seed= parameter
* __main__ demo on synthetic 2-D data with a correctness self-check
"""

from __future__ import annotations

import math
import random
from typing import Sequence


# ---------------------------------------------------------------------------
# Type aliases
# ---------------------------------------------------------------------------
Point = tuple[float, ...]
Centroid = list[float]


# ---------------------------------------------------------------------------
# Distance helpers
# ---------------------------------------------------------------------------

def _sq_dist(a: Point, b: Point | Centroid) -> float:
    """Squared Euclidean distance between two equal-length sequences."""
    return sum((x - y) ** 2 for x, y in zip(a, b))


def _dist(a: Point, b: Point | Centroid) -> float:
    return math.sqrt(_sq_dist(a, b))


# ---------------------------------------------------------------------------
# k-means++ initialisation
# ---------------------------------------------------------------------------

def _kmeanspp_init(
    points: list[Point],
    k: int,
    rng: random.Random,
) -> list[Centroid]:
    """
    Choose k seed centroids using the k-means++ probability scheme.

    Each successive centroid is sampled from the data with probability
    proportional to D²(x) — the squared distance to the nearest already-
    chosen centroid.  This spreads seeds across the data and cuts the
    expected number of Lloyd iterations needed to converge.
    """
    # Pick the first centroid uniformly at random.
    centroids: list[Centroid] = [list(rng.choice(points))]

    for _ in range(1, k):
        # D²(x) for each point: squared distance to nearest chosen centroid.
        d2 = [min(_sq_dist(p, c) for c in centroids) for p in points]
        total = sum(d2)
        if total == 0.0:
            # All remaining points coincide with an existing centroid.
            centroids.append(list(rng.choice(points)))
            continue
        # Weighted random pick.
        threshold = rng.random() * total
        cumulative = 0.0
        chosen = points[-1]
        for p, w in zip(points, d2):
            cumulative += w
            if cumulative >= threshold:
                chosen = p
                break
        centroids.append(list(chosen))

    return centroids


# ---------------------------------------------------------------------------
# Assignment and update steps
# ---------------------------------------------------------------------------

def _assign(points: list[Point], centroids: list[Centroid]) -> list[int]:
    """Return the index of the nearest centroid for every point."""
    labels: list[int] = []
    for p in points:
        best_idx = min(range(len(centroids)), key=lambda i: _sq_dist(p, centroids[i]))
        labels.append(best_idx)
    return labels


def _update_centroids(
    points: list[Point],
    labels: list[int],
    k: int,
    dim: int,
    old_centroids: list[Centroid],
) -> list[Centroid]:
    """
    Recompute each centroid as the mean of its assigned points.

    Empty-cluster recovery: if a cluster has no points assigned, the centroid
    is moved to whichever point is farthest from *its* current centroid.  This
    avoids a degenerate solution with fewer than k clusters.
    """
    sums = [[0.0] * dim for _ in range(k)]
    counts = [0] * k

    for p, label in zip(points, labels):
        for d in range(dim):
            sums[label][d] += p[d]
        counts[label] += 1

    new_centroids: list[Centroid] = []
    for i in range(k):
        if counts[i] > 0:
            new_centroids.append([sums[i][d] / counts[i] for d in range(dim)])
        else:
            # Empty cluster: steal the point farthest from its own centroid.
            farthest = max(
                range(len(points)),
                key=lambda idx: _sq_dist(points[idx], old_centroids[labels[idx]]),
            )
            new_centroids.append(list(points[farthest]))

    return new_centroids


def _inertia(points: list[Point], labels: list[int], centroids: list[Centroid]) -> float:
    """Sum of squared distances from each point to its assigned centroid."""
    return sum(_sq_dist(p, centroids[label]) for p, label in zip(points, labels))


# ---------------------------------------------------------------------------
# Main k-means function
# ---------------------------------------------------------------------------

def kmeans(
    points: Sequence[Sequence[float]],
    k: int,
    *,
    max_iter: int = 300,
    tol: float = 1e-6,
    seed: int | None = 42,
    n_init: int = 10,
) -> tuple[list[int], list[Centroid], float]:
    """
    Fit k-means clustering and return the best result over n_init runs.

    Parameters
    ----------
    points   : sequence of equal-length numeric sequences
    k        : number of clusters (1 ≤ k ≤ len(points))
    max_iter : maximum Lloyd iterations per run
    tol      : convergence threshold — stop when centroid shift < tol
    seed     : base RNG seed for reproducibility; each run uses seed+i
    n_init   : number of independent random initialisations

    Returns
    -------
    labels    : cluster index for each point  (len == len(points))
    centroids : final centroid list            (len == k)
    inertia   : total within-cluster sum of squares for the best run
    """
    pts: list[Point] = [tuple(float(x) for x in p) for p in points]
    n = len(pts)
    if n == 0:
        raise ValueError("points must be non-empty")
    dim = len(pts[0])
    if any(len(p) != dim for p in pts):
        raise ValueError("all points must have the same dimension")
    if not (1 <= k <= n):
        raise ValueError(f"k must satisfy 1 ≤ k ≤ {n}, got {k}")

    best_labels: list[int] = []
    best_centroids: list[Centroid] = []
    best_inertia = math.inf

    for run in range(n_init):
        rng = random.Random(None if seed is None else seed + run)
        centroids = _kmeanspp_init(pts, k, rng)

        labels: list[int] = []
        for iteration in range(max_iter):
            labels = _assign(pts, centroids)
            new_centroids = _update_centroids(pts, labels, k, dim, centroids)

            # Check convergence: maximum shift of any centroid.
            shift = max(
                _dist(c_old, c_new)
                for c_old, c_new in zip(centroids, new_centroids)
            )
            centroids = new_centroids
            if shift < tol:
                break

        trial_inertia = _inertia(pts, labels, centroids)
        if trial_inertia < best_inertia:
            best_inertia = trial_inertia
            best_labels = labels
            best_centroids = centroids

    return best_labels, best_centroids, best_inertia


# ---------------------------------------------------------------------------
# Elbow analysis
# ---------------------------------------------------------------------------

def elbow_inertias(
    points: Sequence[Sequence[float]],
    k_range: range | Sequence[int],
    **kmeans_kwargs,
) -> dict[int, float]:
    """
    Run k-means for each k in k_range and collect the inertia at each k.

    Returns a dict mapping k → inertia, suitable for an elbow plot.
    """
    result: dict[int, float] = {}
    for k in k_range:
        _, _, inertia = kmeans(points, k, **kmeans_kwargs)
        result[k] = inertia
    return result


def print_elbow_plot(
    inertias: dict[int, float],
    *,
    width: int = 60,
    height: int = 15,
) -> None:
    """
    Render an ASCII elbow plot for a k → inertia mapping.

    The x-axis is k; the y-axis is inertia (larger = worse fit).
    """
    if not inertias:
        print("(no data)")
        return

    ks = sorted(inertias)
    values = [inertias[k] for k in ks]

    min_val = min(values)
    max_val = max(values)
    val_range = max_val - min_val or 1.0

    # Build a grid: rows × cols
    grid = [[" "] * width for _ in range(height)]

    def col_for_k(idx: int) -> int:
        """Map k index (0-based among ks) to a column in the grid."""
        if len(ks) == 1:
            return width // 2
        return int(idx / (len(ks) - 1) * (width - 1))

    def row_for_val(v: float) -> int:
        """Map inertia value to a row (0 = top = high inertia)."""
        normalised = (v - min_val) / val_range          # 0 (low) … 1 (high)
        return int((1.0 - normalised) * (height - 1))   # invert: high → top row

    # Plot each k.
    for idx, (k, v) in enumerate(zip(ks, values)):
        c = col_for_k(idx)
        r = row_for_val(v)
        grid[r][c] = "●"

    # Connect adjacent points with dashes (rough horizontal run).
    for idx in range(len(ks) - 1):
        c0 = col_for_k(idx)
        c1 = col_for_k(idx + 1)
        r0 = row_for_val(values[idx])
        r1 = row_for_val(values[idx + 1])
        steps = max(abs(c1 - c0), abs(r1 - r0), 1)
        for s in range(1, steps):
            t = s / steps
            cr = int(r0 + t * (r1 - r0))
            cc = int(c0 + t * (c1 - c0))
            if grid[cr][cc] == " ":
                grid[cr][cc] = "·"

    # Y-axis labels (two: max at top, min at bottom).
    y_label_width = 10
    print()
    print("  Inertia (elbow plot)")
    for r, row in enumerate(grid):
        if r == 0:
            label = f"{max_val:>{y_label_width}.1f} |"
        elif r == height - 1:
            label = f"{min_val:>{y_label_width}.1f} |"
        else:
            label = " " * y_label_width + " |"
        print(label + "".join(row))

    # X-axis.
    print(" " * (y_label_width + 2) + "-" * width)
    # K tick labels — first, last, and a few in between.
    tick_line = [" "] * width
    for idx, k in enumerate(ks):
        c = col_for_k(idx)
        label = str(k)
        for j, ch in enumerate(label):
            pos = c + j
            if 0 <= pos < width:
                tick_line[pos] = ch
    print(" " * (y_label_width + 2) + "".join(tick_line))
    print(" " * (y_label_width + 2 + width // 2 - 1) + "k")
    print()


# ---------------------------------------------------------------------------
# __main__ demo
# ---------------------------------------------------------------------------

def _make_demo_points(seed: int = 0) -> list[tuple[float, float]]:
    """
    Generate three well-separated Gaussian blobs in 2-D.

    Blob centres: (0, 0), (8, 0), (4, 7)
    Each blob has 40 points with std ≈ 0.9.
    """
    rng = random.Random(seed)
    centres = [(0.0, 0.0), (8.0, 0.0), (4.0, 7.0)]
    sigma = 0.9
    pts: list[tuple[float, float]] = []
    for cx, cy in centres:
        for _ in range(40):
            x = cx + rng.gauss(0.0, sigma)
            y = cy + rng.gauss(0.0, sigma)
            pts.append((x, y))
    return pts


def _self_check(
    points: list[tuple[float, float]],
    labels: list[int],
    n_per_blob: int = 40,
) -> bool:
    """
    Verify that all points from the same ground-truth blob share one label.

    The ground truth is implicit in the construction: points 0..39 belong to
    blob 0, 40..79 to blob 1, 80..119 to blob 2.  The clustering is correct
    if each blob maps to exactly one cluster (even if the cluster indices differ
    from the blob indices).
    """
    n_blobs = len(points) // n_per_blob
    for b in range(n_blobs):
        blob_labels = labels[b * n_per_blob : (b + 1) * n_per_blob]
        if len(set(blob_labels)) != 1:
            return False
    return True


def main() -> None:
    print("=" * 70)
    print("  k-means demo — 3 Gaussian blobs in 2-D, stdlib only")
    print("=" * 70)

    pts = _make_demo_points(seed=7)
    print(f"\nDataset: {len(pts)} points, 3 true clusters\n")

    # --- Elbow analysis for k = 1 … 7 ---
    print("Running elbow analysis for k = 1 … 7 …")
    inertias = elbow_inertias(pts, range(1, 8), seed=42, n_init=5)
    for k, v in sorted(inertias.items()):
        print(f"  k={k}  inertia={v:10.2f}")
    print_elbow_plot(inertias)

    # --- Fit at the true k = 3 ---
    print("Fitting k=3 …")
    labels, centroids, inertia = kmeans(pts, k=3, seed=42, n_init=10)

    print(f"\nFinal inertia : {inertia:.4f}")
    print("Centroids found:")
    for i, c in enumerate(centroids):
        print(f"  cluster {i}: ({c[0]:+.3f}, {c[1]:+.3f})")

    cluster_sizes = [labels.count(i) for i in range(3)]
    print(f"\nCluster sizes : {cluster_sizes}")

    # --- Self-check ---
    ok = _self_check(pts, labels)
    status = "PASS ✓" if ok else "FAIL ✗"
    print(f"\nLabelling self-check : {status}")
    if not ok:
        # Print which blob failed for debugging.
        n_per_blob = 40
        for b in range(3):
            blob_labels = labels[b * n_per_blob : (b + 1) * n_per_blob]
            unique = set(blob_labels)
            if len(unique) != 1:
                print(f"  Blob {b} split across clusters: {unique}")

    print()


if __name__ == "__main__":
    main()
