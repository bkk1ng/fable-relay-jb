"""asyncio worker pool with bounded queue, per-task timeout, and exponential-backoff retries.

Python 3.12, stdlib only.

Usage:
    python pool.py

Architecture:
    - A bounded asyncio.Queue holds submitted jobs.
    - N long-running worker tasks pull jobs off the queue and execute them.
    - Each job runs under asyncio.wait_for with a per-task timeout.
    - On timeout or exception the job is retried up to max_retries times,
      with exponential backoff (base_delay * 2**attempt, capped at max_delay)
      plus optional jitter so a thundering herd of retries doesn't form.
    - A fresh coroutine is built for every attempt, so a timed-out or failed
      coroutine is never reused.
    - Every finished job produces a JobResult stored under its id.
    - shutdown() injects one sentinel per worker so each worker wakes, drains
      any queued work, and exits; workers that don't stop within an optional
      timeout are cancelled.
"""

from __future__ import annotations

import asyncio
import random
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable


CoroFn = Callable[..., Awaitable[Any]]


@dataclass
class JobResult:
    """Outcome of one submitted job after all retry attempts."""

    job_id: int
    success: bool
    result: Any = None
    error: str | None = None
    attempts: int = 0
    elapsed: float = 0.0


@dataclass
class _Job:
    """Internal queue payload: a job plus the parameters that govern its retries."""

    job_id: int
    coro_fn: CoroFn
    args: tuple
    kwargs: dict[str, Any]
    timeout: float
    max_retries: int
    base_delay: float
    max_delay: float
    jitter: float
    attempts: int = 0


# Distinct sentinel object. A worker that pulls this out of the queue stops.
_SHUTDOWN_SENTINEL: Any = object()


class WorkerPool:
    """An asyncio worker pool with a bounded queue and exponential-backoff retries.

    Submit jobs with submit(); N workers pull them off a bounded queue, execute
    each under a per-task timeout, and retry on failure with exponential backoff
    plus jitter. Results are collected and retrievable after shutdown.
    """

    def __init__(
        self,
        num_workers: int = 4,
        queue_size: int = 16,
        default_timeout: float = 5.0,
        max_retries: int = 3,
        base_delay: float = 0.1,
        max_delay: float = 5.0,
        jitter: float = 0.25,
    ) -> None:
        if num_workers <= 0:
            raise ValueError("num_workers must be positive")
        if queue_size <= 0:
            raise ValueError("queue_size must be positive")
        self._num_workers = num_workers
        self._default_timeout = default_timeout
        self._max_retries = max_retries
        self._base_delay = base_delay
        self._max_delay = max_delay
        self._jitter = jitter

        self._queue: asyncio.Queue[_Job | object] = asyncio.Queue(maxsize=queue_size)
        self._workers: list[asyncio.Task[None]] = []
        self._results: dict[int, JobResult] = {}
        self._next_id = 0
        self._running = False
        self._closed = False

    # ------------------------------------------------------------------ setup

    @property
    def num_workers(self) -> int:
        return self._num_workers

    @property
    def pending(self) -> int:
        """Approximate number of jobs still waiting in the queue."""
        return self._queue.qsize()

    async def start(self) -> None:
        """Spawn the worker tasks (idempotent)."""
        if self._running:
            return
        self._running = True
        self._workers = [
            asyncio.create_task(self._worker(i), name=f"worker-{i}")
            for i in range(self._num_workers)
        ]

    # ----------------------------------------------------------------- submit

    async def submit(
        self,
        coro_fn: CoroFn,
        *args: Any,
        timeout: float | None = None,
        max_retries: int | None = None,
        **kwargs: Any,
    ) -> int:
        """Submit a job and return its assigned id.

        Blocks until space is available in the bounded queue. Raises
        RuntimeError if the pool has already been shut down.
        """
        if self._closed:
            raise RuntimeError("pool is closed; cannot submit after shutdown")
        if not self._running:
            await self.start()
        job_id = self._next_id
        self._next_id += 1
        job = _Job(
            job_id=job_id,
            coro_fn=coro_fn,
            args=args,
            kwargs=kwargs,
            timeout=self._default_timeout if timeout is None else timeout,
            max_retries=self._max_retries if max_retries is None else max_retries,
            base_delay=self._base_delay,
            max_delay=self._max_delay,
            jitter=self._jitter,
        )
        await self._queue.put(job)
        return job_id

    # ---------------------------------------------------------------- workers

    async def _worker(self, worker_id: int) -> None:
        """Pull jobs forever; exit cleanly when a shutdown sentinel arrives."""
        while True:
            item = await self._queue.get()
            if item is _SHUTDOWN_SENTINEL:
                self._queue.task_done()
                return
            job: _Job = item  # type: ignore[assignment]
            try:
                await self._process(job)
            finally:
                self._queue.task_done()

    async def _process(self, job: _Job) -> None:
        """Run one job with timeout, retrying with exponential backoff on failure."""
        start = time.perf_counter()
        last_error: str | None = None
        result: Any = None
        success = False

        # 1 initial attempt plus up to max_retries retries.
        total_attempts = job.max_retries + 1
        for attempt in range(1, total_attempts + 1):
            job.attempts = attempt
            try:
                # A brand-new coroutine per attempt: a timed-out or failed
                # coroutine cannot be resumed, so we rebuild it each time.
                result = await asyncio.wait_for(
                    job.coro_fn(*job.args, **job.kwargs),
                    timeout=job.timeout,
                )
                success = True
                last_error = None
                break
            except asyncio.TimeoutError:
                last_error = (
                    f"TimeoutError: timed out after {job.timeout}s "
                    f"(attempt {attempt}/{total_attempts})"
                )
            except asyncio.CancelledError:
                # Propagate cancellation so the pool can shut down promptly.
                raise
            except Exception as exc:  # noqa: BLE001 - retry on any other error
                last_error = (
                    f"{type(exc).__name__}: {exc} "
                    f"(attempt {attempt}/{total_attempts})"
                )

            # Back off before the next retry.
            if attempt < total_attempts:
                delay = min(job.base_delay * (2 ** (attempt - 1)), job.max_delay)
                if job.jitter > 0:
                    delay += random.uniform(0, delay * job.jitter)
                await asyncio.sleep(delay)

        elapsed = time.perf_counter() - start
        self._results[job.job_id] = JobResult(
            job_id=job.job_id,
            success=success,
            result=result,
            error=None if success else last_error,
            attempts=job.attempts,
            elapsed=elapsed,
        )

    # --------------------------------------------------------------- shutdown

    async def shutdown(self, timeout: float | None = None) -> None:
        """Signal workers to stop and wait for them.

        One sentinel per worker is queued (behind any pending jobs, so the
        queue drains gracefully). If *timeout* is given and a worker has not
        stopped by then, it is cancelled.
        """
        if not self._running:
            return
        self._running = False
        self._closed = True

        # One sentinel per worker; each worker exits as soon as it pulls one.
        # These are queued behind any outstanding jobs, so the pool drains
        # before shutting down.
        for _ in range(self._num_workers):
            await self._queue.put(_SHUTDOWN_SENTINEL)

        if self._workers:
            done, pending = await asyncio.wait(
                self._workers,
                timeout=timeout,
                return_when=asyncio.ALL_COMPLETED,
            )
            for task in pending:
                task.cancel()
            if pending:
                await asyncio.gather(*pending, return_exceptions=True)
        self._workers = []

    # ---------------------------------------------------------------- results

    async def results(self) -> list[JobResult]:
        """Return all collected results, ordered by job id."""
        return [self._results[k] for k in sorted(self._results)]

    async def result(self, job_id: int) -> JobResult | None:
        """Return the result for a single job id, or None if not finished."""
        return self._results.get(job_id)

    # ----------------------------------------------------------- context mgr

    async def __aenter__(self) -> "WorkerPool":
        await self.start()
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        await self.shutdown()


