/*
 * sched.h — cooperative task scheduler for ARM Cortex-M4, no HAL.
 *
 * Usage
 * -----
 *   1. Call sched_init() once at startup.
 *   2. Register tasks with sched_add().
 *   3. Call sched_tick() from SysTick_Handler().
 *   4. Use sched_add/sched_remove/sched_set_enabled from main or task code.
 *
 * Tick arithmetic is unsigned 32-bit wrap-safe throughout; the scheduler
 * remains correct across the uint32_t rollover without special casing.
 *
 * Among all enabled tasks whose period has elapsed, the task that has
 * waited the longest (greatest elapsed ticks) runs first.
 *
 * sched_add() and sched_remove() protect the task table with a brief
 * CPSID/CPSIE critical section so they are safe to call from main-thread
 * context while SysTick is active.  Task callbacks run inside the SysTick
 * ISR and must therefore be short and non-blocking.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

/* Override at compile time: -DSCHED_MAX_TASKS=16 */
#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS 8
#endif

/* Task callback signature. */
typedef void (*sched_task_fn_t)(void);

/* Task descriptor — one per fixed slot. */
typedef struct {
    sched_task_fn_t fn;        /* callback; NULL marks an empty slot  */
    uint32_t        period;    /* execution period in ticks (> 0)     */
    uint32_t        last_run;  /* tick value at last execution        */
    uint8_t         enabled;   /* non-zero = active, 0 = suspended    */
} sched_task_t;

/*
 * sched_init — zero the task table and tick counter.
 * Call once before any other sched_* function.
 */
void sched_init(void);

/*
 * sched_add — register a task.
 *
 *   fn      callback to invoke (must not be NULL)
 *   period  execution period in ticks (must be > 0)
 *   enabled initial enabled state (non-zero = active)
 *
 * Returns the slot index [0, SCHED_MAX_TASKS) on success, or -1 when no
 * slot is free or the arguments are invalid.
 *
 * Safe to call from main-thread context while SysTick is running.
 */
int sched_add(sched_task_fn_t fn, uint32_t period, uint8_t enabled);

/*
 * sched_remove — deregister the task at the given slot index.
 *
 * Returns 0 on success, -1 when the index is out of range or the slot
 * is already empty.
 *
 * Safe to call from main-thread context while SysTick is running.
 */
int sched_remove(int slot);

/*
 * sched_set_enabled — suspend or resume the task at slot without removing it.
 *
 * When re-enabled, last_run is reset to the current tick so the task does
 * not fire immediately to compensate for time spent suspended.
 *
 * Returns 0 on success, -1 on an invalid or empty slot.
 */
int sched_set_enabled(int slot, uint8_t enabled);

/*
 * sched_tick — advance the scheduler by one tick and run any due tasks.
 *
 * Place a call to this function inside SysTick_Handler().  Tasks run in
 * longest-wait-first order inside the ISR; keep callbacks short.
 */
void sched_tick(void);

/*
 * sched_get_ticks — return the current tick counter value.
 */
uint32_t sched_get_ticks(void);

#endif /* SCHED_H */
