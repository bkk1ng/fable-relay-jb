"""
k-means clustering — stdlib only, Python 3.12+

Features
--------
* k-means++ initialisation for better starting centroids
* Lloyd's algorithm iterations
* Empty-cluster handling (re-seed from highest-inertia point)
* Per-iteration inertia tracking
* ASCII elbow plot
* Deterministic seeding via random.seed()
* __main__ demo with 2-D synthetic data and a label self-check
"""

from __future__ import annotations

import math
import random
from typing import NamedTuple


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

Point = tuple[float, ...]          # arbitrary dimension
Centroid = tuple[float, ...]


class KMeansResult(NamedTuple):
    labels: list[int]              # cluster index for each input point
    centroids: list[Centroid]      # final centroid positions
    inertia: float                 # sum of squared distances to assigned centroid
    inertia_history: list[float]   # inertia after each Lloyd iteration


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

def _kmeanspp_init(points: list[Point], k: int, rng: random.Random) -> list[Centroid]:
    """
    Choose k seed centroids via k-means++ probability weighting.

    Each new centroid is drawn with probability proportional to the squared
    distance from the nearest already-chosen centroid, which spreads seeds
    across the data and reduces bad restarts.
    """
    centroids: list[Centroid] = [rng.choice(points)]

    for _ in range(k - 1):
        # D²(x) for every point: squared distance to the nearest centroid so far
        d2 = [min(_sq_dist(p, c) for c in centroids) for p in points]
        total = sum(d2)
        # weighted random pick
        threshold = rng.uniform(0.0, total)
        cumulative = 0.0
        chosen = points[-1]  # fallback (shouldn't be reached)
        for p, w in zip(points, d2):
            cumulative += w
            if cumulative >= threshold:
                chosen = p
                break
        centroids.append(chosen)

    return centroids


# ---------------------------------------------------------------------------
# Assignment and centroid update
# ---------------------------------------------------------------------------

def _assign(points: list[Point], centroids: list[Centroid]) -> list[int]:
    """Return the index of the nearest centroid for every point."""
    return [
        min(range(len(centroids)), key=lambda j: _sq_dist(p, centroids[j]))
        for p in points
    ]


def _update_centroids(
    points: list[Point],
    labels: list[int],
    k: int,
    old_centroids: list[Centroid],
    rng: random.Random,
) -> list[Centroid]:
    """
    Compute the mean of each cluster.

    Empty-cluster handling: if a cluster has no members, re-seed its centroid
    from the point that is farthest from its current centroid (highest
    individual inertia contribution).  This avoids degeneracy when k is large
    relative to the data.
    """
    dim = len(points[0])
    new_centroids: list[Centroid] = []

    for j in range(k):
        members = [points[i] for i, label in enumerate(labels) if label == j]

        if members:
            mean = tuple(
                sum(members[i][d] for i in range(len(members))) / len(members)
                for d in range(dim)
            )
            new_centroids.append(mean)
        else:
            # Re-seed: pick the point with the greatest distance to its centroid
            sq_dists = [_sq_dist(points[i], old_centroids[labels[i]]) for i in range(len(points))]
            fallback_idx = max(range(len(sq_dists)), key=lambda i: sq_dists[i])
            new_centroids.append(points[fallback_idx])

    return new_centroids


def _inertia(points: list[Point], labels: list[int], centroids: list[Centroid]) -> float:
    return sum(_sq_dist(p, centroids[labels[i]]) for i, p in enumerate(points))


# ---------------------------------------------------------------------------
# Main algorithm
# ---------------------------------------------------------------------------

def kmeans(
    points: list[Point],
    k: int,
    *,
    max_iter: int = 300,
    tol: float = 1e-9,
    seed: int | None = 42,
) -> KMeansResult:
    """
    Run k-means clustering.

    Parameters
    ----------
    points   : list of equal-length tuples (any numeric dimension)
    k        : number of clusters (1 ≤ k ≤ len(points))
    max_iter : maximum Lloyd iterations
    tol      : convergence threshold on centroid movement (L2 sum)
    seed     : integer seed for reproducibility; None → system entropy

    Returns
    -------
    KMeansResult with labels, centroids, final inertia, inertia history
    """
    if not points:
        raise ValueError("points must not be empty")
    if not 1 <= k <= len(points):
        raise ValueError(f"k must be between 1 and {len(points)}, got {k}")

    rng = random.Random(seed)

    centroids = _kmeanspp_init(points, k, rng)
    labels: list[int] = []
    inertia_history: list[float] = []

    for _ in range(max_iter):
        labels = _assign(points, centroids)
        new_centroids = _update_centroids(points, labels, k, centroids, rng)

        # Track inertia with the new centroids
        inertia_history.append(_inertia(points, labels, new_centroids))

        # Convergence: total centroid shift
        shift = sum(_dist(a, b) for a, b in zip(centroids, new_centroids))
        centroids = new_centroids

        if shift < tol:
            break

    final_labels = _assign(points, centroids)
    final_inertia = _inertia(points, final_labels, centroids)

    return KMeansResult(
        labels=final_labels,
        centroids=centroids,
        inertia=final_inertia,
        inertia_history=inertia_history,
    )


