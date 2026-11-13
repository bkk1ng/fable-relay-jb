"""
mp_pool.py — multiprocessing pipeline: worker pool over chunks,
imap_unordered progress reporting, per-task exception capture with
tracebacks collected at the end, graceful KeyboardInterrupt that
terminates children, and a __main__ demo on a CPU-bound function.

Python 3.12, stdlib only.
"""

from __future__ import annotations

import multiprocessing
import os
import sys
import time
import traceback
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import Any


# ---------------------------------------------------------------------------
# Result containers
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class TaskOK:
    """A task that completed without error."""

    index: int   # original submission index
    input: Any
    result: Any


@dataclass(slots=True)
class TaskError:
    """A task that raised an exception; tb holds the formatted traceback."""

    index: int
    input: Any
    tb: str


@dataclass
class PipelineResult:
    """Aggregated results returned by run_pipeline()."""

    successes: list[TaskOK] = field(default_factory=list)
    errors: list[TaskError] = field(default_factory=list)

    @property
    def n_ok(self) -> int:
        return len(self.successes)

    @property
    def n_err(self) -> int:
        return len(self.errors)

    def print_errors(self, *, file=sys.stderr) -> None:
        """Pretty-print every collected traceback."""
        if not self.errors:
            return
        print("─" * 60, file=file)
        for err in self.errors:
            print(
                f"Task #{err.index}  input={err.input!r}\n{err.tb}",
                file=file,
            )
        print("─" * 60, file=file)


# ---------------------------------------------------------------------------
# Worker shim — must be a module-level function so pickle can find it
# on all platforms (fork, spawn, forkserver).
# ---------------------------------------------------------------------------


def _worker_shim(
    packed: tuple[int, Any, Callable[[Any], Any]],
) -> tuple[int, Any, bool, Any]:
    """
    Run func(item) in the child process.

    Returns (index, item, ok, payload) where payload is the return value on
    success or a formatted traceback string on failure.  Exceptions never
    propagate back through imap_unordered; they are captured as values so
    every task is accounted for regardless of individual failures.
    """
    index, item, func = packed
    try:
        return index, item, True, func(item)
    except Exception:
        return index, item, False, traceback.format_exc()


# ---------------------------------------------------------------------------
# Progress helper
# ---------------------------------------------------------------------------


def _print_progress(done: int, total: int, n_err: int) -> None:
    if total == 0:
        return
    pct = done / total * 100
    width = 32
    filled = round(width * done / total)
    bar = "█" * filled + "░" * (width - filled)
    err_tag = f"  err={n_err}" if n_err else ""
    print(
        f"\r[{bar}] {done:>{len(str(total))}}/{total}  {pct:5.1f}%{err_tag}",
        end="",
        flush=True,
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def run_pipeline(
    func: Callable[[Any], Any],
    items: Iterable[Any],
    *,
    workers: int | None = None,
    chunksize: int = 1,
    progress: bool = True,
) -> PipelineResult:
    """
    Run *func* over every item in *items* using a multiprocessing pool.

    Parameters
    ----------
    func:
        CPU-bound callable.  Must be importable (defined at module top level
        or via functools.partial of a top-level function); lambdas fail on
        spawn-based start methods.
    items:
        Input iterable.  Consumed eagerly upfront so the total is known for
        the progress bar.
    workers:
        Pool size.  Defaults to os.cpu_count().
    chunksize:
        Passed to imap_unordered.  Increase for many cheap tasks; keep at 1
        for a small number of expensive tasks where load balance matters.
    progress:
        Write a live progress bar to stderr.

    Returns
    -------
    PipelineResult
        .successes — TaskOK list in completion order (not submission order).
        .errors    — TaskError list; each .tb is a full formatted traceback.

        Sort by .index if deterministic ordering is required downstream.

    KeyboardInterrupt
        Caught internally.  The pool is terminated, children are joined, and
        a partial PipelineResult is returned covering tasks completed so far.
    """
    items_list = list(items)
    total = len(items_list)
    n_workers = workers if workers is not None else (os.cpu_count() or 1)
    work = [(i, item, func) for i, item in enumerate(items_list)]

    result = PipelineResult()
    done = 0

    pool = multiprocessing.Pool(processes=n_workers)
    try:
        for index, item, ok, payload in pool.imap_unordered(
            _worker_shim, work, chunksize=chunksize
        ):
            done += 1
            if ok:
                result.successes.append(TaskOK(index=index, input=item, result=payload))
            else:
                result.errors.append(TaskError(index=index, input=item, tb=payload))

            if progress:
                _print_progress(done, total, result.n_err)

    except KeyboardInterrupt:
        pool.terminate()
        pool.join()
        if progress:
            # End the progress line cleanly before the interrupt message.
            print(file=sys.stderr)
        print(
            f"\n[pipeline] KeyboardInterrupt — pool terminated. "
            f"{done}/{total} tasks finished before interrupt.",
            file=sys.stderr,
        )
        return result

    else:
        pool.close()
        pool.join()

    if progress:
        print(file=sys.stderr)  # newline after the final progress line

    return result


# ---------------------------------------------------------------------------
# Demo CPU-bound work
# ---------------------------------------------------------------------------


def _collatz_steps(n: int) -> int:
    """Steps to reach 1 under the Collatz conjecture."""
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps


def heavy_collatz(n: int) -> int:
    """
    Sum of Collatz step counts for all integers 1..n.

    Intentionally O(n²) so each task is meaningfully CPU-bound.
    Raises ValueError for n <= 0 and RuntimeError for n == 666 to
    demonstrate per-task error capture.
    """
    if n <= 0:
        raise ValueError(f"n must be a positive integer, got {n!r}")
    if n == 666:
        raise RuntimeError("Demo error: n=666 is cursed")
    return sum(_collatz_steps(k) for k in range(1, n + 1))


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


if __name__ == "__main__":
    # Required on Windows and macOS (spawn start method).
    multiprocessing.freeze_support()

    # Build the input list: integers 1..600 plus a deliberate bad value.
    inputs: list[int] = list(range(1, 601)) + [666, -1]
    n_total = len(inputs)
    cpus = os.cpu_count() or 1

    print(f"Pipeline demo — {n_total} tasks, {cpus} logical CPUs")
    print("Press Ctrl-C at any time to test graceful termination.\n")

    t0 = time.perf_counter()
    report = run_pipeline(heavy_collatz, inputs, chunksize=8)
    elapsed = time.perf_counter() - t0

    # Summary
    print(f"\nCompleted in {elapsed:.2f}s")
    print(f"  Successes : {report.n_ok}")
    print(f"  Errors    : {report.n_err}")

    # Print every collected traceback.
    if report.errors:
        print("\nCaptured error tracebacks:")
        report.print_errors(file=sys.stderr)

    # Spot-check: five inputs with the highest Collatz sums.
    if report.successes:
        top = sorted(report.successes, key=lambda t: t.result, reverse=True)[:5]
        print("\nTop 5 results (highest sum-of-steps):")
        for t in top:
            print(f"  n={t.input!r:>5}  sum={t.result:>14,}")
