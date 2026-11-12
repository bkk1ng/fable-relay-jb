/**
 * sched.h — cooperative tick-driven scheduler for ARM Cortex-M4
 *
 * Usage sketch
 * ------------
 *   SysTick_Config(SystemCoreClock / 1000);   // 1 ms ticks
 *
 *   void SysTick_Handler(void) { sched_tick(); }
 *
 *   int main(void) {
 *       sched_init();
 *       sched_add(task_a, 100);   // every 100 ms
 *       sched_add(task_b, 500);   // every 500 ms
 *       for (;;) sched_dispatch();
 *   }
 *
 * Dispatch policy
 * ---------------
 *   Each call to sched_dispatch() runs at most one task — whichever enabled
 *   task has waited the greatest number of ticks past its period.  Call it
 *   in a tight main loop so overdue tasks are caught quickly.
 *
 * Tick overflow
 * -------------
 *   All elapsed-time comparisons use unsigned subtraction, which wraps
 *   correctly on 32-bit rollover.  At 1 kHz the counter rolls over after
 *   ~49 days; periods far shorter than that are always handled correctly.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>

/** Maximum number of concurrent task slots.  Override with -DSCHED_MAX_TASKS=N. */
#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS 8
#endif

/** Task function signature. */
typedef void (*sched_fn_t)(void);

/** One fixed task slot. */
typedef struct {
    sched_fn_t fn;          /**< Task entry point; NULL means slot is free.   */
    uint32_t   period;      /**< Run period in ticks (must be > 0).            */
    uint32_t   last_run;    /**< Tick value recorded when the task last ran.   */
    bool       enabled;     /**< When false the slot exists but is not run.    */
} sched_task_t;

/**
 * sched_init — initialise scheduler state.
 * Call once before any other sched_* function, before enabling SysTick.
 */
void sched_init(void);

/**
 * sched_add — register a periodic task.
 *
 * @param fn            Function to call; must not be NULL.
 * @param period_ticks  Minimum ticks between successive runs; must be > 0.
 * @return              Slot index in [0, SCHED_MAX_TASKS), or -1 when full.
 */
int sched_add(sched_fn_t fn, uint32_t period_ticks);

/**
 * sched_remove — free a slot by index.
 * The slot may be reused by a subsequent sched_add().
 *
 * @param slot  Value previously returned by sched_add().
 */
void sched_remove(int slot);

/**
 * sched_tick — advance the system tick counter by one.
 * Call exclusively from SysTick_Handler().  Does not run any task.
 */
void sched_tick(void);

/**
 * sched_dispatch — run the longest-waiting due task, if any.
 * Call repeatedly from the main loop.  Runs at most one task per call.
 */
void sched_dispatch(void);

/**
 * sched_enable — suspend or resume a slot without removing it.
 *
 * @param slot  Value previously returned by sched_add().
 * @param en    true to enable, false to suspend.
 */
void sched_enable(int slot, bool en);

/**
 * sched_ticks — return a snapshot of the current tick counter.
 * Useful for application-level timeouts.
 */
uint32_t sched_ticks(void);

#endif /* SCHED_H */
