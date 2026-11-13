"""
mp_pool.py — multiprocessing pipeline with worker pool, imap_unordered progress
reporting, per-task exception capture, graceful KeyboardInterrupt, and a __main__
demo on a CPU-bound function.

Python 3.12, stdlib only.
"""

from __future__ import annotations

import multiprocessing as mp
import traceback
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
    """Holds either a successful return value or a captured exception + traceback."""

    index: int
    input: Any
    value: Any = None
    error: BaseException | None = None
    tb: str = field(default="", repr=False)

    @property
    def ok(self) -> bool:
        return self.error is None


# ---------------------------------------------------------------------------
# Worker shim (must be module-level so it is picklable on all platforms)
# ---------------------------------------------------------------------------

def _worker_shim(args: tuple[int, Any, Callable]) -> TaskResult:
    """
    Unpack (index, item, fn), call fn(item), and return a TaskResult.
    Any exception is caught and recorded; the worker process itself never crashes.
    """
    index, item, fn = args
    try:
        return TaskResult(index=index, input=item, value=fn(item))
    except Exception as exc:                        # noqa: BLE001
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
    fn: Callable[[T], R],
    items: Iterable[T],
    *,
    workers: int | None = None,
    chunksize: int = 1,
    progress: bool = True,
) -> list[TaskResult]:
    """
    Run ``fn`` over every item in ``items`` using a multiprocessing pool.

    Parameters
    ----------
    fn:
        A picklable callable applied to each item.
    items:
        Any iterable of inputs.  Consumed once; the full sequence is not
        buffered into memory before submission.
    workers:
        Pool size.  Defaults to ``os.cpu_count()``.
    chunksize:
        Passed to ``imap_unordered``; larger values reduce IPC overhead for
        cheap tasks.
    progress:
        Print a one-line progress counter to stdout while work is running.

    Returns
    -------
    list[TaskResult]
        One entry per input item, in the **original** order (sorted by index
        before returning).  Check ``result.ok`` to distinguish success from
        failure.

    Notes
    -----
    * Per-task exceptions are *captured*, not re-raised; callers receive a
      ``TaskResult`` with ``error`` set and a full ``tb`` string.
    * ``KeyboardInterrupt`` cancels remaining work immediately; already-finished
      results are still returned so the caller can inspect partial output.
    """
    items_list = list(items)
    total = len(items_list)
    work = [(i, item, fn) for i, item in enumerate(items_list)]

    results: list[TaskResult] = []

    try:
        with mp.Pool(processes=workers) as pool:
            imap = pool.imap_unordered(_worker_shim, work, chunksize=chunksize)
            completed = 0
            try:
                for result in imap:
                    results.append(result)
                    completed += 1
                    if progress:
                        print(
                            f"\r  progress: {completed}/{total}",
                            end="",
                            flush=True,
                        )
            except KeyboardInterrupt:
                # Stop issuing new tasks and let already-running children finish
                # their current unit of work, then terminate.
                pool.terminate()
                pool.join()
                if progress:
                    print(f"\n  interrupted after {completed}/{total} tasks")
                # Fall through and return whatever we collected.
            else:
                if progress:
                    print()  # newline after the \r line
    except KeyboardInterrupt:
        # Interrupt before the pool was fully set up.
        if progress:
            print("\n  interrupted during pool startup")

    # Restore original ordering so callers can zip results with inputs.
    results.sort(key=lambda r: r.index)
    return results


# ---------------------------------------------------------------------------
# Convenience: collect failures and print a summary
# ---------------------------------------------------------------------------

def report_errors(results: list[TaskResult]) -> int:
    """
    Print the captured traceback for every failed task.

    Returns the number of failures (0 means everything succeeded).
    """
    failures = [r for r in results if not r.ok]
    if not failures:
        return 0

    print(f"\n{len(failures)} task(s) failed:\n")
    for r in failures:
        print(f"  --- task index={r.index}  input={r.input!r} ---")
        print(r.tb)
    return len(failures)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def _cpu_bound(n: int) -> int:
    """
    Intentionally CPU-heavy: count integers up to ``n`` with a tight loop so
    the GIL-release advantage of multiprocessing is visible even at modest N.
    """
    total = 0
    for i in range(n):
        total += i * i
    return total


def _demo() -> None:
    import time

    # Mix of normal inputs and one that deliberately raises, so error capture
    # is exercised without requiring a real buggy function.
    inputs: list[int | str] = [
        500_000,
        1_000_000,
        "boom",        # will cause TypeError inside _cpu_bound
        1_500_000,
        2_000_000,
        250_000,
        "also_boom",   # second intentional failure
        750_000,
    ]

    print(f"Running pipeline over {len(inputs)} items …")
    t0 = time.perf_counter()

    results = run_pipeline(
        _cpu_bound,
        inputs,
        workers=min(4, mp.cpu_count()),
        chunksize=1,
        progress=True,
    )

    elapsed = time.perf_counter() - t0
    print(f"  finished in {elapsed:.3f}s\n")

    print("Results (original order):")
    for r in results:
        if r.ok:
            print(f"  [{r.index}] _cpu_bound({r.input!r:>10}) = {r.value}")
        else:
            print(f"  [{r.index}] _cpu_bound({r.input!r:>10}) → ERROR: {r.error}")

    n_failed = report_errors(results)
    print(f"\nSummary: {len(results) - n_failed} succeeded, {n_failed} failed.")


if __name__ == "__main__":
    # Required on Windows / macOS (spawn start method) to prevent recursive
    # worker spawning when the module is imported by child processes.
    mp.freeze_support()
    _demo()
