/*
 * sched.c — cooperative scheduler for ARM Cortex-M4, no HAL
 *
 * Tick counter is uint32_t; all elapsed-time comparisons use subtraction
 * so they remain correct across the natural wrap at 0xFFFFFFFF.
 *
 * Selection policy: among all ready tasks (enabled && elapsed >= period),
 * dispatch the one with the greatest elapsed time (longest wait first).
 * If no task is ready, sched_tick() returns immediately.
 *
 * Call sched_tick() from SysTick_Handler only. All other API functions
 * are safe to call from the main loop or from task functions, with the
 * exception noted in sched_remove().
 */

#include "sched.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */

static sched_task_t  s_tasks[SCHED_MAX_TASKS];
static uint32_t      s_ticks;   /* incremented once per sched_tick() call */

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void sched_init(void)
{
    s_ticks = 0U;

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        s_tasks[i].fn           = NULL;
        s_tasks[i].period_ticks = 0U;
        s_tasks[i].last_run     = 0U;
        s_tasks[i].enabled      = false;
    }
}

sched_handle_t sched_add(sched_fn_t fn, uint32_t period_ticks, bool enabled)
{
    if (fn == NULL) {
        return SCHED_INVALID_HANDLE;
    }

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        if (s_tasks[i].fn == NULL) {
            s_tasks[i].fn           = fn;
            s_tasks[i].period_ticks = period_ticks;
            /*
             * Initialise last_run so the task is immediately eligible on
             * the next tick: set it to (now - period) which, via unsigned
             * wrap, correctly makes elapsed == period on the first check.
             */
            s_tasks[i].last_run     = s_ticks - period_ticks;
            s_tasks[i].enabled      = enabled;
            return (sched_handle_t)i;
        }
    }

    return SCHED_INVALID_HANDLE;    /* table full */
}

void sched_remove(sched_handle_t handle)
{
    if (handle >= SCHED_MAX_TASKS) {
        return;
    }

    s_tasks[handle].fn      = NULL;
    s_tasks[handle].enabled = false;
}

void sched_set_enabled(sched_handle_t handle, bool enabled)
{
    if (handle >= SCHED_MAX_TASKS) {
        return;
    }

    if (s_tasks[handle].fn != NULL) {
        s_tasks[handle].enabled = enabled;
    }
}

void sched_tick(void)
{
    /* Advance the global counter — wraps naturally at 2^32. */
    s_ticks++;

    /*
     * Longest-wait-first selection pass.
     *
     * elapsed = s_ticks - last_run  (unsigned subtraction handles wrap)
     *
     * A task is ready when it is enabled, its slot is occupied, and at
     * least one full period has elapsed since it last ran.  Among all
     * ready tasks we pick the one with the highest elapsed value.
     *
     * We dispatch at most one task per tick (cooperative; the task runs
     * to completion before returning here).
     */
    uint32_t        best_elapsed = 0U;
    sched_handle_t  best_idx     = SCHED_INVALID_HANDLE;

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        if ((s_tasks[i].fn == NULL) || (!s_tasks[i].enabled)) {
            continue;
        }

        /* Overflow-safe elapsed calculation. */
        uint32_t elapsed = s_ticks - s_tasks[i].last_run;

        if (elapsed >= s_tasks[i].period_ticks) {
            if (elapsed > best_elapsed) {
                best_elapsed = elapsed;
                best_idx     = (sched_handle_t)i;
            }
        }
    }

    if (best_idx == SCHED_INVALID_HANDLE) {
        return;     /* nothing ready this tick */
    }

    /*
     * Record the run time before calling the function so that the task's
     * own period is measured from the dispatch point, not from whenever
     * the function returns. This keeps long-running tasks from drifting
     * their own deadline forward on every execution.
     */
    s_tasks[best_idx].last_run = s_ticks;
    s_tasks[best_idx].fn(best_idx);
}

uint32_t sched_get_ticks(void)
{
    return s_ticks;
}
