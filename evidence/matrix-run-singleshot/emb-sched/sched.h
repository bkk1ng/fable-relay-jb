/**
 * sched.h — cooperative tick scheduler for ARM Cortex-M4, no HAL.
 *
 * Usage
 * -----
 *   1. Call sched_init() before enabling SysTick.
 *   2. Configure SysTick to call sched_tick() from SysTick_Handler.
 *   3. Register tasks with sched_add(); hold the returned slot index to
 *      remove or suspend later.
 *
 * Dispatch model
 * --------------
 *   sched_tick() runs the single most-overdue enabled task per tick.
 *   "Most overdue" means the largest (now - last_run) among all tasks
 *   where that value >= period (overflow-safe unsigned subtraction).
 *   Tasks execute in ISR context (Handler mode, MSP); keep them short.
 *
 * Overflow safety
 * ---------------
 *   The tick counter and all deadline comparisons use uint32_t unsigned
 *   subtraction.  (uint32_t)(now - last_run) wraps correctly through the
 *   full 32-bit rollover provided no task misses more than 2^32 − 1 ticks
 *   (~49 days at 1 kHz).
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>

#ifndef SCHED_MAX_TASKS
/** Maximum number of simultaneous task slots.  Override via -DSCHED_MAX_TASKS=N. */
#define SCHED_MAX_TASKS  8
#endif

/** Single task descriptor stored in the scheduler table. */
typedef struct {
    void     (*fn)(void);  /**< Entry point; NULL marks the slot as free.          */
    uint32_t  period;      /**< Repeat interval in SysTick ticks.                  */
    uint32_t  last_run;    /**< Tick value recorded at the most recent execution.   */
    bool      enabled;     /**< false suspends dispatch without freeing the slot.   */
} sched_task_t;

/**
 * @brief  Zero the task table and tick counter.
 *         Call once before any other sched_* function and before SysTick
 *         is enabled.
 */
void sched_init(void);

/**
 * @brief  Register a periodic task.
 *
 * @param  fn            Callback to invoke; must not be NULL.
 * @param  period_ticks  Ticks between successive executions.  0 fires every tick.
 *
 * @return Non-negative slot index on success, -1 when the table is full.
 */
int sched_add(void (*fn)(void), uint32_t period_ticks);

/**
 * @brief  Remove a task and free its slot for reuse.
 *         Safe to call from thread context while SysTick is active.
 *
 * @param  slot  Value returned by a prior sched_add() call.
 */
void sched_remove(int slot);

/**
 * @brief  Suspend or resume a task without releasing its slot.
 *         Re-enabling resets last_run to now so the task fires one full
 *         period later rather than immediately.
 *
 * @param  slot     Value returned by a prior sched_add() call.
 * @param  enabled  true to resume, false to suspend.
 */
void sched_enable(int slot, bool enabled);

/**
 * @brief  Advance the tick counter and dispatch the longest-waiting ready task.
 *
 *         Wire this directly to SysTick_Handler:
 *
 *             void SysTick_Handler(void) { sched_tick(); }
 *
 * @note   Task callbacks run in Handler mode on the MSP.  Do not block,
 *         do not call functions that re-enable interrupts, and keep stack
 *         depth predictable.
 */
void sched_tick(void);

#endif /* SCHED_H */
