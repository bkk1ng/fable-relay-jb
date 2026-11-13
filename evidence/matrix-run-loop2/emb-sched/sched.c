/*
 * sched.c — cooperative task scheduler for ARM Cortex-M4, no HAL.
 *
 * Tick arithmetic
 * ---------------
 * All elapsed-time calculations use unsigned subtraction:
 *
 *   elapsed = now - last_run
 *
 * This is correct across the uint32_t rollover because unsigned wrap is
 * defined behaviour in C.  For example, if now = 0x00000003 and
 * last_run = 0xFFFFFFFE, elapsed = 0x00000005 — exactly five ticks.
 *
 * Critical sections
 * -----------------
 * sched_add(), sched_remove(), and sched_set_enabled() may be called from
 * main-thread context while sched_tick() runs from SysTick.  A minimal
 * CPSID I / CPSIE I pair guards the shared table.  sched_tick() itself runs
 * entirely inside the ISR and needs no additional protection.
 */

#include "sched.h"
#include <string.h>   /* memset */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Disable / enable IRQs on Cortex-M4 using inline assembly. */
static inline void sched_irq_disable(void)
{
    __asm volatile ("cpsid i" : : : "memory");
}

static inline void sched_irq_enable(void)
{
    __asm volatile ("cpsie i" : : : "memory");
}

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static sched_task_t s_tasks[SCHED_MAX_TASKS];
static uint32_t     s_ticks;   /* monotonic tick counter, wraps at 2^32 */

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void sched_init(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
    s_ticks = 0U;
}

int sched_add(sched_task_fn_t fn, uint32_t period, uint8_t enabled)
{
    int slot = -1;
    int i;

    if (fn == NULL || period == 0U) {
        return -1;
    }

    sched_irq_disable();

    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        if (s_tasks[i].fn == NULL) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        s_tasks[slot].fn       = fn;
        s_tasks[slot].period   = period;
        s_tasks[slot].last_run = s_ticks;   /* start measuring from now    */
        s_tasks[slot].enabled  = enabled;
    }

    sched_irq_enable();

    return slot;
}

int sched_remove(int slot)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS) {
        return -1;
    }

    sched_irq_disable();

    if (s_tasks[slot].fn == NULL) {
        sched_irq_enable();
        return -1;
    }

    memset(&s_tasks[slot], 0, sizeof(sched_task_t));

    sched_irq_enable();

    return 0;
}

int sched_set_enabled(int slot, uint8_t enabled)
{
    if (slot < 0 || slot >= SCHED_MAX_TASKS) {
        return -1;
    }

    sched_irq_disable();

    if (s_tasks[slot].fn == NULL) {
        sched_irq_enable();
        return -1;
    }

    if (enabled && !s_tasks[slot].enabled) {
        /*
         * Reset last_run so the task does not immediately fire to compensate
         * for the time it spent suspended — it waits a full period first.
         */
        s_tasks[slot].last_run = s_ticks;
    }
    s_tasks[slot].enabled = enabled;

    sched_irq_enable();

    return 0;
}

void sched_tick(void)
{
    int      i;
    int      best;
    uint32_t best_elapsed;
    uint32_t elapsed;

    s_ticks++;

    /*
     * Longest-wait-first selection: scan every enabled task whose period has
     * elapsed and pick the one with the greatest elapsed ticks.  Run it once,
     * then return — the next SysTick will dispatch the next candidate.  This
     * keeps ISR latency bounded even when several tasks are overdue.
     */
    best         = -1;
    best_elapsed = 0U;

    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        if (s_tasks[i].fn == NULL || !s_tasks[i].enabled) {
            continue;
        }

        /*
         * Unsigned subtraction gives the correct elapsed time even when
         * s_ticks has wrapped past s_tasks[i].last_run.
         */
        elapsed = s_ticks - s_tasks[i].last_run;

        if (elapsed >= s_tasks[i].period) {
            if (best < 0 || elapsed > best_elapsed) {
                best         = i;
                best_elapsed = elapsed;
            }
        }
    }

    if (best >= 0) {
        /*
         * Advance last_run by exactly one period (not to s_ticks) so that
         * accumulated drift does not permanently shift the schedule when a
         * task fires late.
         */
        s_tasks[best].last_run += s_tasks[best].period;
        s_tasks[best].fn();
    }
}

uint32_t sched_get_ticks(void)
{
    return s_ticks;
}
