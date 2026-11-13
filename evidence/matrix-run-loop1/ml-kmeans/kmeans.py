"""
k-means clustering — stdlib only, Python 3.12
  • k-means++ initialisation
  • Lloyd iterations
  • Empty-cluster handling (reassign to point with highest inertia contribution)
  • Inertia tracking with ASCII elbow plot
  • Deterministic seeding via random.seed()
  • __main__ demo on 2-D synthetic data with a self-check on labelling
"""

from __future__ import annotations

import math
import random
from typing import NamedTuple


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

Point = tuple[float, ...]   # arbitrary-dimensional point


class KMeansResult(NamedTuple):
    labels:    list[int]        # cluster index for each input point
    centroids: list[Point]      # final centroid positions
    inertia:   float            # sum of squared distances to assigned centroid
    iterations: int             # Lloyd iterations performed


# ---------------------------------------------------------------------------
# Distance helpers
# ---------------------------------------------------------------------------

def _sq_dist(a: Point, b: Point) -> float:
    """Squared Euclidean distance."""
    return sum((x - y) ** 2 for x, y in zip(a, b))


def _dist(a: Point, b: Point) -> float:
    return math.sqrt(_sq_dist(a, b))


# ---------------------------------------------------------------------------
# k-means++ initialisation
# ---------------------------------------------------------------------------

def _kmeanspp_init(points: list[Point], k: int, rng: random.Random) -> list[Point]:
    """
    Choose k seed centroids via k-means++.
    Each subsequent centre is chosen with probability proportional to its
    squared distance from the nearest already-chosen centre.
    """
    centroids: list[Point] = [rng.choice(points)]

    for _ in range(1, k):
        # Squared distance from each point to its nearest current centroid
        d2 = [min(_sq_dist(p, c) for c in centroids) for p in points]
        total = sum(d2)
        # Weighted random pick
        r = rng.uniform(0.0, total)
        cumulative = 0.0
        chosen = points[-1]
        for p, w in zip(points, d2):
            cumulative += w
            if cumulative >= r:
                chosen = p
                break
        centroids.append(chosen)

    return centroids


# ---------------------------------------------------------------------------
# Assignment and centroid update
# ---------------------------------------------------------------------------

def _assign(points: list[Point], centroids: list[Point]) -> list[int]:
    """Return index of nearest centroid for every point."""
    labels = []
    for p in points:
        best_idx = min(range(len(centroids)), key=lambda i: _sq_dist(p, centroids[i]))
        labels.append(best_idx)
    return labels


def _recompute_centroids(
    points: list[Point],
    labels: list[int],
    k: int,
    old_centroids: list[Point],
) -> list[Point]:
    """
    Compute mean of each cluster.
    Empty clusters: reassign to the point that is farthest from its current
    centroid (highest per-point inertia contribution), then recompute.
    """
    dim = len(points[0])
    new_centroids: list[Point] = []

    for ci in range(k):
        members = [p for p, l in zip(points, labels) if l == ci]

        if members:
            mean = tuple(sum(coord[d] for coord in members) / len(members) for d in range(dim))
            new_centroids.append(mean)
        else:
            # Empty-cluster recovery: steal the point with the largest
            # squared distance to its currently assigned centroid.
            sq_dists = [_sq_dist(p, old_centroids[l]) for p, l in zip(points, labels)]
            worst_idx = max(range(len(sq_dists)), key=lambda i: sq_dists[i])
            new_centroids.append(points[worst_idx])
            # Mark that point as belonging to the rescued cluster so a later
            # cluster cannot steal the same point in the same iteration.
            labels[worst_idx] = ci

    return new_centroids


def _inertia(points: list[Point], labels: list[int], centroids: list[Point]) -> float:
    return sum(_sq_dist(p, centroids[l]) for p, l in zip(points, labels))


# ---------------------------------------------------------------------------
# Main k-means function
# ---------------------------------------------------------------------------

def kmeans(
    points: list[Point],
    k: int,
    *,
    max_iter: int = 300,
    tol: float = 1e-6,
    seed: int | None = 42,
    n_init: int = 10,
) -> KMeansResult:
    """
    Run k-means with k-means++ initialisation.

    Parameters
    ----------
    points   : list of equal-length tuples (any dimension ≥ 1)
    k        : number of clusters (1 ≤ k ≤ len(points))
    max_iter : maximum Lloyd iterations per run
    tol      : convergence threshold on centroid shift (L2)
    seed     : random seed for reproducibility; None → non-deterministic
    n_init   : number of independent restarts; best inertia is returned

    Returns
    -------
    KMeansResult with labels, centroids, inertia, iterations
    """
    if not points:
        raise ValueError("points must not be empty")
    if not 1 <= k <= len(points):
        raise ValueError(f"k must be between 1 and len(points), got k={k}")

    rng = random.Random(seed)

    best: KMeansResult | None = None

    for _ in range(n_init):
        centroids = _kmeanspp_init(points, k, rng)
        labels: list[int] = []
        iters = 0

        for iters in range(1, max_iter + 1):
            labels = _assign(points, centroids)
            new_centroids = _recompute_centroids(points, labels, k, centroids)

            # Convergence check: max centroid shift
            shift = max(_dist(a, b) for a, b in zip(centroids, new_centroids))
            centroids = new_centroids
            if shift < tol:
                break

        score = _inertia(points, labels, centroids)
        result = KMeansResult(labels=labels, centroids=centroids, inertia=score, iterations=iters)
        if best is None or score < best.inertia:
            best = result

    assert best is not None
    return best


# ---------------------------------------------------------------------------
# Elbow plot
# ---------------------------------------------------------------------------

