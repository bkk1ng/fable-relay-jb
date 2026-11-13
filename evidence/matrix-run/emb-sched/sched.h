/**
 * sched.h — cooperative scheduler for ARM Cortex-M4, no HAL
 *
 * Wire-up:
 *   1. Configure SysTick for your desired tick rate (e.g. 1 ms).
 *   2. Call sched_tick() from SysTick_Handler.
 *   3. Call sched_run() from the main loop — it executes at most one
 *      ready task per call, chosen by longest-wait-first.
 *
 * Overflow safety:
 *   The 32-bit tick counter is allowed to wrap freely.  All elapsed-time
 *   comparisons use unsigned subtraction, which gives the correct positive
 *   result across the wrap boundary.
 *
 * SCHED_MAX_TASKS can be overridden on the compiler command line or before
 * this header is included.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdbool.h>
#include <stdint.h>

#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS 16
#endif

typedef void (*sched_fn_t)(void);

typedef struct {
    sched_fn_t fn;       /**< Entry point.  NULL means the slot is free.      */
    uint32_t   period;   /**< Minimum ticks between consecutive runs.          */
    uint32_t   last_run; /**< Tick value recorded at the end of the last run.  */
    bool       enabled;  /**< false suspends the task without freeing its slot.*/
} sched_task_t;

/**
 * sched_add - Register a periodic task.
 *
 * @fn      Function to call; must not be NULL.
 * @period  Ticks between runs.  0 means run every tick.
 *
 * last_run is initialised to the current tick so the task does not fire
 * immediately — it waits a full period before its first execution.
 *
 * Returns the slot id in [0, SCHED_MAX_TASKS) on success, or -1 when the
 * table is full.
 */
int sched_add(sched_fn_t fn, uint32_t period);

/**
 * sched_remove - Free a task slot.
 *
 * @id  Slot id returned by sched_add().  Out-of-range values are ignored.
 */
void sched_remove(int id);

/**
 * sched_tick - Advance the scheduler clock by one tick.
 *
 * Call this directly from SysTick_Handler.  Keeps execution time minimal
 * so ISR latency stays bounded.
 */
void sched_tick(void);

/**
 * sched_run - Dispatch one ready task from the main loop.
 *
 * Scans all occupied, enabled slots and executes the task with the greatest
 * elapsed ticks since its last run (longest-wait-first).  If no task is due
 * the function returns immediately.
 *
 * Tasks run in thread context, not in interrupt context — this is what makes
 * the scheduler cooperative.
 */
void sched_run(void);

/**
 * sched_get_ticks - Return a snapshot of the current tick count.
 */
uint32_t sched_get_ticks(void);

#endif /* SCHED_H */