# ---------------------------------------------------------------------- demo


async def _demo_job(job_id: int, duration: float, fail_prob: float) -> str:
    """Simulated job: sleep for *duration* seconds, then maybe fail.

    Each call builds a fresh coroutine, so retries are safe.
    """
    await asyncio.sleep(duration)
    if random.random() < fail_prob:
        raise RuntimeError(f"job {job_id} exploded")
    return f"job {job_id} done in {duration:.2f}s"


async def main() -> None:
    """Process 50 simulated jobs and print timing plus failure counts."""
    random.seed(42)

    num_jobs = 50
    pool = WorkerPool(
        num_workers=8,
        queue_size=32,
        default_timeout=2.0,
        max_retries=3,
        base_delay=0.05,
        max_delay=2.0,
        jitter=0.25,
    )

    t0 = time.perf_counter()
    async with pool:
        for i in range(num_jobs):
            await pool.submit(
                _demo_job,
                i,
                duration=random.uniform(0.05, 0.4),
                fail_prob=0.25,
            )
    elapsed = time.perf_counter() - t0

    results = await pool.results()
    successes = [r for r in results if r.success]
    failures = [r for r in results if not r.success]
    total_attempts = sum(r.attempts for r in results)
    total_job_time = sum(r.elapsed for r in results)

    print("--- Worker Pool Demo ---")
    print(f"Jobs submitted:  {num_jobs}")
    print(f"Workers:         {pool.num_workers}")
    print(f"Queue size:      32")
    print(f"Elapsed (wall):  {elapsed:.3f}s")
    print(f"Succeeded:       {len(successes)}")
    print(f"Failed:          {len(failures)}")
    print(f"Total attempts:  {total_attempts}")
    print(f"Avg attempts:    {total_attempts / num_jobs:.2f}")
    print(f"Sum job time:    {total_job_time:.3f}s "
          f"(parallel speedup ~{total_job_time / elapsed:.1f}x)")

    if failures:
        print("\nFailed jobs:")
        for r in failures:
            print(f"  job {r.job_id:>2}: {r.error}  [{r.attempts} attempt(s)]")

    print("\nPer-job summary:")
    for r in results:
        status = "OK " if r.success else "ERR"
        detail = r.result if r.success else r.error
        print(f"  [{status}] job {r.job_id:>2}: "
              f"{r.attempts} attempt(s), {r.elapsed:.3f}s -> {detail}")


if __name__ == "__main__":
    asyncio.run(main())
