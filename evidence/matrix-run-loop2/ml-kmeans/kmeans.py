"""
kmeans.py – k-means clustering, stdlib only, Python 3.12

Features
--------
• k-means++ initialisation
• Lloyd's algorithm (iterative assignment + centroid update)
• Empty-cluster rescue: re-seed to the point furthest from its centroid
• Inertia tracking and ASCII elbow plot
• Deterministic seeding via an explicit random.Random instance
• __main__ demo on synthetic 2D data with a labelling self-check
"""

from __future__ import annotations

import math
import random
from typing import Sequence


# ── types ──────────────────────────────────────────────────────────────────

Point = tuple[float, ...]   # D-dimensional point represented as a float tuple
Label = int                 # cluster index in [0, k)


# ── distance helpers ───────────────────────────────────────────────────────

def _sq_dist(a: Point, b: Point) -> float:
    """Squared Euclidean distance between two equal-length points."""
    return sum((x - y) ** 2 for x, y in zip(a, b))


# ── k-means++ initialisation ───────────────────────────────────────────────

def _kmeanspp(points: list[Point], k: int, rng: random.Random) -> list[Point]:
    """
    Choose k initial centroids using the k-means++ scheme.

    The first centroid is drawn uniformly at random.  Each subsequent
    centroid is drawn with probability proportional to D²(x) — the
    squared distance from x to the nearest already-chosen centroid.
    This reduces expected initialisation cost from O(log k) to O(1)
    compared with a uniform random pick, and dramatically cuts the
    chance of a poor local minimum.
    """
    centres: list[Point] = [points[rng.randrange(len(points))]]

    for _ in range(1, k):
        # D²(x) for every point against the current set of centres
        d2 = [min(_sq_dist(p, c) for c in centres) for p in points]
        total = sum(d2)
        # Weighted random selection; walk the cumulative sum
        threshold = rng.random() * total
        acc = 0.0
        chosen = points[-1]  # safety fallback if floats sum short
        for p, w in zip(points, d2):
            acc += w
            if acc >= threshold:
                chosen = p
                break
        centres.append(chosen)

    return centres


# ── Lloyd's iteration helpers ──────────────────────────────────────────────

def _assign(points: list[Point], centres: list[Point]) -> list[Label]:
    """Return the index of the nearest centroid for every point."""
    return [
        min(range(len(centres)), key=lambda j, p=p: _sq_dist(p, centres[j]))
        for p in points
    ]


def _update_centres(
    points: list[Point],
    labels: list[Label],
    k: int,
    prev_centres: list[Point],
) -> list[Point]:
    """
    Recompute centroids as the mean of their assigned points.

    Empty-cluster rescue: when a cluster has no members (which can happen
    if two centres collapse onto the same region) its centroid is replaced
    by the point that sits furthest from *its own* assigned centroid.
    That point contributes the most to inertia and is the best candidate
    to seed a new cluster.  Multiple empty clusters are handled in a single
    pass; each rescued point is used at most once.
    """
    dim = len(points[0])
    sums = [[0.0] * dim for _ in range(k)]
    counts = [0] * k

    for p, lbl in zip(points, labels):
        for d in range(dim):
            sums[lbl][d] += p[d]
        counts[lbl] += 1

    # Pre-compute sq distance of each point to its currently assigned centroid
    sq_to_own = [_sq_dist(p, prev_centres[lbl]) for p, lbl in zip(points, labels)]

    new_centres: list[Point] = []
    rescued: set[int] = set()   # point indices already used as rescue seeds

    for j in range(k):
        if counts[j] > 0:
            new_centres.append(tuple(sums[j][d] / counts[j] for d in range(dim)))
        else:
            # Pick the point furthest from its centroid, not already a rescue seed
            idx = max(
                (i for i in range(len(points)) if i not in rescued),
                key=lambda i: sq_to_own[i],
            )
            rescued.add(idx)
            new_centres.append(points[idx])

    return new_centres


