"""
mp_pool.py — multiprocessing pipeline with a worker pool, imap_unordered
progress reporting, per-task exception capture with tracebacks, and graceful
KeyboardInterrupt handling.

Python 3.12, stdlib only.

Usage
-----
    from mp_pool import run_pool

    def my_fn(item):
        ...

    successes, failures = run_pool(my_fn, items, workers=4)

    for r in failures:
        print(r.index, r.tb)
"""

from __future__ import annotations

import multiprocessing
import os
import sys
import time
import traceback
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Iterator


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------

@dataclass
class TaskResult:
    """
    Outcome of a single task.

    Attributes
    ----------
    index:
        Zero-based position of the item in the original input sequence.
    value:
        Return value of fn(item) on success; None on failure.
    tb:
        Formatted traceback string on failure; None on success.
    """

    index: int
    value: Any = None
    tb: str | None = None

    @property
    def ok(self) -> bool:
        """True when the task completed without an exception."""
        return self.tb is None


# ---------------------------------------------------------------------------
# Worker process state (module-level so multiprocessing can pickle it)
# ---------------------------------------------------------------------------

_WORKER_FN: Callable[[Any], Any] | None = None


def _init_worker(fn: Callable[[Any], Any]) -> None:
    """Pool initializer: store *fn* once per worker process."""
    global _WORKER_FN
    _WORKER_FN = fn


def _worker(packed: tuple[int, Any]) -> TaskResult:
    """
    Apply the pool-registered function to one item.

    Never raises — any exception is caught and stored as a formatted traceback
    string in the returned TaskResult so the main process can collect them all.
    """
    index, item = packed
    fn = _WORKER_FN
    if fn is None:
        return TaskResult(
            index=index,
            tb="Worker not initialised — _WORKER_FN is None.\n",
        )
    try:
        return TaskResult(index=index, value=fn(item))
    except Exception:
        return TaskResult(index=index, tb=traceback.format_exc())


# ---------------------------------------------------------------------------
# Progress bar
# ---------------------------------------------------------------------------

_BAR_WIDTH = 40


def _render_progress(done: int, total: int) -> str:
    pct = done / total * 100
    filled = round(_BAR_WIDTH * done / total)
    bar = "█" * filled + "░" * (_BAR_WIDTH - filled)
    width = len(str(total))
    return f"\r  [{bar}] {done:>{width}}/{total}  {pct:5.1f}%"


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def run_pool(
    fn: Callable[[Any], Any],
    items: Iterable[Any],
    *,
    workers: int | None = None,
    chunksize: int = 1,
    progress: bool = True,
) -> tuple[list[TaskResult], list[TaskResult]]:
    """
    Process *items* in parallel using a multiprocessing worker pool.

    *fn* is called exactly once per item in a worker process.  Exceptions
    inside *fn* do **not** abort the pool — they are captured per task and
    returned in the *failures* list so the caller can inspect every traceback
    after all work has finished.

    Parameters
    ----------
    fn:
        A picklable callable applied to each item.  It must be importable
        from a worker process, so it should be a module-level function.
    items:
        Any iterable.  Materialised into a list once to obtain the total count
        for progress reporting.
    workers:
        Number of worker processes.  Defaults to ``os.cpu_count()`` (or 1 if
        that returns *None*).
    chunksize:
        ``imap_unordered`` batch size per IPC round-trip.  Larger values reduce
        per-task IPC overhead but coarsen progress granularity.
    progress:
        When *True*, write a live progress bar to *stderr*.

    Returns
    -------
    successes, failures
        Both lists contain :class:`TaskResult` objects in completion order
        (not submission order, because ``imap_unordered`` is used).  Items in
        *failures* carry a full ``traceback.format_exc()`` string in ``.tb``.

    KeyboardInterrupt
        The pool is terminated immediately, its workers are reaped, and the
        partial results accumulated so far are returned.  The caller can detect
        this case by checking ``len(successes) + len(failures) < total``.
    """
    tasks: list[tuple[int, Any]] = list(enumerate(items))
    total = len(tasks)
    successes: list[TaskResult] = []
    failures: list[TaskResult] = []

    if total == 0:
        return successes, failures

    n_workers = workers if workers is not None else (os.cpu_count() or 1)

    pool = multiprocessing.Pool(
        processes=n_workers,
        initializer=_init_worker,
        initargs=(fn,),
    )
    try:
        result_iter: Iterator[TaskResult] = pool.imap_unordered(
            _worker, tasks, chunksize=chunksize
        )
        for done_count, result in enumerate(result_iter, start=1):
            (successes if result.ok else failures).append(result)
            if progress:
                print(_render_progress(done_count, total),
                      end="", flush=True, file=sys.stderr)
    except KeyboardInterrupt:
        pool.terminate()
        pool.join()
        if progress:
            print(file=sys.stderr)  # newline after the partial bar
        print(
            "\nKeyboardInterrupt — pool terminated. "
            f"Partial results: {len(successes)} ok, {len(failures)} failed "
            f"(of {total} total).",
            file=sys.stderr,
        )
        return successes, failures
    else:
        pool.close()
        pool.join()

    if progress:
        print(file=sys.stderr)  # final newline after the completed bar

    return successes, failures


# ---------------------------------------------------------------------------
# Demo helpers — module-level so multiprocessing can pickle them
# ---------------------------------------------------------------------------

def _sum_of_squares(n: int) -> int:
    """Return sum(i² for i in range(n)).  CPU-bound for large n."""
    return sum(i * i for i in range(n))


def _sum_of_squares_faulty(n: int) -> int:
    """Like _sum_of_squares but raises ValueError when n is divisible by 7."""
    if n % 7 == 0:
        raise ValueError(f"n={n} is divisible by 7 — refusing")
    return sum(i * i for i in range(n))


# ---------------------------------------------------------------------------
# __main__ demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # 60 tasks; each iterates ~50 000 times — clearly CPU-bound.
    # _sum_of_squares_faulty raises on multiples of 7 so we can see error
    # collection in action.
    CHUNKS = list(range(50_000, 50_060))
    WORKERS = max(1, (os.cpu_count() or 2) - 1)

    print(f"Pool workers : {WORKERS}")
    print(f"Tasks        : {len(CHUNKS)}")
    print(f"Function     : _sum_of_squares_faulty  (raises when n % 7 == 0)")
    print()

    t0 = time.perf_counter()
    ok, err = run_pool(
        _sum_of_squares_faulty,
        CHUNKS,
        workers=WORKERS,
        chunksize=4,
    )
    elapsed = time.perf_counter() - t0

    print(
        f"\nFinished in {elapsed:.2f}s — "
        f"{len(ok)} succeeded, {len(err)} failed\n"
    )

    if err:
        sep = "─" * 64
        print(sep)
        print(f"Collected tracebacks ({len(err)} failure(s)):\n")
        for r in sorted(err, key=lambda x: x.index):
            n = CHUNKS[r.index]
            print(f"  Task #{r.index}  n={n}")
            assert r.tb is not None
            for line in r.tb.rstrip().splitlines():
                print(f"    {line}")
            print()
        print(sep)

    # Verify a couple of successful results match the pure-Python formula.
    # sum(i² for i in range(n)) == n*(n-1)*(2n-1) // 6
    mismatches = [
        r for r in ok
        if r.value != (CHUNKS[r.index] * (CHUNKS[r.index] - 1) * (2 * CHUNKS[r.index] - 1)) // 6
    ]
    if mismatches:
        print(f"WARNING: {len(mismatches)} result(s) failed the sanity check!")
    else:
        print(f"All {len(ok)} successful results passed the closed-form sanity check.")
