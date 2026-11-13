"""
mp_pool.py — multiprocessing pipeline with worker pool, imap_unordered progress,
per-task exception capture, graceful KeyboardInterrupt, and a __main__ demo.

Python 3.12, stdlib only.
"""

from __future__ import annotations

import multiprocessing as mp
import os
import sys
import time
import traceback
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, TypeVar

T = TypeVar("T")


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------

@dataclass
class TaskResult:
    """Holds the outcome of a single chunk task."""
    index: int
    value: Any = None
    error: str | None = None   # formatted traceback string when the task failed

    @property
    def ok(self) -> bool:
        return self.error is None


# ---------------------------------------------------------------------------
# Worker (module-level so it is picklable on all platforms)
# ---------------------------------------------------------------------------

def _worker(args: tuple[int, Any]) -> TaskResult:
    """
    Unpack (index, item), call the registered task function, and catch any
    exception — returning it as a formatted traceback rather than re-raising,
    so the pool never sees an unhandled worker exception.
    """
    index, item = args
    try:
        result = _worker._fn(item)          # type: ignore[attr-defined]
        return TaskResult(index=index, value=result)
    except Exception:
        return TaskResult(index=index, error=traceback.format_exc())


def _init_worker(fn: Callable) -> None:
    """Pool initializer: stash the task function in the worker process."""
    _worker._fn = fn                        # type: ignore[attr-defined]


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
) -> tuple[list[TaskResult], list[TaskResult]]:
    """
    Run *fn* over *items* in a worker pool.

    Parameters
    ----------
    fn:        Task function.  Must be picklable (module-level or top-level).
    items:     Iterable of inputs; consumed into a list upfront for length tracking.
    workers:   Pool size.  Defaults to os.cpu_count().
    chunksize: imap_unordered chunksize hint.
    progress:  Print a progress line to stderr.

    Returns
    -------
    (successes, failures) — two lists of TaskResult objects.
    """
    item_list = list(items)
    total = len(item_list)
    indexed = list(enumerate(item_list))

    n_workers = workers or os.cpu_count() or 1
    successes: list[TaskResult] = []
    failures: list[TaskResult] = []

    try:
        with mp.Pool(
            processes=n_workers,
            initializer=_init_worker,
            initargs=(fn,),
        ) as pool:
            try:
                for done, result in enumerate(
                    pool.imap_unordered(_worker, indexed, chunksize=chunksize),
                    start=1,
                ):
                    if result.ok:
                        successes.append(result)
                    else:
                        failures.append(result)

                    if progress:
                        pct = done / total * 100
                        label = "ok" if result.ok else "ERR"
                        print(
                            f"\r  [{label}] {done}/{total}  ({pct:.1f}%)",
                            end="",
                            flush=True,
                            file=sys.stderr,
                        )

            except KeyboardInterrupt:
                print("\n  KeyboardInterrupt — terminating pool …", file=sys.stderr)
                pool.terminate()
                pool.join()
                return successes, failures

    finally:
        if progress:
            print(file=sys.stderr)   # newline after progress line

    return successes, failures


# ---------------------------------------------------------------------------
# Reporting helper
# ---------------------------------------------------------------------------

def report(successes: list[TaskResult], failures: list[TaskResult]) -> None:
    """Print a summary and, for failures, the captured tracebacks."""
    total = len(successes) + len(failures)
    print(f"\nResults: {len(successes)}/{total} succeeded, {len(failures)} failed.")

    if failures:
        print(f"\n{'─' * 60}")
        print(f"  {len(failures)} task(s) raised exceptions:")
        for f in sorted(failures, key=lambda r: r.index):
            print(f"\n  ── task index {f.index} ──")
            # Indent each line of the traceback for readability
            for line in (f.error or "").splitlines():
                print(f"    {line}")
        print("─" * 60)
    else:
        print("  All tasks completed without errors.")


# ---------------------------------------------------------------------------
# Demo — CPU-bound function
# ---------------------------------------------------------------------------

def _cpu_task(n: int) -> int:
    """
    A deliberately CPU-bound task: sum of divisors of *n*.
    Raises for multiples of 7 so we can exercise the error path.
    """
    if n % 7 == 0:
        raise ValueError(f"refusing to process a multiple of 7: {n}")
    return sum(i for i in range(1, n + 1) if n % i == 0)


def _demo() -> None:
    inputs = list(range(1, 33))          # 32 chunks; a few are multiples of 7
    n_workers = min(4, os.cpu_count() or 1)

    print(f"Demo: sum-of-divisors over {len(inputs)} inputs "
          f"with {n_workers} workers.\n"
          f"  (Items that are multiples of 7 intentionally raise.)\n")

    t0 = time.perf_counter()
    successes, failures = run_pipeline(
        _cpu_task,
        inputs,
        workers=n_workers,
        chunksize=4,
        progress=True,
    )
    elapsed = time.perf_counter() - t0

    report(successes, failures)
    print(f"\nWall time: {elapsed:.3f}s")

    # Show a sample of successful results (sorted by input index)
    sample = sorted(successes, key=lambda r: r.index)[:8]
    print("\nSample results (index → value):")
    for r in sample:
        print(f"  inputs[{r.index}] = {inputs[r.index]}  →  {r.value}")


if __name__ == "__main__":
    # Required on Windows / macOS (spawn start method)
    mp.freeze_support()
    _demo()