def elbow_plot(
    points: list[Point],
    k_range: range | None = None,
    *,
    width: int = 60,
    height: int = 15,
    seed: int | None = 42,
) -> None:
    """
    Print an ASCII elbow plot of inertia vs k.

    Parameters
    ----------
    points  : dataset
    k_range : range of k values to test (default: range(1, min(11, len(points)+1)))
    width   : character width of the plot area
    height  : character height of the plot area
    seed    : forwarded to kmeans() for reproducibility
    """
    if k_range is None:
        k_range = range(1, min(11, len(points) + 1))

    ks = list(k_range)
    inertias = [kmeans(points, k, seed=seed).inertia for k in ks]

    min_i = min(inertias)
    max_i = max(inertias)
    span = max_i - min_i or 1.0

    # Build a grid of spaces, then paint '*' marks
    grid = [[" " for _ in range(width)] for _ in range(height)]

    for k, inertia in zip(ks, inertias):
        # Map k to x column
        x = int((k - ks[0]) / max(len(ks) - 1, 1) * (width - 1))
        # Map inertia to y row (high inertia → top row 0, low → bottom row height-1)
        norm = (inertia - min_i) / span          # 0 = min, 1 = max
        y = int((1.0 - norm) * (height - 1))     # invert: high value → low row index
        y = max(0, min(height - 1, y))
        grid[y][x] = "*"

    # Render
    print(f"\n  Inertia vs k  (inertia range {min_i:.1f} – {max_i:.1f})")
    print("  " + "┌" + "─" * width + "┐")
    for row in grid:
        print("  │" + "".join(row) + "│")
    print("  └" + "─" * width + "┘")
    # x-axis labels
    label_line = ["  "] + [" "] * (width + 2)
    for k in ks:
        x = int((k - ks[0]) / max(len(ks) - 1, 1) * (width - 1))
        tag = str(k)
        pos = 2 + x + 1  # +2 for "  ", +1 for the border char
        for j, ch in enumerate(tag):
            if pos + j < len(label_line):
                label_line[pos + j] = ch
    print("".join(label_line))
    print("  " + " " * (width // 2 - 1) + "k\n")


# ---------------------------------------------------------------------------
# Self-check helper
# ---------------------------------------------------------------------------

def _check_labels(
    points: list[Point],
    labels: list[int],
    expected_groups: list[list[int]],
) -> bool:
    """
    Verify that points in the same expected group share the same predicted
    label, and points in different groups have different labels.
    Returns True if the labelling is consistent, False otherwise.
    """
    group_labels: dict[int, int] = {}  # expected_group_index → assigned label

    for group_idx, indices in enumerate(expected_groups):
        assigned = {labels[i] for i in indices}
        if len(assigned) != 1:
            print(f"  FAIL: expected group {group_idx} has mixed labels {assigned}")
            return False
        assigned_label = next(iter(assigned))
        for other_idx, other_label in group_labels.items():
            if other_label == assigned_label:
                print(
                    f"  FAIL: group {group_idx} and group {other_idx} "
                    f"share label {assigned_label}"
                )
                return False
        group_labels[group_idx] = assigned_label

    return True


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def _make_cluster(
    cx: float, cy: float, n: int, spread: float, rng: random.Random
) -> list[Point]:
    return [
        (cx + rng.gauss(0, spread), cy + rng.gauss(0, spread))
        for _ in range(n)
    ]


def main() -> None:
    rng = random.Random(0)

    # Three well-separated 2-D clusters
    cluster_a = _make_cluster(0.0,  0.0,  40, 0.4, rng)   # indices   0 –  39
    cluster_b = _make_cluster(5.0,  0.0,  40, 0.4, rng)   # indices  40 –  79
    cluster_c = _make_cluster(2.5,  4.5,  40, 0.4, rng)   # indices  80 – 119

    points = cluster_a + cluster_b + cluster_c

    # --- Elbow plot ---------------------------------------------------------
    print("=" * 60)
    print("  K-MEANS DEMO — 120 points, three Gaussian clusters")
    print("=" * 60)
    elbow_plot(points, range(1, 9), seed=42)

    # --- Fit with k=3 -------------------------------------------------------
    result = kmeans(points, k=3, seed=42)
    print(f"  k=3 fit: inertia={result.inertia:.4f}, "
          f"iterations={result.iterations}")
    print("  Centroids:")
    for i, c in enumerate(result.centroids):
        print(f"    cluster {i}: ({c[0]:.3f}, {c[1]:.3f})")

    # --- Self-check ---------------------------------------------------------
    expected_groups = [
        list(range(0,   40)),   # cluster_a
        list(range(40,  80)),   # cluster_b
        list(range(80, 120)),   # cluster_c
    ]
    ok = _check_labels(points, result.labels, expected_groups)
    status = "PASS ✓" if ok else "FAIL ✗"
    print(f"\n  Labelling self-check: {status}")

    # --- Confirm degenerate cases -------------------------------------------
    # k=1 → all points in one cluster
    r1 = kmeans(points, k=1, seed=42)
    assert len(set(r1.labels)) == 1, "k=1 should produce exactly one label"

    # k = len(points) → every point is its own cluster (inertia ≈ 0)
    tiny = [(float(i), 0.0) for i in range(5)]
    rn = kmeans(tiny, k=5, seed=42)
    assert math.isclose(rn.inertia, 0.0, abs_tol=1e-9), (
        f"k=n should give inertia≈0, got {rn.inertia}"
    )

    print("  Edge-case checks: PASS ✓\n")


if __name__ == "__main__":
    main()
