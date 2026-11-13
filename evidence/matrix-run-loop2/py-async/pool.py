"""
Async worker pool — Python 3.12, stdlib only.

Features:
  - Bounded input queue (backpressure-aware)
  - N concurrent workers
  - Per-task timeout
  - Exponential-backoff retries with jitter
  - Task result collection (success + failure)
  - Clean cancellation on shutdown (SIGINT / explicit)
  - __main__ demo: 50 simulated jobs, timing, failure counts
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
import random
import time
from collections.abc import Awaitable, Callable
from typing import Any, Generic, TypeVar

logger = logging.getLogger(__name__)

T = TypeVar("T")


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------

@dataclasses.dataclass(slots=True)
class TaskResult(Generic[T]):
    task_id: int
    success: bool
    value: T | None = None
    error: BaseException | None = None
    attempts: int = 1
    elapsed: float = 0.0


# ---------------------------------------------------------------------------
# Pool
# ---------------------------------------------------------------------------

class WorkerPool(Generic[T]):
    """
    Bounded async worker pool.

    Parameters
    ----------
    worker_fn:
        Async callable that accepts a single payload and returns T.
    n_workers:
        Number of concurrent workers.
    queue_size:
        Maximum items in the input queue (0 = unbounded).
    task_timeout:
        Per-attempt wall-clock timeout in seconds.
    max_retries:
        Maximum retry attempts after the initial try (total = max_retries + 1).
    base_backoff:
        Initial backoff in seconds; doubles each retry.
    max_backoff:
        Backoff ceiling in seconds.
    jitter:
        Fraction of backoff added as random jitter (0–1).
    """

    def __init__(
        self,
        worker_fn: Callable[[Any], Awaitable[T]],
        *,
        n_workers: int = 4,
        queue_size: int = 0,
        task_timeout: float = 10.0,
        max_retries: int = 3,
        base_backoff: float = 0.5,
        max_backoff: float = 30.0,
        jitter: float = 0.25,
    ) -> None:
        self._fn = worker_fn
        self._n_workers = n_workers
        self._task_timeout = task_timeout
        self._max_retries = max_retries
        self._base_backoff = base_backoff
        self._max_backoff = max_backoff
        self._jitter = jitter

        self._queue: asyncio.Queue[tuple[int, Any] | None] = asyncio.Queue(
            maxsize=queue_size
        )
        self._results: list[TaskResult[T]] = []
        self._results_lock = asyncio.Lock()
        self._workers: list[asyncio.Task[None]] = []
        self._running = False

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    async def start(self) -> None:
        """Spawn workers; call before submitting tasks."""
        if self._running:
            raise RuntimeError("Pool is already running.")
        self._running = True
        self._workers = [
            asyncio.create_task(self._worker_loop(wid), name=f"worker-{wid}")
            for wid in range(self._n_workers)
        ]
        logger.debug("Pool started with %d workers.", self._n_workers)

    async def submit(self, task_id: int, payload: Any) -> None:
        """Enqueue a task.  Blocks if the queue is full (backpressure)."""
        if not self._running:
            raise RuntimeError("Pool is not running.  Call start() first.")
        await self._queue.put((task_id, payload))

    async def join(self) -> list[TaskResult[T]]:
        """
        Signal end-of-input, wait for all tasks to finish, stop workers,
        and return collected results.
        """
        # One sentinel per worker
        for _ in self._workers:
            await self._queue.put(None)

        await asyncio.gather(*self._workers, return_exceptions=True)
        self._running = False
        return list(self._results)

    async def shutdown(self, *, cancel_pending: bool = True) -> list[TaskResult[T]]:
        """
        Graceful shutdown.  If cancel_pending is True (default), drain the
        queue and cancel workers; otherwise behaves like join().
        """
        if not cancel_pending:
            return await self.join()

        self._running = False

        # Drain remaining items so workers don't block on put
        while not self._queue.empty():
            try:
                self._queue.get_nowait()
            except asyncio.QueueEmpty:
                break

        for w in self._workers:
            w.cancel()

        await asyncio.gather(*self._workers, return_exceptions=True)
        return list(self._results)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    async def _worker_loop(self, wid: int) -> None:
        logger.debug("Worker %d started.", wid)
        while True:
            item = await self._queue.get()
            if item is None:          # sentinel → clean exit
                self._queue.task_done()
                break
            task_id, payload = item
            result = await self._run_with_retry(task_id, payload)
            async with self._results_lock:
                self._results.append(result)
            self._queue.task_done()
        logger.debug("Worker %d exiting.", wid)

    async def _run_with_retry(self, task_id: int, payload: Any) -> TaskResult[T]:
        attempts = 0
        t0 = time.monotonic()
        last_error: BaseException | None = None

        while attempts <= self._max_retries:
            attempts += 1
            try:
                value: T = await asyncio.wait_for(
                    self._fn(payload),
                    timeout=self._task_timeout,
                )
                return TaskResult(
                    task_id=task_id,
                    success=True,
                    value=value,
                    attempts=attempts,
                    elapsed=time.monotonic() - t0,
                )
            except asyncio.CancelledError:
                raise  # propagate so the worker can exit cleanly
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                logger.debug(
                    "Task %d attempt %d failed: %r", task_id, attempts, exc
                )
                if attempts <= self._max_retries:
                    delay = self._backoff_delay(attempts)
                    logger.debug(
                        "Task %d backing off %.2fs before retry.", task_id, delay
                    )
                    await asyncio.sleep(delay)

        return TaskResult(
            task_id=task_id,
            success=False,
            error=last_error,
            attempts=attempts,
            elapsed=time.monotonic() - t0,
        )

    def _backoff_delay(self, attempt: int) -> float:
        """Full-jitter exponential backoff."""
        base = min(self._base_backoff * (2 ** (attempt - 1)), self._max_backoff)
        return base * (1 + self._jitter * random.random())  # noqa: S311


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

async def _simulated_job(payload: dict[str, Any]) -> str:
    """
    Fake workload:
      - 20% chance of transient error
      - 10% chance of timeout (sleeps past the pool's timeout)
      - otherwise completes in 0.05–0.4 s
    """
    job_id: int = payload["id"]
    rng = random.random()

    if rng < 0.10:
        # Simulate a timeout — sleep longer than task_timeout
        await asyncio.sleep(99)

    if rng < 0.20:
        raise RuntimeError(f"job {job_id}: transient error")

    duration = random.uniform(0.05, 0.40)
    await asyncio.sleep(duration)
    return f"job-{job_id}:ok"


async def _demo() -> None:
    logging.basicConfig(
        level=logging.WARNING,
        format="%(asctime)s  %(levelname)-8s  %(message)s",
    )

    n_jobs = 50
    n_workers = 8
    task_timeout = 0.6   # seconds — tight enough to catch the slow jobs
    max_retries = 2
    queue_size = 20

    print(
        f"Demo: {n_jobs} jobs, {n_workers} workers, "
        f"timeout={task_timeout}s, max_retries={max_retries}"
    )
    wall_start = time.monotonic()

    pool: WorkerPool[str] = WorkerPool(
        _simulated_job,
        n_workers=n_workers,
        queue_size=queue_size,
        task_timeout=task_timeout,
        max_retries=max_retries,
        base_backoff=0.1,
        max_backoff=1.0,
        jitter=0.3,
    )

    await pool.start()

    # Submit all jobs — this will block when the queue is full (backpressure demo)
    for i in range(n_jobs):
        await pool.submit(i, {"id": i})

    results = await pool.join()
    wall_elapsed = time.monotonic() - wall_start

    # Tally
    successes = [r for r in results if r.success]
    failures  = [r for r in results if not r.success]
    total_attempts = sum(r.attempts for r in results)

    print(f"\nCompleted {len(results)}/{n_jobs} tasks in {wall_elapsed:.2f}s wall time")
    print(f"  Successes : {len(successes)}")
    print(f"  Failures  : {len(failures)}")
    print(f"  Total attempts (incl. retries): {total_attempts}")

    if failures:
        print(f"\nFailed task IDs and final errors:")
        for r in sorted(failures, key=lambda x: x.task_id):
            print(f"  task {r.task_id:>3d}  attempts={r.attempts}  err={r.error!r}")

    if successes:
        avg_elapsed = sum(r.elapsed for r in successes) / len(successes)
        print(f"\nMean per-task elapsed (successes): {avg_elapsed*1000:.1f}ms")


if __name__ == "__main__":
    asyncio.run(_demo())
