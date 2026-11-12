/*
 * sched.c — cooperative task scheduler for bare-metal ARM Cortex-M4
 *
 * Compile with any Cortex-M4 toolchain (arm-none-eabi-gcc etc.).
 * No external dependencies beyond <stdint.h> and the matching header.
 *
 * Design notes:
 *
 *  Tick counting
 *    sched_tick_count is a 32-bit unsigned counter incremented once per
 *    SysTick. At a 1 kHz tick rate it wraps after ~49.7 days. All
 *    elapsed-time computations use unsigned subtraction:
 *
 *        elapsed = tick_now - last_run
 *
 *    This is overflow-safe: if tick_now wrapped past last_run, the
 *    subtraction still yields the correct (small, positive) elapsed
 *    value because unsigned arithmetic is modulo 2^32. No special
 *    handling is needed at wraparound.
 *
 *  Dispatch policy — longest-wait-first
 *    Each dispatch call scans all slots, computes elapsed for each
 *    enabled task, and picks the one with the largest elapsed that
 *    also meets the due condition (elapsed >= period). That task runs
 *    to completion (cooperative — no preemption), and its last_run is
 *    reset to the tick snapshot taken at the start of dispatch. Only
 *    one task runs per call; the main loop should call sched_dispatch()
 *    repeatedly.
 *
 *  ISR / thread safety
 *    sched_tick_count is read with a single 32-bit load in dispatch,
 *    which is atomic on Cortex-M4. The SysTick handler only writes it.
 *    No critical section is required for the standard single-SysTick
 *    setup. If sched_tick were called from multiple ISRs at different
 *    priorities, the lower-priority call would need interrupt masking.
 */

#include "sched.h"

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

volatile uint32_t sched_tick_count;

static sched_task_t sched_tasks[SCHED_MAX_TASKS];

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

void sched_init(void)
{
    sched_tick_count = 0;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        sched_tasks[i].fn       = (sched_task_fn)0;
        sched_tasks[i].period   = 0;
        sched_tasks[i].last_run = 0;
        sched_tasks[i].enabled  = 0;
        sched_tasks[i].in_use   = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Add / remove                                                       */
/* ------------------------------------------------------------------ */

int sched_add(sched_task_fn fn, uint32_t period)
{
    if (fn == (sched_task_fn)0)
        return -1;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (sched_tasks[i].in_use)
            continue;

        sched_tasks[i].fn       = fn;
        sched_tasks[i].period   = period;
        sched_tasks[i].last_run = sched_tick_count;   /* start fresh */
        sched_tasks[i].enabled  = 1;
        sched_tasks[i].in_use   = 1;
        return i;
    }

    return -1;   /* table full */
}

void sched_remove(int slot)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS)
        return;

    sched_tasks[slot].in_use  = 0;
    sched_tasks[slot].enabled = 0;
    sched_tasks[slot].fn      = (sched_task_fn)0;
}

void sched_enable(int slot)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS)
        return;
    if (!sched_tasks[slot].in_use)
        return;
    sched_tasks[slot].enabled = 1;
}

void sched_disable(int slot)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS)
        return;
    sched_tasks[slot].enabled = 0;
}

/* ------------------------------------------------------------------ */
/* Tick (ISR context)                                                 */
/* ------------------------------------------------------------------ */

void sched_tick(void)
{
    sched_tick_count++;
}

/* ------------------------------------------------------------------ */
/* Dispatch (thread context)                                          */
/* ------------------------------------------------------------------ */

int sched_dispatch(void)
{
    /*
     * Snapshot the tick counter once. A 32-bit aligned read on
     * Cortex-M4 is a single LDR, so this cannot be torn by the
     * SysTick ISR. All elapsed calculations use this snapshot so a
     * tick increment mid-scan cannot change decisions inconsistently.
     */
    uint32_t tick = sched_tick_count;

    int      best       = -1;
    uint32_t best_wait  = 0;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (!sched_tasks[i].in_use || !sched_tasks[i].enabled)
            continue;

        /*
         * Overflow-safe elapsed calculation.
         *
         * If tick < last_run the counter has wrapped. Unsigned
         * subtraction still produces the correct positive delta:
         *
         *   elapsed = (UINT32_MAX - last_run) + tick + 1
         *
         * which equals (tick - last_run) mod 2^32. No branch needed.
         */
        uint32_t elapsed = tick - sched_tasks[i].last_run;

        if (elapsed >= sched_tasks[i].period) {
            /* longest-wait-first: track the largest elapsed */
            if (best < 0 || elapsed > best_wait) {
                best      = i;
                best_wait = elapsed;
            }
        }
    }

    if (best < 0)
        return 0;   /* nothing ready */

    /*
     * Update last_run before invoking the function so the task can
     * safely call sched_add / sched_remove / sched_disable on itself
     * without corrupting state we still need. Using the pre-snapshot
     * tick (not a fresh read) keeps the schedule deterministic.
     */
    sched_tasks[best].last_run = tick;
    sched_tasks[best].fn();

    return 1;
}
