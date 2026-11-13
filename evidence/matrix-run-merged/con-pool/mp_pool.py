"""
mp_pool.py — multiprocessing pipeline with worker pool, progress reporting,
per-task exception capture, graceful KeyboardInterrupt, and a __main__ demo.

Python 3.12, stdlib only.
"""

from __future__ import annotations

import multiprocessing as mp
import traceback
import sys
import time
import math
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import Any, TypeVar

T = TypeVar("T")
R = TypeVar("R")


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------

@dataclass
class TaskResult:
    """Holds the outcome of a single worker task."""
    index: int
    input: Any
    output: Any = None
    error: Exception | None = None
    tb: str = ""          # formatted traceback when error is set

    @property
    def ok(self) -> bool:
        return self.error is None


# ---------------------------------------------------------------------------
# Worker (runs in child process)
# ---------------------------------------------------------------------------

def _worker(args: tuple[int, Any, Callable]) -> TaskResult:
    """
    Unpack (index, item, fn), call fn(item), return a TaskResult.
    Exceptions are caught and serialised so the parent can collect them all
    rather than stopping at the first failure.
    """
    index, item, fn = args
    try:
        output = fn(item)
        return TaskResult(index=index, input=item, output=output)
    except Exception as exc:  # noqa: BLE001
        return TaskResult(
            index=index,
            input=item,
            error=exc,
            tb=traceback.format_exc(),
        )


# ---------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------

def run_pipeline(
    fn: Callable[[Any], Any],
    items: Iterable[Any],
    *,
    workers: int | None = None,
    chunksize: int = 1,
    progress: bool = True,
) -> list[TaskResult]:
    """
    Map *fn* over *items* in a worker pool.

    Parameters
    ----------
    fn:
        CPU-bound function to apply to each item.
    items:
        Iterable of inputs; materialised once to know the total count.
    workers:
        Pool size; defaults to os.cpu_count().
    chunksize:
        imap_unordered chunk size (tune for items >> workers).
    progress:
        Print a simple progress line to stderr.

    Returns
    -------
    List of TaskResult in original index order.  Tasks that raised are
    present with .ok == False and a full traceback in .tb.
    """
    item_list = list(items)
    total = len(item_list)
    # Package each item with its index and the target function so the
    # top-level worker function is picklable on all platforms.
    work = [(i, item, fn) for i, item in enumerate(item_list)]

    results: list[TaskResult] = [None] * total  # type: ignore[list-item]
    done = 0

    try:
        with mp.Pool(processes=workers) as pool:
            for result in pool.imap_unordered(_worker, work, chunksize=chunksize):
                results[result.index] = result
                done += 1
                if progress:
                    pct = done / total * 100
                    bar_len = 30
                    filled = int(bar_len * done // total)
                    bar = "█" * filled + "░" * (bar_len - filled)
                    print(
                        f"\r  [{bar}] {done}/{total}  ({pct:5.1f}%)",
                        end="",
                        flush=True,
                        file=sys.stderr,
                    )
    except KeyboardInterrupt:
        # Pool.__exit__ calls pool.terminate() then pool.join(), so children
        # are reaped before we surface the interrupt to the caller.
        print("\n  Interrupted — pool terminated.", file=sys.stderr)
        raise

    if progress:
        print(file=sys.stderr)  # newline after the progress bar

    return results


# ---------------------------------------------------------------------------
# Error reporting helper
# ---------------------------------------------------------------------------

def report_errors(results: list[TaskResult]) -> int:
    """
    Print collected tracebacks for failed tasks.

    Returns the number of failures (0 → all good).
    """
    failures = [r for r in results if not r.ok]
    if not failures:
        return 0

    print(f"\n{'─' * 60}", file=sys.stderr)
    print(f"  {len(failures)} task(s) failed:", file=sys.stderr)
    for r in failures:
        print(f"\n  ── task {r.index!r} (input={r.input!r}) ──", file=sys.stderr)
        # Indent the traceback for readability
        for line in r.tb.splitlines():
            print(f"    {line}", file=sys.stderr)
    print(f"{'─' * 60}", file=sys.stderr)
    return len(failures)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def _cpu_bound(n: int) -> float:
    """
    Intentionally CPU-heavy: sum of log(k) for k in [1, n].
    Raises ValueError for non-positive inputs so we can demo error capture.
    """
    if n <= 0:
        raise ValueError(f"Input must be positive, got {n!r}")
    return sum(math.log(k) for k in range(1, n + 1))


def main() -> None:
    # Mix of valid inputs and one deliberate bad value to show error capture.
    inputs = [500_000, 300_000, -1, 400_000, 0, 600_000, 200_000, 700_000]

    workers = max(2, (mp.cpu_count() or 2) - 1)
    print(f"Pool size : {workers}")
    print(f"Tasks     : {len(inputs)}")
    print(f"Inputs    : {inputs}\n")

    t0 = time.perf_counter()
    try:
        results = run_pipeline(_cpu_bound, inputs, workers=workers, chunksize=2)
    except KeyboardInterrupt:
        print("Aborted by user.", file=sys.stderr)
        sys.exit(130)
    elapsed = time.perf_counter() - t0

    # Summarise successes
    ok = [r for r in results if r.ok]
    print(f"\nCompleted in {elapsed:.2f}s  —  {len(ok)}/{len(results)} succeeded\n")
    for r in ok:
        print(f"  [{r.index}] log-sum({r.input:,}) = {r.output:.4f}")

    # Collect and display failures
    n_failed = report_errors(results)
    sys.exit(1 if n_failed else 0)


if __name__ == "__main__":
    # Required on Windows / macOS (spawn start method) to avoid recursive
    # child spawning when the module is imported by worker subprocesses.
    mp.freeze_support()
    main()