# ---------------------------------------------------------------------------
# ASCII elbow plot
# ---------------------------------------------------------------------------

def ascii_elbow_plot(
    k_values: list[int],
    inertias: list[float],
    *,
    width: int = 60,
    height: int = 15,
) -> str:
    """
    Render an elbow plot as ASCII text.

    Parameters
    ----------
    k_values : list of k values on the x-axis
    inertias : corresponding final inertia for each k
    width    : character width of the plot area
    height   : character height of the plot area

    Returns the multi-line string; caller decides how to display it.
    """
    if len(k_values) != len(inertias):
        raise ValueError("k_values and inertias must have the same length")

    min_i, max_i = min(inertias), max(inertias)
    span = max_i - min_i or 1.0

    # Build a 2-D grid of characters (height rows × width cols)
    grid = [[" "] * width for _ in range(height)]

    # Map each (k, inertia) to a grid cell and plot '*'
    min_k, max_k = min(k_values), max(k_values)
    k_span = max_k - min_k or 1

    plotted: set[tuple[int, int]] = set()
    for k_val, inertia in zip(k_values, inertias):
        col = round((k_val - min_k) / k_span * (width - 1))
        row = height - 1 - round((inertia - min_i) / span * (height - 1))
        row = max(0, min(height - 1, row))
        col = max(0, min(width - 1, col))
        grid[row][col] = "*"
        plotted.add((row, col))

    # Connect adjacent points with '-' or '/' or '\' for readability
    sorted_points = sorted(
        zip(k_values, inertias),
        key=lambda t: t[0],
    )
    screen_coords = []
    for k_val, inertia in sorted_points:
        col = round((k_val - min_k) / k_span * (width - 1))
        row = height - 1 - round((inertia - min_i) / span * (height - 1))
        row = max(0, min(height - 1, row))
        col = max(0, min(width - 1, col))
        screen_coords.append((row, col))

    for idx in range(len(screen_coords) - 1):
        r0, c0 = screen_coords[idx]
        r1, c1 = screen_coords[idx + 1]
        # Simple linear interpolation between the two '*' cells
        steps = max(abs(r1 - r0), abs(c1 - c0))
        for s in range(1, steps):
            r = round(r0 + (r1 - r0) * s / steps)
            c = round(c0 + (c1 - c0) * s / steps)
            if (r, c) not in plotted:
                dr = r1 - r0
                dc = c1 - c0
                if dr == 0:
                    ch = "-"
                elif dc == 0:
                    ch = "|"
                elif (dr < 0) == (dc > 0):
                    ch = "/"
                else:
                    ch = "\\"
                grid[r][c] = ch

    # Build the string with y-axis labels
    lines: list[str] = []
    lines.append(f"  Inertia")
    for row_idx, row in enumerate(grid):
        # Label every other row on the y-axis
        inertia_at_row = max_i - (row_idx / (height - 1)) * span
        label = f"{inertia_at_row:9.1f} |" if row_idx % 2 == 0 else "           |"
        lines.append(label + "".join(row))

    # x-axis
    lines.append("           +" + "-" * width)
    # x tick labels
    tick_line = "            "
    tick_step = max(1, (width) // max(len(k_values) - 1, 1))
    for k_val in k_values:
        col = round((k_val - min_k) / k_span * (width - 1))
        label = str(k_val)
        pos = col - len(label) // 2
        # Pad tick_line to the required position
        if pos > len(tick_line) - 12:
            tick_line += " " * (pos - (len(tick_line) - 12))
        tick_line += label
    lines.append(tick_line)
    lines.append("            " + " " * (width // 2 - 1) + "k")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# __main__ demo
# ---------------------------------------------------------------------------

def _generate_blobs(
    centres: list[tuple[float, float]],
    n_per_cluster: int,
    std: float,
    seed: int,
) -> tuple[list[Point], list[int]]:
    """Generate isotropic Gaussian blobs with known ground-truth labels."""
    rng = random.Random(seed)
    points: list[Point] = []
    true_labels: list[int] = []
    for label, (cx, cy) in enumerate(centres):
        for _ in range(n_per_cluster):
            x = cx + rng.gauss(0.0, std)
            y = cy + rng.gauss(0.0, std)
            points.append((x, y))
            true_labels.append(label)
    return points, true_labels


def _cluster_purity(true_labels: list[int], pred_labels: list[int], k: int) -> float:
    """
    Cluster purity: for each predicted cluster pick the majority true class,
    then compute the fraction of correctly attributed points.

    Purity = 1.0 means every predicted cluster is a perfect subset of one
    true class (up to the arbitrary permutation of cluster indices).
    """
    n = len(true_labels)
    total_correct = 0
    for j in range(k):
        members_true = [true_labels[i] for i in range(n) if pred_labels[i] == j]
        if members_true:
            majority_count = max(members_true.count(c) for c in set(members_true))
            total_correct += majority_count
    return total_correct / n


def main() -> None:
    print("=" * 70)
    print("k-means demo  (stdlib only, Python 3.12, deterministic seed=42)")
    print("=" * 70)

    # -----------------------------------------------------------------
    # 1. Synthetic data: 4 well-separated Gaussian blobs
    # -----------------------------------------------------------------
    TRUE_K = 4
    N_PER_CLUSTER = 60
    BLOB_CENTRES = [(0.0, 0.0), (8.0, 0.0), (4.0, 7.0), (12.0, 7.0)]

    points, true_labels = _generate_blobs(
        centres=BLOB_CENTRES,
        n_per_cluster=N_PER_CLUSTER,
        std=1.2,
        seed=0,
    )
    total_points = len(points)
    print(f"\nGenerated {total_points} points in {TRUE_K} blobs (each N={N_PER_CLUSTER}).\n")

    # -----------------------------------------------------------------
    # 2. Fit at the correct k
    # -----------------------------------------------------------------
    result = kmeans(points, k=TRUE_K, seed=42)

    purity = _cluster_purity(true_labels, result.labels, TRUE_K)
    print(f"Fit with k={TRUE_K}")
    print(f"  Final inertia : {result.inertia:,.2f}")
    print(f"  Iterations    : {len(result.inertia_history)}")
    print(f"  Cluster purity: {purity:.4f}  (1.0 = perfect assignment)\n")

    # Self-check: for tight blobs with generous separation the purity
    # should be essentially perfect.
    assert purity >= 0.95, (
        f"Purity {purity:.4f} unexpectedly low — check algorithm correctness."
    )
    print("  [PASS] Purity >= 0.95  — labelling self-check passed.\n")

    # -----------------------------------------------------------------
    # 3. Elbow sweep: k = 1 … 8
    # -----------------------------------------------------------------
    k_range = list(range(1, 9))
    inertias: list[float] = []

    print("Elbow sweep:")
    print(f"  {'k':>3}   {'inertia':>12}   {'delta':>12}")
    print("  " + "-" * 32)

    prev = None
    for k in k_range:
        r = kmeans(points, k=k, seed=42)
        inertias.append(r.inertia)
        delta = (r.inertia - prev) if prev is not None else float("nan")
        delta_str = f"{delta:>12.2f}" if not math.isnan(delta) else f"{'—':>12}"
        print(f"  {k:>3}   {r.inertia:>12.2f}   {delta_str}")
        prev = r.inertia

    # -----------------------------------------------------------------
    # 4. ASCII elbow plot
    # -----------------------------------------------------------------
    print("\nElbow plot (inertia vs k):\n")
    plot = ascii_elbow_plot(k_range, inertias, width=56, height=14)
    print(plot)

    # -----------------------------------------------------------------
    # 5. Per-iteration inertia convergence for k=4
    # -----------------------------------------------------------------
    print("\nPer-iteration inertia (k=4):")
    print(f"  {'iter':>5}   {'inertia':>12}")
    print("  " + "-" * 22)
    for i, val in enumerate(result.inertia_history, 1):
        print(f"  {i:>5}   {val:>12.4f}")

    print("\nDone.")


if __name__ == "__main__":
    main()