def _inertia(points: list[Point], labels: list[Label], centres: list[Point]) -> float:
    """Sum of squared distances of each point to its assigned centroid."""
    return sum(_sq_dist(p, centres[lbl]) for p, lbl in zip(points, labels))


# ── k-means ────────────────────────────────────────────────────────────────

def kmeans(
    points: Sequence[Point],
    k: int,
    *,
    max_iter: int = 300,
    tol: float = 1e-9,
    seed: int | None = 42,
    n_init: int = 10,
) -> tuple[list[Label], list[Point], float]:
    """
    Run k-means clustering with k-means++ initialisation.

    Parameters
    ----------
    points   : sequence of equal-length float tuples
    k        : number of clusters, 1 ≤ k ≤ len(points)
    max_iter : maximum Lloyd iterations per restart
    tol      : convergence threshold on absolute change in inertia
    seed     : integer seed for reproducibility; None uses system entropy
    n_init   : number of independent restarts; the lowest-inertia result
                is returned

    Returns
    -------
    labels   : cluster index for each input point (same order as input)
    centres  : list of k centroids as float tuples
    inertia  : sum of squared distances to assigned centroids
    """
    pts = list(points)
    n = len(pts)
    if not 1 <= k <= n:
        raise ValueError(f"k must satisfy 1 ≤ k ≤ {n}, got {k}")

    rng = random.Random(seed)
    best_labels: list[Label] = []
    best_centres: list[Point] = []
    best_inertia = math.inf

    for _ in range(n_init):
        # Initialise
        centres = _kmeanspp(pts, k, rng)
        labels = _assign(pts, centres)
        cur_inertia = _inertia(pts, labels, centres)

        # Lloyd iterations
        for _ in range(max_iter):
            centres = _update_centres(pts, labels, k, centres)
            labels = _assign(pts, centres)
            new_inertia = _inertia(pts, labels, centres)
            if abs(cur_inertia - new_inertia) < tol:
                cur_inertia = new_inertia
                break
            cur_inertia = new_inertia

        if cur_inertia < best_inertia:
            best_inertia = cur_inertia
            best_labels = labels[:]
            best_centres = centres[:]

    return best_labels, best_centres, best_inertia


# ── elbow analysis ─────────────────────────────────────────────────────────

def elbow_inertias(
    points: Sequence[Point],
    k_range: Sequence[int],
    *,
    seed: int | None = 42,
    n_init: int = 10,
    max_iter: int = 300,
) -> dict[int, float]:
    """
    Run k-means for every k in k_range and return {k: inertia}.

    The same seed is reused across all values of k so comparisons are
    not confounded by different RNG trajectories.
    """
    return {
        k: kmeans(points, k, seed=seed, n_init=n_init, max_iter=max_iter)[2]
        for k in k_range
    }


# ── ASCII elbow plot ───────────────────────────────────────────────────────

def print_elbow_plot(
    inertias: dict[int, float],
    *,
    height: int = 20,
    col_width: int = 5,
    title: str = "Elbow plot  (inertia vs k)",
) -> None:
    """
    Print an ASCII scatter plot of inertia vs k.

    Layout
    ------
    Each column corresponds to one value of k.  A point is plotted at
    the row that corresponds to the scaled inertia value.  The k with
    the largest positive second derivative (the 'elbow' — where the
    rate of decrease bends most sharply) is marked ◆; all others use ●.
    Y-axis labels show the actual inertia values.
    """
    ks = sorted(inertias)
    vals = [inertias[k] for k in ks]

    lo, hi = min(vals), max(vals)
    span = hi - lo if hi != lo else 1.0

    def _row(v: float) -> int:
        """Map inertia to a grid row; row 0 = top = highest inertia."""
        return round((hi - v) / span * (height - 1))

    # Identify elbow: index of the largest positive second finite difference
    # Δ²[i] = vals[i-1] - 2·vals[i] + vals[i+1]  (positive ⟹ concave up)
    elbow_col: int | None = None
    elbow_k: int | None = None
    if len(vals) >= 3:
        d2 = [vals[i - 1] - 2 * vals[i] + vals[i + 1] for i in range(1, len(vals) - 1)]
        best = d2.index(max(d2)) + 1   # +1: d2[0] corresponds to vals[1]
        elbow_col = best
        elbow_k = ks[best]

    # Build the character grid
    grid = [[" "] * len(ks) for _ in range(height)]
    for col, v in enumerate(vals):
        r = _row(v)
        grid[r][col] = "◆" if col == elbow_col else "●"

    total_width = col_width * len(ks)
    print()
    print(title.center(total_width + 13))
    print()

    for r in range(height):
        y_val = hi - (r / (height - 1)) * span
        row_label = f"{y_val:>11.1f} │"
        print(row_label + "".join(ch.center(col_width) for ch in grid[r]))

    # X-axis
    print(" " * 12 + "└" + "─" * total_width)
    print(" " * 13 + "".join(str(k).center(col_width) for k in ks))
    print(" " * 13 + "k")

    if elbow_k is not None:
        print(f"\n  ◆  suggested elbow at k = {elbow_k}")
    print()


