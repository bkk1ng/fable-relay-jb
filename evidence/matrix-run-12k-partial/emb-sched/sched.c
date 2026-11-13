/*
 * sched.c — cooperative scheduler for ARM Cortex-M4, no HAL
 *
 * Designed to be driven by SysTick:
 *   SysTick_Handler  ->  sched_tick();
 *   main()           ->  for (;;) sched_dispatch();
 *
 * Overflow-safe tick maths:
 *   The elapsed time is computed as (now - last_run) using unsigned 32-bit
 *   subtraction.  When the tick counter wraps from 0xFFFFFFFF back to 0,
 *   the modular arithmetic still yields the correct elapsed count, so long
 *   as no task waits longer than 2^32 ticks (~49.7 days at 1 kHz) between
 *   dispatches — an assumption that holds for any realistic firmware.
 *
 * Longest-wait-first:
 *   Among all tasks whose elapsed time has reached (or exceeded) their
 *   period, the one with the greatest elapsed time runs first.  This
 *   naturally starves no task: a task that was skipped on a busy pass
 *   accumulates elapsed time and rises in priority for the next pass.
 */

#include "sched.h"

/* ---- internal state -------------------------------------------------- */

static sched_task_t tasks[SCHED_MAX_TASKS];

/* Volatile: written by sched_tick() inside SysTick, read by sched_dispatch()
 * in the main loop.  A 32-bit aligned read is atomic on Cortex-M4, so the
 * snapshot taken in sched_dispatch() is internally consistent. */
static volatile uint32_t tick_count;

/* ---- public API ------------------------------------------------------- */

void sched_init(void)
{
    tick_count = 0U;

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        tasks[i].fn       = (void (*)(void))0;
        tasks[i].period   = 0U;
        tasks[i].last_run = 0U;
        tasks[i].enabled  = false;
    }
}

int8_t sched_add(void (*fn)(void), uint32_t period_ticks)
{
    if (fn == (void (*)(void))0)
        return -1;

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        if (!tasks[i].enabled) {
            tasks[i].fn       = fn;
            tasks[i].period   = period_ticks;
            tasks[i].last_run = tick_count;   /* count from registration time */
            tasks[i].enabled  = true;
            return (int8_t)i;
        }
    }
    return -1;  /* all slots occupied */
}

void sched_remove(int8_t id)
{
    if (id < 0 || id >= (int8_t)SCHED_MAX_TASKS)
        return;

    tasks[id].enabled = false;
    tasks[id].fn      = (void (*)(void))0;
}

void sched_tick(void)
{
    tick_count++;
}

uint32_t sched_ticks(void)
{
    return tick_count;
}

void sched_dispatch(void)
{
    /* Snapshot the free-running counter once.  Using a single consistent
     * "now" across all comparisons avoids TOCTOU churn if SysTick fires
     * mid-scan, and keeps every elapsed value in the same time frame. */
    uint32_t now = tick_count;

    int8_t  best        = -1;
    uint32_t best_elapsed = 0U;

    for (uint8_t i = 0U; i < SCHED_MAX_TASKS; ++i) {
        if (!tasks[i].enabled)
            continue;

        /* Unsigned subtraction wraps correctly across the 2^32 boundary.
         * A period of 0 means "run on every dispatch" (elapsed >= 0 is
         * always true for an unsigned value). */
        uint32_t elapsed = now - tasks[i].last_run;

        if (elapsed >= tasks[i].period) {
            /* Longest-wait-first: among ready tasks, keep the one whose
             * elapsed time is greatest.  Strict ">" on subsequent
             * comparisons gives deterministic tie-breaking by slot index
             * (lower index wins ties). */
            if (best < 0 || elapsed > best_elapsed) {
                best         = (int8_t)i;
                best_elapsed = elapsed;
            }
        }
    }

    if (best >= 0) {
        /* Stamp last_run before invoking the task so the period is measured
         * from dispatch-start, not from function-return.  This keeps the
         * effective period stable even when a task body runs long. */
        tasks[best].last_run = now;
        tasks[best].fn();
    }
}
