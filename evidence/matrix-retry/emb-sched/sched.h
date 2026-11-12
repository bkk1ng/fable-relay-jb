/*
 * sched.h — cooperative task scheduler for bare-metal ARM Cortex-M4
 *
 * Fixed-slot cooperative scheduler with longest-wait-first dispatch.
 * No HAL, no dynamic allocation, no preemption.
 *
 * Usage:
 *   sched_init();
 *   int led = sched_add(led_task, 100);   // every 100 ticks
 *   // SysTick_Handler() { sched_tick(); }
 *   for (;;) sched_dispatch();             // main loop
 *
 * tick maths is overflow-safe: all comparisons use unsigned subtraction
 * which wraps correctly modulo 2^32, so tick counter wrap is transparent.
 */
#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

/* Maximum number of task slots. Override by defining before #include. */
#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS 16
#endif

/* Task function: no arguments, no return. Keeps the slot struct small. */
typedef void (*sched_task_fn)(void);

/*
 * Task slot — the four requested fields plus an in_use flag for slot
 * management. Exposed so callers can statically allocate or inspect if
 * needed, but the fields should be treated as read-only outside sched.c.
 */
typedef struct {
    sched_task_fn fn;        /* function to call                 */
    uint32_t      period;    /* period in ticks (0 = every tick) */
    uint32_t      last_run;  /* tick value at last execution     */
    uint8_t       enabled;   /* 1 = eligible for dispatch        */
    uint8_t       in_use;    /* 1 = slot occupied                */
} sched_task_t;

/*
 * Global monotonic tick counter. Incremented by sched_tick() which is
 * called from the SysTick handler. A single aligned 32-bit read on
 * Cortex-M4 is atomic (one LDR), so thread-context readers can snapshot
 * it without disabling interrupts.
 */
extern volatile uint32_t sched_tick_count;

/* Reset all slots and zero the tick counter. Call before enabling SysTick. */
void sched_init(void);

/*
 * Add a task to a free slot.
 *   fn     — function pointer (must be non-NULL)
 *   period  — dispatch period in ticks; 0 means "run every dispatch"
 * Returns: slot index >= 0 on success, -1 if the table is full or fn is NULL.
 * The task starts enabled and its last_run is set to the current tick, so
 * the first execution happens after one full period.
 */
int sched_add(sched_task_fn fn, uint32_t period);

/*
 * Remove a task by slot index (the value returned by sched_add).
 * Safe to call with an invalid or already-removed index.
 */
void sched_remove(int slot);

/*
 * Advance the tick counter. Call this from the SysTick handler:
 *
 *   void SysTick_Handler(void) { sched_tick(); }
 *
 * On Cortex-M4 the increment runs in ISR context and cannot be
 * interrupted by thread code, so no critical section is needed.
 */
void sched_tick(void);

/*
 * Run one ready task. Call from the main loop. Among all enabled tasks
 * whose elapsed time has reached their period, the task that has been
 * waiting the longest (largest elapsed) runs first. After execution its
 * last_run is set to the current tick, rescheduling it for period ticks
 * later.
 *
 * Returns: 1 if a task ran, 0 if no task was ready.
 */
int sched_dispatch(void);

/*
 * Enable / disable an individual task by slot index without removing it.
 * Safe with invalid indices.
 */
void sched_enable(int slot);
void sched_disable(int slot);

#endif /* SCHED_H */
