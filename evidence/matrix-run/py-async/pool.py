"""
pool.py — asyncio worker pool, Python 3.12, stdlib only.

Features:
  • bounded asyncio.Queue  (back-pressure on producers)
  • N concurrent workers
  • per-task asyncio.wait_for timeout
  • exponential-backoff retries with jitter
  • task result collection
  • clean shutdown (drain in-flight work, signal workers to exit)

Typical usage::

    pool = WorkerPool(n_workers=4, queue_size=200, timeout=2.0, max_retries=3)
    await pool.start()
    for item in workload:
        await pool.submit(my_coro_fn, item)
    await pool.join()
    results = await pool.shutdown()

Or as an async context manager (shutdown is called automatically)::

    async with WorkerPool(n_workers=4) as pool:
        for item in workload:
            await pool.submit(my_coro_fn, item)
        await pool.join()
    results = pool.results
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
import random
import time
from collections.abc import Callable, Coroutine
from typing import Any

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-7s %(name)s — %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("pool")

# Sentinel that tells a worker to exit its loop.
_STOP: object = object()


# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------


@dataclasses.dataclass(slots=True)
class TaskResult:
    """Outcome of a single task, including retry metadata."""

    task_id: int
    success: bool
    result: Any = None
    error: Exception | None = None
    attempts: int = 1
    elapsed: float = 0.0  # wall-clock seconds from first attempt through last

    def __str__(self) -> str:
        status = "OK  " if self.success else "FAIL"
        tail = f"  error={self.error!r}" if self.error else ""
        return (
            f"Task #{self.task_id:3d} [{status}]"
            f"  attempts={self.attempts}"
            f"  elapsed={self.elapsed:.3f}s"
            f"{tail}"
        )


# ---------------------------------------------------------------------------
# Worker pool
# ---------------------------------------------------------------------------


class WorkerPool:
    """
    Asyncio worker pool with bounded queue, per-task timeout, and
    exponential-backoff retries.

    Parameters
    ----------
    n_workers:
        Number of concurrent worker coroutines.
    queue_size:
        Maximum items in the queue before ``submit`` blocks.
    timeout:
        Seconds before a single attempt is considered timed out.
    max_retries:
        How many times to retry after a failure.  Total attempts =
        ``max_retries + 1``.
    base_delay:
        Initial back-off delay in seconds.  Doubles each retry.
    max_delay:
        Cap on the back-off delay.
    """

    def __init__(
        self,
        *,
        n_workers: int = 4,
        queue_size: int = 100,
        timeout: float = 5.0,
        max_retries: int = 3,
        base_delay: float = 0.25,
        max_delay: float = 8.0,
    ) -> None:
        if n_workers < 1:
            raise ValueError("n_workers must be >= 1")
        if queue_size < 1:
            raise ValueError("queue_size must be >= 1")

        self._n_workers = n_workers
        self._timeout = timeout
        self._max_retries = max_retries
        self._base_delay = base_delay
        self._max_delay = max_delay

        self._queue: asyncio.Queue[Any] = asyncio.Queue(maxsize=queue_size)
        self._results: list[TaskResult] = []
        self._task_counter: int = 0
        self._workers: list[asyncio.Task[None]] = []
        self._running: bool = False

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    @property
    def results(self) -> list[TaskResult]:
        """All completed task results collected so far (snapshot copy)."""
        return list(self._results)

    async def start(self) -> None:
        """Spawn the worker tasks.  Must be called before ``submit``."""
        if self._running:
            raise RuntimeError("Pool is already running.")
        self._running = True
        self._workers = [
            asyncio.create_task(self._worker(i), name=f"pool-worker-{i}")
            for i in range(self._n_workers)
        ]
        logger.debug("Pool started with %d workers.", self._n_workers)

    async def submit(
        self,
        fn: Callable[..., Coroutine[Any, Any, Any]],
        /,
        *args: Any,
        **kwargs: Any,
    ) -> int:
        """
        Enqueue a job.

        Blocks when the queue is full, providing natural back-pressure so
        fast producers cannot exhaust memory.  Returns the assigned task id.
        """
        if not self._running:
            raise RuntimeError("Call start() before submitting tasks.")
        self._task_counter += 1
        task_id = self._task_counter
        await self._queue.put((task_id, fn, args, kwargs))
        return task_id

    async def join(self) -> None:
        """Block until every submitted task has been processed."""
        await self._queue.join()

    async def shutdown(self) -> list[TaskResult]:
        """
        Signal every worker to exit after finishing its current task,
        wait for all workers to stop, and return the full result list.

        It is safe to call ``shutdown`` without calling ``join`` first;
        in-flight tasks will finish normally before the workers exit.
        """
        self._running = False
        # One sentinel per worker guarantees each worker exits exactly once.
        for _ in range(self._n_workers):
            await self._queue.put(_STOP)
        await asyncio.gather(*self._workers, return_exceptions=True)
        logger.debug(
            "Pool shut down.  %d results collected.", len(self._results)
        )
        return list(self._results)

    # ------------------------------------------------------------------
    # Async context manager
    # ------------------------------------------------------------------

    async def __aenter__(self) -> WorkerPool:
        await self.start()
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> None:
        if exc_type is not None:
            # Abnormal exit: cancel workers immediately rather than draining.
            for w in self._workers:
                w.cancel()
            await asyncio.gather(*self._workers, return_exceptions=True)
        else:
            await self.shutdown()

    # ------------------------------------------------------------------
    # Worker internals
    # ------------------------------------------------------------------

    async def _worker(self, worker_id: int) -> None:
        logger.debug("Worker %d ready.", worker_id)
        while True:
            item = await self._queue.get()
            try:
                if item is _STOP:
                    break
                task_id, fn, args, kwargs = item
                result = await self._execute(task_id, fn, args, kwargs)
                self._results.append(result)
                if result.success:
                    logger.debug("worker=%d  %s", worker_id, result)
                else:
                    logger.warning("worker=%d  %s", worker_id, result)
            finally:
                # Always mark done so queue.join() can unblock correctly.
                self._queue.task_done()
        logger.debug("Worker %d exiting.", worker_id)

    async def _execute(
        self,
        task_id: int,
        fn: Callable[..., Coroutine[Any, Any, Any]],
        args: tuple[Any, ...],
        kwargs: dict[str, Any],
    ) -> TaskResult:
        """
        Run ``fn(*args, **kwargs)`` with timeout and exponential-backoff
        retries.  Returns a ``TaskResult`` regardless of outcome.
        """
        t0 = time.monotonic()
        total_attempts = self._max_retries + 1
        last_error: Exception | None = None

        for attempt in range(1, total_attempts + 1):
            try:
                value = await asyncio.wait_for(
                    fn(*args, **kwargs),
                    timeout=self._timeout,
                )
                return TaskResult(
                    task_id=task_id,
                    success=True,
                    result=value,
                    attempts=attempt,
                    elapsed=time.monotonic() - t0,
                )

            except asyncio.CancelledError:
                # Propagate without retrying; the pool is shutting down.
                raise

            except Exception as exc:
                last_error = exc
                logger.debug(
                    "Task #%d  attempt %d/%d  failed: %s",
                    task_id, attempt, total_attempts, exc,
                )
                if attempt < total_attempts:
                    # Exponential back-off with ±10 % jitter.
                    raw = self._base_delay * (2 ** (attempt - 1))
                    jitter = raw * random.uniform(-0.10, 0.10)
                    delay = min(raw + jitter, self._max_delay)
                    await asyncio.sleep(delay)

        return TaskResult(
            task_id=task_id,
            success=False,
            error=last_error,
            attempts=total_attempts,
            elapsed=time.monotonic() - t0,
        )


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    # Per-job attempt counter; asyncio is single-threaded so a plain dict
    # is safe here without locks.
    _job_attempts: dict[int, int] = {}

    async def simulate_job(job_id: int) -> str:
        """
        Three behaviours based on job_id:
          job_id % 10 == 0  — always exceeds timeout (models a hung RPC call)
          job_id % 7  == 0  — transient: fails on attempts 1–2, succeeds on 3
          otherwise         — normal: short random sleep then success
        """
        _job_attempts[job_id] = _job_attempts.get(job_id, 0) + 1
        attempt = _job_attempts[job_id]

        if job_id % 10 == 0:
            await asyncio.sleep(999.0)          # will always hit the timeout

        if job_id % 7 == 0 and attempt < 3:
            await asyncio.sleep(random.uniform(0.01, 0.04))
            raise ValueError(f"transient error  job={job_id}  attempt={attempt}")

        await asyncio.sleep(random.uniform(0.02, 0.12))
        return f"ok:{job_id}"

    async def main() -> None:
        n_jobs = 50

        pool = WorkerPool(
            n_workers=6,
            queue_size=20,
            timeout=0.4,        # tight — catches the sleep-999 jobs quickly
            max_retries=3,      # transient jobs need 3 attempts; 4 total here
            base_delay=0.05,
            max_delay=1.0,
        )

        wall_start = time.monotonic()

        await pool.start()
        for jid in range(1, n_jobs + 1):
            await pool.submit(simulate_job, jid)

        await pool.join()
        results = await pool.shutdown()

        wall_elapsed = time.monotonic() - wall_start

        # ── categorise ───────────────────────────────────────────────
        successes = [r for r in results if r.success]
        failures  = [r for r in results if not r.success]
        timeouts  = [
            r for r in failures
            if isinstance(r.error, asyncio.TimeoutError)
        ]
        errors    = [
            r for r in failures
            if not isinstance(r.error, asyncio.TimeoutError)
        ]
        retried   = [r for r in results if r.attempts > 1]

        avg_elapsed = (
            sum(r.elapsed for r in results) / len(results) if results else 0.0
        )

        # ── print summary ────────────────────────────────────────────
        sep = "─" * 54
        print(f"\n{sep}")
        print(f"  Jobs submitted    : {n_jobs}")
        print(f"  Jobs succeeded    : {len(successes)}")
        print(f"  Jobs failed       : {len(failures)}")
        print(f"    timeout failures : {len(timeouts)}")
        print(f"    error  failures  : {len(errors)}")
        print(f"  Jobs retried      : {len(retried)}")
        print(f"  Wall-clock time   : {wall_elapsed:.2f}s")
        print(f"  Avg task time     : {avg_elapsed:.3f}s")
        print(sep)

        if failures:
            print("\nFailed tasks:")
            for r in sorted(failures, key=lambda r: r.task_id):
                print(f"  {r}")
            print()

        sys.exit(0 if not failures else 1)

    asyncio.run(main())
