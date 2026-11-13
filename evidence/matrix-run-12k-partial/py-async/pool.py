"""
pool.py ─ asyncio worker pool with bounded queue, per-task timeout,
exponential-backoff retries, result collection, and clean shutdown.

Requirements: Python 3.12, stdlib only.

Usage
─────
    async with AsyncWorkerPool(n_workers=8, timeout=2.0, max_retries=3) as pool:
        fut = await pool.submit(my_coro, arg1, kwarg=val)
    # all tasks have finished; futures are resolved
    results = pool.results

Run the built-in demo:
    python pool.py
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
import random
import time
from collections.abc import Callable, Coroutine
from typing import Any, Final

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Public types
# ─────────────────────────────────────────────────────────────────────────────

@dataclasses.dataclass(slots=True)
class TaskResult:
    """Outcome of one submitted task."""
    task_id:  int
    value:    Any                   # return value on success; None on failure
    error:    BaseException | None  # terminal exception; None on success
    attempts: int                   # 1 = succeeded on first try
    duration: float                 # wall-clock seconds (all attempts combined)


# ─────────────────────────────────────────────────────────────────────────────
# Internal types
# ─────────────────────────────────────────────────────────────────────────────

_SENTINEL: Final[object] = object()  # poison-pill that stops a worker


@dataclasses.dataclass(slots=True)
class _WorkItem:
    task_id: int
    fn:      Callable[..., Coroutine[Any, Any, Any]]
    args:    tuple[Any, ...]
    kwargs:  dict[str, Any]
    future:  asyncio.Future[Any]


# ─────────────────────────────────────────────────────────────────────────────
# Pool
# ─────────────────────────────────────────────────────────────────────────────

class AsyncWorkerPool:
    """
    Bounded asyncio worker pool.

    Parameters
    ----------
    n_workers       Number of concurrent worker coroutines.
    queue_size      Max tasks waiting in the queue; ``submit`` blocks
                    (back-pressure) when the queue is at capacity.
    timeout         Per-attempt wall-clock limit in seconds.
    max_retries     Max *retries* after the first attempt (total attempts
                    = max_retries + 1).  0 means try exactly once.
    base_delay      Initial back-off delay in seconds.
    max_delay       Cap on the computed back-off delay.
    jitter          Multiply each delay by Uniform(0.5, 1.5) to spread load.
    """

    def __init__(
        self,
        *,
        n_workers:   int   = 4,
        queue_size:  int   = 100,
        timeout:     float = 5.0,
        max_retries: int   = 3,
        base_delay:  float = 0.5,
        max_delay:   float = 30.0,
        jitter:      bool  = True,
    ) -> None:
        self.n_workers   = n_workers
        self.timeout     = timeout
        self.max_retries = max_retries
        self.base_delay  = base_delay
        self.max_delay   = max_delay
        self.jitter      = jitter

        self._queue: asyncio.Queue[_WorkItem | object] = asyncio.Queue(
            maxsize=queue_size
        )
        self._workers: list[asyncio.Task[None]] = []
        self._results: list[TaskResult] = []
        self._counter:  int = 0

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    async def start(self) -> None:
        """Spawn worker tasks.  Must be called before ``submit``."""
        self._workers = [
            asyncio.create_task(self._worker(i), name=f"pool-worker-{i}")
            for i in range(self.n_workers)
        ]

    async def shutdown(self, *, cancel_pending: bool = False) -> list[TaskResult]:
        """
        Signal workers to stop and wait for all of them to finish.

        cancel_pending=False (default)
            Graceful drain: workers finish their current task (including
            retries), then stop when they see the sentinel.  Any tasks still
            waiting in the queue are also executed.

        cancel_pending=True
            Immediate stop: tasks still in the queue have their futures
            cancelled and are discarded; in-flight worker tasks are
            asyncio-cancelled.

        Returns the list of all TaskResult objects collected so far.
        """
        if cancel_pending:
            # Drain queue, cancelling futures for tasks that never started.
            while not self._queue.empty():
                try:
                    item = self._queue.get_nowait()
                except asyncio.QueueEmpty:
                    break
                self._queue.task_done()
                if item is not _SENTINEL and isinstance(item, _WorkItem):
                    if not item.future.done():
                        item.future.cancel()
            # Cancel the worker coroutines.
            for w in self._workers:
                w.cancel()
            await asyncio.gather(*self._workers, return_exceptions=True)
        else:
            # Graceful: one sentinel per worker, then wait for them all.
            for _ in self._workers:
                await self._queue.put(_SENTINEL)
            await asyncio.gather(*self._workers, return_exceptions=True)

        return list(self._results)

    # ── Context manager ───────────────────────────────────────────────────────

    async def __aenter__(self) -> AsyncWorkerPool:
        await self.start()
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.shutdown()

    # ── Public interface ──────────────────────────────────────────────────────

    async def submit(
        self,
        fn: Callable[..., Coroutine[Any, Any, Any]],
        /,
        *args: Any,
        **kwargs: Any,
    ) -> asyncio.Future[Any]:
        """
        Enqueue *fn* to be called as ``await fn(*args, **kwargs)``.

        Blocks if the queue is at capacity (natural back-pressure).

        Returns an asyncio.Future that resolves to the task's return value
        on success, or raises the terminal exception on permanent failure.
        """
        self._counter += 1
        fut: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        await self._queue.put(
            _WorkItem(
                task_id=self._counter,
                fn=fn,
                args=args,
                kwargs=kwargs,
                future=fut,
            )
        )
        return fut

    @property
    def results(self) -> list[TaskResult]:
        """Snapshot of all TaskResult objects collected so far."""
        return list(self._results)

    # ── Internals ─────────────────────────────────────────────────────────────

    def _backoff(self, attempt: int) -> float:
        """Exponential back-off with optional full jitter."""
        delay = min(self.base_delay * (2.0 ** attempt), self.max_delay)
        if self.jitter:
            delay *= random.uniform(0.5, 1.5)
        return delay

    async def _run_once(self, item: _WorkItem) -> Any:
        """Execute the task coroutine with the configured timeout."""
        return await asyncio.wait_for(
            item.fn(*item.args, **item.kwargs),
            timeout=self.timeout,
        )

    async def _worker(self, worker_id: int) -> None:
        logger.debug("worker %d started", worker_id)
        while True:
            work = await self._queue.get()

            # Sentinel: time to stop.
            if work is _SENTINEL:
                self._queue.task_done()
                logger.debug("worker %d stopping", worker_id)
                return

            item: _WorkItem = work  # type: ignore[assignment]
            t0 = time.perf_counter()
            last_exc: BaseException | None = None
            value: Any = None
            succeeded = False
            attempt = 0

            for attempt in range(self.max_retries + 1):
                try:
                    value = await self._run_once(item)
                    succeeded = True
                    break

                except asyncio.CancelledError:
                    # Pool is being force-shut down; clean up and re-raise so
                    # the worker task itself is properly cancelled.
                    self._queue.task_done()
                    raise

                except asyncio.TimeoutError as exc:
                    last_exc = exc
                    logger.debug(
                        "worker %d | task %d | attempt %d/%d → timed out",
                        worker_id, item.task_id,
                        attempt + 1, self.max_retries + 1,
                    )

                except Exception as exc:
                    last_exc = exc
                    logger.debug(
                        "worker %d | task %d | attempt %d/%d → %r",
                        worker_id, item.task_id,
                        attempt + 1, self.max_retries + 1, exc,
                    )

                if attempt < self.max_retries:
                    await asyncio.sleep(self._backoff(attempt))

            duration = time.perf_counter() - t0
            result = TaskResult(
                task_id=item.task_id,
                value=value if succeeded else None,
                error=None if succeeded else last_exc,
                attempts=attempt + 1,
                duration=duration,
            )
            self._results.append(result)

            if not item.future.done():
                if succeeded:
                    item.future.set_result(value)
                else:
                    assert last_exc is not None
                    item.future.set_exception(last_exc)

            self._queue.task_done()


# ─────────────────────────────────────────────────────────────────────────────
# Demo
# ─────────────────────────────────────────────────────────────────────────────

async def _demo_job(
    job_id: int,
    *,
    fail_rate: float = 0.15,
    slow_rate: float = 0.10,
) -> dict[str, int]:
    """
    Simulated job with variable latency and randomised failure modes.

    fail_rate  Probability of raising an exception on any given attempt.
    slow_rate  Probability of sleeping far longer than the pool timeout
               (simulates a hung operation that exhausts retries).
    """
    # Normal work takes 20–150 ms.
    await asyncio.sleep(random.uniform(0.02, 0.15))

    r = random.random()
    if r < fail_rate:
        raise ValueError(f"job {job_id}: simulated transient error")
    if r < fail_rate + slow_rate:
        await asyncio.sleep(10.0)  # will exceed the pool timeout on every retry

    return {"job_id": job_id, "square": job_id * job_id}


async def _run_demo() -> None:
    n_jobs      = 50
    n_workers   = 8
    timeout     = 0.30   # seconds per attempt
    max_retries = 2      # up to 3 total attempts per task
    queue_size  = 20

    sep = "─" * 56

    print(sep)
    print(f"  AsyncWorkerPool demo")
    print(f"  {n_jobs} jobs  |  {n_workers} workers  |  "
          f"timeout={timeout}s  |  max_retries={max_retries}")
    print(sep)

    pool = AsyncWorkerPool(
        n_workers   = n_workers,
        queue_size  = queue_size,
        timeout     = timeout,
        max_retries = max_retries,
        base_delay  = 0.05,
        max_delay   = 1.0,
        jitter      = True,
    )

    t0: float = time.perf_counter()
    futures: list[asyncio.Future[Any]] = []

    async with pool:
        for i in range(n_jobs):
            fut = await pool.submit(_demo_job, i)
            futures.append(fut)
    # All workers have stopped and every future is resolved here.

    elapsed = time.perf_counter() - t0
    results = pool.results

    # ── Collect per-future outcomes ────────────────────────────────────────
    # futures are all done at this point; asyncio.gather returns immediately.
    outcomes = await asyncio.gather(*futures, return_exceptions=True)
    n_fut_ok  = sum(1 for o in outcomes if not isinstance(o, BaseException))
    n_fut_err = n_jobs - n_fut_ok

    # ── Aggregate from TaskResult records ─────────────────────────────────
    successful = [r for r in results if r.error is None]
    failed     = [r for r in results if r.error is not None]
    timeouts   = [r for r in failed   if isinstance(r.error, asyncio.TimeoutError)]
    errors     = [r for r in failed   if not isinstance(r.error, asyncio.TimeoutError)]
    retried    = [r for r in results  if r.attempts > 1]

    avg_ok_dur    = (sum(r.duration for r in successful) / len(successful)
                     if successful else 0.0)
    total_attempts = sum(r.attempts for r in results)

    print(f"  Wall time            : {elapsed:.3f}s")
    print(f"  Tasks submitted      : {n_jobs}")
    print(f"  Succeeded            : {len(successful)}")
    print(f"  Failed (permanent)   : {len(failed)}")
    print(f"    ├─ Timeouts        : {len(timeouts)}")
    print(f"    └─ Errors          : {len(errors)}")
    print(f"  Tasks retried ≥ 1×   : {len(retried)}")
    print(f"  Total attempts made  : {total_attempts}  (baseline: {n_jobs})")
    print(f"  Avg duration (ok)    : {avg_ok_dur*1000:.1f}ms per task")
    print(f"  Future outcomes      : {n_fut_ok} ok / {n_fut_err} raised")
    print(sep)

    # ── Sample successful results ──────────────────────────────────────────
    print("  First 6 successful results:")
    shown = 0
    for r in sorted(results, key=lambda x: x.task_id):
        if r.error is None:
            print(
                f"    task {r.task_id:>3}  value={r.value}  "
                f"attempts={r.attempts}  dur={r.duration*1000:.1f}ms"
            )
            shown += 1
            if shown >= 6:
                break

    # ── Sample failures ────────────────────────────────────────────────────
    if failed:
        print(f"  First {min(3, len(failed))} permanent failure(s):")
        for r in sorted(failed, key=lambda x: x.task_id)[: 3]:
            kind = "TimeoutError" if isinstance(r.error, asyncio.TimeoutError) else type(r.error).__name__
            print(
                f"    task {r.task_id:>3}  {kind}  "
                f"attempts={r.attempts}  dur={r.duration*1000:.1f}ms"
            )
    print(sep)


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.WARNING,
        format="%(levelname)-8s %(name)s: %(message)s",
    )
    asyncio.run(_run_demo())
