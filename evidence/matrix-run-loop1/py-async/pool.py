"""
pool.py — asyncio worker pool
Python 3.12, stdlib only.

Features:
  - Bounded input queue (backpressure)
  - N concurrent workers
  - Per-task timeout
  - Exponential-backoff retries with jitter
  - Task result collection (success + failure)
  - Clean cancellation on shutdown
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
import math
import random
import time
from collections.abc import Awaitable, Callable
from typing import Any, Generic, TypeVar

logger = logging.getLogger(__name__)

T = TypeVar("T")


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclasses.dataclass(slots=True)
class TaskResult(Generic[T]):
    task_id: int
    success: bool
    value: T | None = None
    error: BaseException | None = None
    attempts: int = 0
    elapsed: float = 0.0


@dataclasses.dataclass(slots=True)
class _WorkItem(Generic[T]):
    task_id: int
    coro_fn: Callable[..., Awaitable[T]]
    args: tuple[Any, ...]
    kwargs: dict[str, Any]


# ---------------------------------------------------------------------------
# Pool
# ---------------------------------------------------------------------------

class WorkerPool(Generic[T]):
    """
    Bounded asyncio worker pool.

    Parameters
    ----------
    workers:        number of concurrent worker coroutines
    queue_size:     max items waiting in the queue (0 = unlimited)
    task_timeout:   per-attempt wall-clock timeout in seconds (None = none)
    max_retries:    maximum retry attempts after the first failure
    base_backoff:   initial retry delay in seconds
    max_backoff:    ceiling for retry delay
    """

    def __init__(
        self,
        *,
        workers: int = 4,
        queue_size: int = 100,
        task_timeout: float | None = 10.0,
        max_retries: int = 3,
        base_backoff: float = 0.25,
        max_backoff: float = 30.0,
    ) -> None:
        if workers < 1:
            raise ValueError("workers must be >= 1")
        self._workers = workers
        self._queue_size = queue_size
        self._task_timeout = task_timeout
        self._max_retries = max_retries
        self._base_backoff = base_backoff
        self._max_backoff = max_backoff

        self._queue: asyncio.Queue[_WorkItem[T] | None] = asyncio.Queue(
            maxsize=queue_size
        )
        self._results: list[TaskResult[T]] = []
        self._results_lock = asyncio.Lock()
        self._worker_tasks: list[asyncio.Task[None]] = []
        self._next_id = 0
        self._running = False

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    async def start(self) -> None:
        """Spawn worker tasks. Call before submitting work."""
        if self._running:
            raise RuntimeError("pool is already running")
        self._running = True
        self._worker_tasks = [
            asyncio.create_task(self._worker_loop(wid), name=f"worker-{wid}")
            for wid in range(self._workers)
        ]
        logger.debug("pool started with %d workers", self._workers)

    async def submit(
        self,
        coro_fn: Callable[..., Awaitable[T]],
        *args: Any,
        **kwargs: Any,
    ) -> int:
        """
        Enqueue a job and return its task_id.
        Blocks if the queue is full (backpressure).
        """
        if not self._running:
            raise RuntimeError("pool is not running; call start() first")
        task_id = self._next_id
        self._next_id += 1
        item: _WorkItem[T] = _WorkItem(
            task_id=task_id,
            coro_fn=coro_fn,
            args=args,
            kwargs=kwargs,
        )
        await self._queue.put(item)
        return task_id

    async def shutdown(self, *, wait: bool = True, cancel: bool = False) -> None:
        """
        Signal workers to stop.

        wait=True   — wait until all queued work finishes (graceful).
        cancel=True — cancel in-flight tasks immediately (fast shutdown).
        """
        if not self._running:
            return
        self._running = False

        if cancel:
            for t in self._worker_tasks:
                t.cancel()
            await asyncio.gather(*self._worker_tasks, return_exceptions=True)
        else:
            # Poison-pill: one None sentinel per worker
            for _ in self._worker_tasks:
                await self._queue.put(None)
            if wait:
                await asyncio.gather(*self._worker_tasks, return_exceptions=True)

        logger.debug("pool shut down")

    @property
    def results(self) -> list[TaskResult[T]]:
        """Snapshot of collected results (safe to read after shutdown)."""
        return list(self._results)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    async def _worker_loop(self, wid: int) -> None:
        logger.debug("worker %d started", wid)
        while True:
            try:
                item = await self._queue.get()
            except asyncio.CancelledError:
                logger.debug("worker %d cancelled while waiting", wid)
                break

            if item is None:
                # Received sentinel — clean exit
                self._queue.task_done()
                break

            result = await self._execute(item)
            async with self._results_lock:
                self._results.append(result)
            self._queue.task_done()

        logger.debug("worker %d exiting", wid)

    async def _execute(self, item: _WorkItem[T]) -> TaskResult[T]:
        t0 = time.perf_counter()
        last_error: BaseException | None = None

        for attempt in range(1, self._max_retries + 2):  # +2: initial + retries
            if attempt > 1:
                delay = self._backoff(attempt - 1)
                logger.debug(
                    "task %d attempt %d, backing off %.3fs",
                    item.task_id, attempt, delay,
                )
                await asyncio.sleep(delay)

            try:
                coro = item.coro_fn(*item.args, **item.kwargs)
                if self._task_timeout is not None:
                    value = await asyncio.wait_for(coro, timeout=self._task_timeout)
                else:
                    value = await coro

                elapsed = time.perf_counter() - t0
                return TaskResult(
                    task_id=item.task_id,
                    success=True,
                    value=value,
                    attempts=attempt,
                    elapsed=elapsed,
                )

            except asyncio.CancelledError:
                # Worker is being cancelled — propagate immediately
                raise
            except Exception as exc:
                last_error = exc
                logger.debug(
                    "task %d attempt %d failed: %s", item.task_id, attempt, exc
                )

        elapsed = time.perf_counter() - t0
        return TaskResult(
            task_id=item.task_id,
            success=False,
            error=last_error,
            attempts=self._max_retries + 1,
            elapsed=elapsed,
        )

    def _backoff(self, attempt: int) -> float:
        """Full-jitter exponential backoff."""
        ceiling = min(self._max_backoff, self._base_backoff * math.pow(2, attempt))
        return random.uniform(0.0, ceiling)


# ---------------------------------------------------------------------------
# Demo / __main__
# ---------------------------------------------------------------------------

async def _simulated_job(job_id: int, *, fail_rate: float = 0.15) -> str:
    """
    Simulates real work:
      - random duration 0.05 – 0.5 s
      - fails with probability `fail_rate` (raises to exercise retries)
      - occasionally slow (triggers timeout when pool timeout is tight)
    """
    duration = random.uniform(0.05, 0.5)
    await asyncio.sleep(duration)
    if random.random() < fail_rate:
        raise ValueError(f"job {job_id} simulated failure")
    return f"job-{job_id}-done"


async def main() -> None:
    logging.basicConfig(
        level=logging.WARNING,          # set to DEBUG to watch internals
        format="%(levelname)s %(name)s: %(message)s",
    )

    NUM_JOBS = 50
    WORKERS = 8
    QUEUE_SIZE = 20                     # backpressure: at most 20 waiting
    TASK_TIMEOUT = 0.6                  # seconds per attempt
    MAX_RETRIES = 2
    FAIL_RATE = 0.20                    # 20 % of jobs fail on each attempt

    pool: WorkerPool[str] = WorkerPool(
        workers=WORKERS,
        queue_size=QUEUE_SIZE,
        task_timeout=TASK_TIMEOUT,
        max_retries=MAX_RETRIES,
        base_backoff=0.1,
        max_backoff=1.0,
    )

    await pool.start()

    wall_start = time.perf_counter()

    # Submit all jobs — this will block when queue is full (backpressure demo)
    for jid in range(NUM_JOBS):
        await pool.submit(_simulated_job, jid, fail_rate=FAIL_RATE)

    await pool.shutdown(wait=True, cancel=False)

    wall_elapsed = time.perf_counter() - wall_start

    # ---- report ----
    results = pool.results
    successes = [r for r in results if r.success]
    failures  = [r for r in results if not r.success]

    total_task_time = sum(r.elapsed for r in results)
    avg_task_time   = total_task_time / len(results) if results else 0.0
    max_attempts    = max((r.attempts for r in results), default=0)

    print("=" * 55)
    print(f"  Jobs submitted : {NUM_JOBS}")
    print(f"  Jobs completed : {len(results)}")
    print(f"  Successes      : {len(successes)}")
    print(f"  Failures       : {len(failures)}")
    print(f"  Wall time      : {wall_elapsed:.3f}s  ({WORKERS} workers)")
    print(f"  Avg task time  : {avg_task_time:.3f}s")
    print(f"  Max attempts   : {max_attempts}  (retries={MAX_RETRIES})")
    print("=" * 55)

    if failures:
        print("\nFailed tasks:")
        for r in failures[:10]:  # cap at 10 lines
            print(f"  task {r.task_id:>3}: {r.error!r}  (attempts={r.attempts})")
        if len(failures) > 10:
            print(f"  ... and {len(failures) - 10} more")


if __name__ == "__main__":
    asyncio.run(main())
