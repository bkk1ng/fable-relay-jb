"""
Async worker pool — Python 3.12, stdlib only.

Features
--------
- Bounded queue (backpressure on producers)
- N concurrent workers
- Per-task timeout
- Exponential-backoff retries with jitter
- Result collection (success + failure)
- Clean cancellation on shutdown
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
import random
import time
from collections.abc import Awaitable, Callable
from typing import Any, Generic, TypeVar

log = logging.getLogger(__name__)

T = TypeVar("T")

# Sentinel placed on the queue to tell workers to stop.
_STOP = object()


@dataclasses.dataclass
class TaskResult(Generic[T]):
    task_id: int
    value: T | None = None
    error: Exception | None = None
    attempts: int = 1
    elapsed: float = 0.0

    @property
    def ok(self) -> bool:
        return self.error is None


@dataclasses.dataclass
class PoolStats:
    succeeded: int = 0
    failed: int = 0
    total_elapsed: float = 0.0

    @property
    def total(self) -> int:
        return self.succeeded + self.failed


class WorkerPool(Generic[T]):
    """
    Bounded async worker pool.

    Parameters
    ----------
    coro_fn:
        Async callable that accepts a single task payload and returns T.
    n_workers:
        Number of concurrent workers.
    queue_size:
        Maximum items in the queue before ``submit`` blocks.
    task_timeout:
        Seconds before a single attempt is cancelled (None = no limit).
    max_retries:
        Maximum number of *retries* (0 = one attempt, no retries).
    backoff_base:
        Initial backoff in seconds; doubles each retry.
    backoff_max:
        Cap on the computed backoff before jitter.
    jitter:
        Maximum random seconds added to each backoff interval.
    """

    def __init__(
        self,
        coro_fn: Callable[[Any], Awaitable[T]],
        *,
        n_workers: int = 4,
        queue_size: int = 100,
        task_timeout: float | None = 10.0,
        max_retries: int = 3,
        backoff_base: float = 0.5,
        backoff_max: float = 30.0,
        jitter: float = 0.5,
    ) -> None:
        self._coro_fn = coro_fn
        self._n_workers = n_workers
        self._task_timeout = task_timeout
        self._max_retries = max_retries
        self._backoff_base = backoff_base
        self._backoff_max = backoff_max
        self._jitter = jitter

        self._queue: asyncio.Queue[Any] = asyncio.Queue(maxsize=queue_size)
        self._results: list[TaskResult[T]] = []
        self._workers: list[asyncio.Task[None]] = []
        self._next_id = 0
        self._running = False

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    async def start(self) -> None:
        """Spawn workers. Call before submitting tasks."""
        if self._running:
            raise RuntimeError("Pool is already running.")
        self._running = True
        self._workers = [
            asyncio.create_task(self._worker(i), name=f"worker-{i}")
            for i in range(self._n_workers)
        ]
        log.debug("Started %d workers.", self._n_workers)

    async def submit(self, payload: Any) -> int:
        """
        Enqueue a task. Blocks if the queue is full (backpressure).

        Returns the task id assigned to this payload.
        """
        if not self._running:
            raise RuntimeError("Pool is not running. Call start() first.")
        task_id = self._next_id
        self._next_id += 1
        await self._queue.put((task_id, payload))
        return task_id

    async def shutdown(self, *, wait: bool = True) -> None:
        """
        Signal workers to stop.

        If *wait* is True, drain the queue and wait for all workers to
        finish before returning. If False, cancel workers immediately.
        """
        if not self._running:
            return
        self._running = False

        if wait:
            # One sentinel per worker; they'll each consume exactly one.
            for _ in self._workers:
                await self._queue.put(_STOP)
            await asyncio.gather(*self._workers, return_exceptions=True)
        else:
            for w in self._workers:
                w.cancel()
            await asyncio.gather(*self._workers, return_exceptions=True)

        log.debug("All workers stopped.")

    @property
    def results(self) -> list[TaskResult[T]]:
        return list(self._results)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    async def _worker(self, worker_id: int) -> None:
        log.debug("Worker %d started.", worker_id)
        while True:
            item = await self._queue.get()
            if item is _STOP:
                self._queue.task_done()
                break

            task_id, payload = item
            result = await self._run_with_retries(task_id, payload)
            self._results.append(result)
            self._queue.task_done()

        log.debug("Worker %d exiting.", worker_id)

    async def _run_with_retries(self, task_id: int, payload: Any) -> TaskResult[T]:
        attempts = 0
        t0 = time.monotonic()

        while True:
            attempts += 1
            try:
                value = await self._attempt(payload)
                elapsed = time.monotonic() - t0
                log.debug(
                    "Task %d succeeded on attempt %d (%.3fs).",
                    task_id, attempts, elapsed,
                )
                return TaskResult(
                    task_id=task_id,
                    value=value,
                    attempts=attempts,
                    elapsed=elapsed,
                )
            except Exception as exc:
                retries_left = self._max_retries - (attempts - 1)
                if retries_left <= 0:
                    elapsed = time.monotonic() - t0
                    log.warning(
                        "Task %d failed permanently after %d attempt(s): %s",
                        task_id, attempts, exc,
                    )
                    return TaskResult(
                        task_id=task_id,
                        error=exc,
                        attempts=attempts,
                        elapsed=elapsed,
                    )

                delay = min(
                    self._backoff_base * (2 ** (attempts - 1)),
                    self._backoff_max,
                ) + random.uniform(0, self._jitter)
                log.debug(
                    "Task %d attempt %d failed (%s). Retrying in %.2fs.",
                    task_id, attempts, exc, delay,
                )
                await asyncio.sleep(delay)

    async def _attempt(self, payload: Any) -> T:
        """Run the coroutine, applying per-task timeout if configured."""
        coro = self._coro_fn(payload)
        if self._task_timeout is not None:
            return await asyncio.wait_for(coro, timeout=self._task_timeout)
        return await coro


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

async def _demo() -> None:
    import sys

    logging.basicConfig(
        level=logging.WARNING,
        format="%(asctime)s  %(levelname)-8s  %(message)s",
        stream=sys.stdout,
    )

    N_JOBS = 50
    N_WORKERS = 6
    QUEUE_SIZE = 20
    TASK_TIMEOUT = 2.0   # seconds per attempt
    MAX_RETRIES = 2       # up to 3 total attempts
    BACKOFF_BASE = 0.1
    BACKOFF_MAX = 1.0
    JITTER = 0.05

    # ---- simulated workload -------------------------------------------
    # Each job has a random duration 0.05-0.4 s.
    # ~20 % of jobs raise on the first attempt (recoverable).
    # ~5 % raise on every attempt (permanent failure).
    # ~3 % sleep longer than TASK_TIMEOUT (timeout failure).

    async def process_job(job: dict) -> dict:
        job_id: int = job["id"]
        duration: float = job["duration"]
        fail_first: bool = job["fail_first"]
        always_fail: bool = job["always_fail"]
        timeout_job: bool = job["timeout_job"]

        if timeout_job:
            await asyncio.sleep(TASK_TIMEOUT + 1.0)   # will time out
            return {}

        if always_fail:
            raise RuntimeError(f"Job {job_id}: permanent failure")

        # Recoverable: fail on first call, succeed on retry.
        # We track "has been attempted" via a mutable cell in the closure.
        if fail_first and not job.get("_retried"):
            job["_retried"] = True
            raise ValueError(f"Job {job_id}: transient failure (will retry)")

        await asyncio.sleep(duration)
        return {"job_id": job_id, "result": job_id * 2}

    rng = random.Random(42)
    jobs = [
        {
            "id": i,
            "duration": rng.uniform(0.05, 0.4),
            "fail_first": rng.random() < 0.20,
            "always_fail": rng.random() < 0.05,
            "timeout_job": rng.random() < 0.03,
        }
        for i in range(N_JOBS)
    ]

    # ---- run the pool ---------------------------------------------------
    pool: WorkerPool[dict] = WorkerPool(
        process_job,
        n_workers=N_WORKERS,
        queue_size=QUEUE_SIZE,
        task_timeout=TASK_TIMEOUT,
        max_retries=MAX_RETRIES,
        backoff_base=BACKOFF_BASE,
        backoff_max=BACKOFF_MAX,
        jitter=JITTER,
    )

    wall_start = time.monotonic()
    await pool.start()

    for job in jobs:
        await pool.submit(job)

    await pool.shutdown(wait=True)
    wall_elapsed = time.monotonic() - wall_start

    # ---- report ---------------------------------------------------------
    results = pool.results
    stats = PoolStats()
    for r in results:
        if r.ok:
            stats.succeeded += 1
        else:
            stats.failed += 1
        stats.total_elapsed += r.elapsed

    print(f"\n{'─'*54}")
    print(f"  Jobs submitted : {N_JOBS}")
    print(f"  Workers        : {N_WORKERS}")
    print(f"  Succeeded      : {stats.succeeded}")
    print(f"  Failed         : {stats.failed}")
    print(f"  Wall time      : {wall_elapsed:.2f}s")
    print(f"  Avg task time  : {stats.total_elapsed / stats.total:.3f}s")
    print(f"{'─'*54}")

    # Breakdown of failed tasks
    failures = [r for r in results if not r.ok]
    if failures:
        print(f"\n  Failed task details ({len(failures)} total):")
        for r in sorted(failures, key=lambda x: x.task_id):
            print(
                f"    task {r.task_id:>3}  attempts={r.attempts}"
                f"  err={type(r.error).__name__}: {r.error}"
            )
    print()


if __name__ == "__main__":
    asyncio.run(_demo())
