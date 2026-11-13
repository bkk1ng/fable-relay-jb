"""
mp_pool.py — multiprocessing pipeline with a worker pool over chunks.

Features
--------
- Pool.imap_unordered with live progress bar written to stderr
- Per-task exception capture; tracebacks are collected and reported together
  at the end rather than stopping the pipeline on the first failure
- Graceful KeyboardInterrupt: pool.terminate() + pool.join() then re-raise
- __main__ demo on a CPU-bound function (sum-of-squares), with a second run
  that injects deliberate faults to exercise the error path
"""

from __future__ import annotations

import multiprocessing
import os
import sys
import time
import traceback
from dataclasses import dataclass
from typing import Any, Callable, Iterable


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------

@dataclass
class TaskResult:
    """Outcome of one chunk processed by the pipeline."""

    index: int   # position in the original iterable (order not preserved)
    chunk: Any   # the input value
    ok: bool
    value: Any = None  # populated on success
    tb: str = ""       # populated on failure; full traceback string


# ---------------------------------------------------------------------------
# Worker (module-level so it is picklable on all platforms)
# ---------------------------------------------------------------------------

def _worker_dispatch(payload: tuple) -> tuple:
    """
    Run in a worker process.

    Receives ``(index, chunk, fn)`` and returns
    ``(index, chunk, ok: bool, value_or_traceback)``.

    Catching the exception here keeps it local to this task.  imap_unordered
    will not see any exception from this call, so the remaining tasks keep
    running and all tracebacks are gathered by the main process.
    """
    index, chunk, fn = payload
    try:
        value = fn(chunk)
        return (index, chunk, True, value)
    except Exception:          # noqa: BLE001  — intentional broad catch
        return (index, chunk, False, traceback.format_exc())


# ---------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------

def run_pipeline(
    fn: Callable[[Any], Any],
    chunks: Iterable[Any],
    *,
    workers: int | None = None,
    pool_chunksize: int = 1,
    progress: bool = True,
) -> tuple[list[TaskResult], list[TaskResult]]:
    """
    Process *chunks* concurrently with *fn* across a multiprocessing pool.

    Parameters
    ----------
    fn:
        A picklable callable.  Receives one chunk, returns one value.
    chunks:
        Iterable of inputs.  Materialised into a list once so the total
        count is known for the progress display.
    workers:
        Pool size.  ``None`` uses ``os.cpu_count()``.
    pool_chunksize:
        Number of tasks shipped to each worker per round-trip.
    progress:
        Write a live progress bar to stderr while running.

    Returns
    -------
    (successes, failures)
        Two lists of :class:`TaskResult`.  Each failure carries the
        per-task traceback in ``TaskResult.tb``; they are also printed
        to stderr at the end.

    Raises
    ------
    KeyboardInterrupt
        Re-raised after the pool has been terminated and joined.
    """
    items = list(chunks)
    total = len(items)
    payloads = [(i, chunk, fn) for i, chunk in enumerate(items)]

    successes: list[TaskResult] = []
    failures: list[TaskResult] = []
    n_done = 0

    # --- progress helper ---------------------------------------------------

    def _report(done: int, label: str = "") -> None:
        if not progress:
            return
        bar_width = 32
        filled = int(bar_width * done / max(total, 1))
        bar = "█" * filled + "░" * (bar_width - filled)
        pct = 100 * done // max(total, 1)
        sys.stderr.write(
            f"\r  [{bar}] {pct:3d}%  {done}/{total}  {label:<28}"
        )
        sys.stderr.flush()

    _report(0)

    # --- pool setup and imap_unordered loop --------------------------------

    pool = multiprocessing.Pool(processes=workers)
    try:
        it = pool.imap_unordered(
            _worker_dispatch, payloads, chunksize=pool_chunksize
        )
        for index, chunk, ok, payload in it:
            n_done += 1
            if ok:
                successes.append(
                    TaskResult(index=index, chunk=chunk, ok=True, value=payload)
                )
                _report(n_done, f"ok   chunk={chunk!r}")
            else:
                failures.append(
                    TaskResult(index=index, chunk=chunk, ok=False, tb=payload)
                )
                _report(n_done, f"ERR  chunk={chunk!r}")

        pool.close()
        pool.join()

    except KeyboardInterrupt:
        pool.terminate()
        pool.join()
        if progress:
            sys.stderr.write("\n  interrupted — pool terminated\n")
            sys.stderr.flush()
        raise

    if progress:
        sys.stderr.write("\n")
        sys.stderr.flush()

    # --- report collected failures -----------------------------------------

    if failures:
        sep = "─" * 64
        print(f"\n{sep}", file=sys.stderr)
        print(f"  {len(failures)} task(s) failed:", file=sys.stderr)
        for r in failures:
            print(
                f"\n  chunk={r.chunk!r}  (task index {r.index})",
                file=sys.stderr,
            )
            for line in r.tb.splitlines():
                print(f"    {line}", file=sys.stderr)
        print(sep, file=sys.stderr)

    return successes, failures


# ---------------------------------------------------------------------------
# CPU-bound functions for the demo
# ---------------------------------------------------------------------------

def _sum_of_squares(n: int) -> int:
    """Sum of i² for i in range(n).  Pure CPU, deterministic."""
    return sum(i * i for i in range(n))


def _sum_of_squares_with_fault(n: int) -> int:
    """Same computation but raises deliberately for multiples of 7."""
    if n % 7 == 0:
        raise ValueError(f"deliberate fault injected for n={n}")
    return sum(i * i for i in range(n))


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Required on Windows / frozen executables to avoid recursive spawning.
    multiprocessing.freeze_support()

    CHUNKS = list(range(1, 51))                   # 50 chunks: n = 1 … 50
    WORKERS = min(4, os.cpu_count() or 1)

    print(f"Python  : {sys.version.split()[0]}")
    print(f"Workers : {WORKERS}")
    print(f"Chunks  : {len(CHUNKS)}  ({CHUNKS[0]}–{CHUNKS[-1]})")
    print()

    # ------------------------------------------------------------------
    # Run 1: all tasks succeed
    # ------------------------------------------------------------------
    print("Run 1 — no faults")
    t0 = time.perf_counter()
    ok1, err1 = run_pipeline(_sum_of_squares, CHUNKS, workers=WORKERS)
    elapsed1 = time.perf_counter() - t0
    print(
        f"  done in {elapsed1:.3f}s  |  "
        f"ok={len(ok1)}  err={len(err1)}"
    )

    # Spot-check a couple of results against the serial answer.
    for r in sorted(ok1, key=lambda x: x.index)[:3]:
        expected = sum(i * i for i in range(r.chunk))
        mark = "✓" if r.value == expected else "✗"
        print(f"  chunk={r.chunk:3d}  value={r.value}  {mark}")

    print()

    # ------------------------------------------------------------------
    # Run 2: tasks for multiples of 7 raise, rest succeed
    # ------------------------------------------------------------------
    print("Run 2 — faults injected for multiples of 7")
    t0 = time.perf_counter()
    ok2, err2 = run_pipeline(
        _sum_of_squares_with_fault, CHUNKS, workers=WORKERS
    )
    elapsed2 = time.perf_counter() - t0
    print(
        f"  done in {elapsed2:.3f}s  |  "
        f"ok={len(ok2)}  err={len(err2)}"
    )

    expected_err = sum(1 for n in CHUNKS if n % 7 == 0)
    match = "✓" if len(err2) == expected_err else "✗"
    print(f"  expected {expected_err} errors, got {len(err2)}  {match}")
