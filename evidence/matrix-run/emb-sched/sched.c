/**
 * sched.c — cooperative scheduler implementation for ARM Cortex-M4, no HAL
 *
 * Tick counter is a plain uint32_t incremented in SysTick_Handler via
 * sched_tick().  All elapsed-time comparisons use unsigned subtraction so
 * they remain correct across the natural 32-bit wrap boundary without any
 * special handling.
 *
 * Selection policy: longest-wait-first.
 * Among all occupied, enabled tasks whose elapsed ticks >= period, the one
 * with the largest (now - last_run) value wins.  Ties are broken by the
 * lower slot index.
 */

#include "sched.h"

#include <stddef.h>   /* NULL */

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static volatile uint32_t s_ticks;                    /* free-running counter */
static sched_task_t      s_tasks[SCHED_MAX_TASKS];   /* task table           */

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int sched_add(sched_fn_t fn, uint32_t period)
{
    if (fn == NULL) {
        return -1;
    }

    for (int i = 0; i < SCHED_MAX_TASKS; ++i) {
        if (s_tasks[i].fn == NULL) {
            s_tasks[i].fn       = fn;
            s_tasks[i].period   = period;
            /*
             * Initialise last_run to now so the task waits a full period
             * before its first execution.  Read s_ticks once; the value
             * does not need to be atomic here because a one-tick error on
             * the very first run is acceptable.
             */
            s_tasks[i].last_run = s_ticks;
            s_tasks[i].enabled  = true;
            return i;
        }
    }

    return -1; /* table full */
}

void sched_remove(int id)
{
    if (id < 0 || id >= SCHED_MAX_TASKS) {
        return;
    }

    /*
     * Zero the entire slot.  Setting fn to NULL is what marks the slot free;
     * the other fields are cleared only for tidiness.
     */
    s_tasks[id].fn       = NULL;
    s_tasks[id].period   = 0;
    s_tasks[id].last_run = 0;
    s_tasks[id].enabled  = false;
}

void sched_tick(void)
{
    /*
     * Called from SysTick_Handler.  Keep this minimal — no decisions, just
     * a counter increment.  Unsigned overflow wraps correctly.
     */
    ++s_ticks;
}

void sched_run(void)
{
    /*
     * Take a single consistent snapshot of the tick counter.  Any ticks that
     * arrive via SysTick_Handler during the scan or during task execution are
     * accounted for on the next sched_run() call.
     */
    uint32_t now = s_ticks;

    int      best_id      = -1;
    uint32_t best_elapsed = 0;

    for (int i = 0; i < SCHED_MAX_TASKS; ++i) {
        if (s_tasks[i].fn == NULL || !s_tasks[i].enabled) {
            continue;
        }

        /*
         * Unsigned subtraction is overflow-safe: if now has wrapped past
         * last_run the result is still the correct positive elapsed count.
         *
         * Example with 32-bit wrap:
         *   last_run = 0xFFFFFFFF, now = 0x00000003
         *   elapsed  = 0x00000003 - 0xFFFFFFFF = 0x00000004  (correct: 4 ticks)
         */
        uint32_t elapsed = now - s_tasks[i].last_run;

        if (elapsed < s_tasks[i].period) {
            continue; /* not yet due */
        }

        /* Longest-wait-first: keep the task with the greatest elapsed time. */
        if (best_id < 0 || elapsed > best_elapsed) {
            best_id      = i;
            best_elapsed = elapsed;
        }
    }

    if (best_id < 0) {
        return; /* nothing ready */
    }

    /*
     * Run the selected task, then record the dispatch time.  Recording *now*
     * rather than the current tick preserves deterministic period cadence —
     * any ticks that accumulated during task execution are treated as normal
     * latency and do not permanently shift the schedule.
     */
    s_tasks[best_id].fn();
    s_tasks[best_id].last_run = now;
}

uint32_t sched_get_ticks(void)
{
    return s_ticks;
}