# ── __main__ demo ──────────────────────────────────────────────────────────

if __name__ == "__main__":
    import sys

    # ── 1. Synthetic data: four Gaussian blobs in 2-D ────────────────────
    TRUE_K = 4
    PER_CLUSTER = 60
    SEED = 42

    rng = random.Random(SEED)
    blob_centres_gt = [(2.0, 2.0), (9.0, 2.0), (2.0, 9.0), (9.0, 9.0)]

    data: list[Point] = []
    ground_truth: list[int] = []

    for cid, (cx, cy) in enumerate(blob_centres_gt):
        for _ in range(PER_CLUSTER):
            data.append((cx + rng.gauss(0.0, 0.6), cy + rng.gauss(0.0, 0.6)))
            ground_truth.append(cid)

    n_total = len(data)
    print(f"Generated {n_total} 2-D points across {TRUE_K} Gaussian blobs.\n")

    # ── 2. Elbow analysis for k = 1 … 8 ──────────────────────────────────
    k_range = list(range(1, 9))
    print("Running elbow analysis (k = 1 … 8) …")
    inertia_map = elbow_inertias(data, k_range, seed=SEED)
    print_elbow_plot(inertia_map, title="Elbow plot — 4-blob synthetic data")

    # ── 3. Cluster with the true k ────────────────────────────────────────
    labels, centres, final_inertia = kmeans(data, TRUE_K, seed=SEED)
    print(f"k-means  k={TRUE_K}  →  inertia = {final_inertia:.4f}")
    for j, c in enumerate(centres):
        cnt = labels.count(j)
        print(f"  centre {j}  ({c[0]:6.3f}, {c[1]:6.3f})  —  {cnt} points")

    # ── 4. Self-check: recovered clusters should map 1-to-1 to blobs ─────
    # For each recovered label find the dominant ground-truth label (majority vote).
    vote: dict[int, dict[int, int]] = {j: {} for j in range(TRUE_K)}
    for lbl, gt in zip(labels, ground_truth):
        vote[lbl][gt] = vote[lbl].get(gt, 0) + 1

    mapping: dict[int, int] = {
        j: max(v, key=lambda gt: v[gt])   # type: ignore[arg-type]
        for j, v in vote.items()
        if v
    }

    correct = sum(
        1 for lbl, gt in zip(labels, ground_truth) if mapping.get(lbl) == gt
    )
    accuracy = correct / n_total
    print(f"\nSelf-check: {correct}/{n_total} correctly labelled  ({accuracy:.1%})")

    threshold = 0.95
    if accuracy >= threshold:
        print(f"PASS — accuracy ≥ {threshold:.0%}: recovered clusters match ground truth.")
    else:
        print(
            f"FAIL — accuracy {accuracy:.1%} is below {threshold:.0%}.",
            file=sys.stderr,
        )
        sys.exit(1)
