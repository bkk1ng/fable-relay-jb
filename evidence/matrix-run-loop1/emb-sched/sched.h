/*
 * sched.h — cooperative scheduler for ARM Cortex-M4, no HAL
 *
 * Tasks are stored in fixed slots. sched_tick() is called from SysTick_Handler.
 * Selection policy: longest-wait-first (task whose (now - last_run) is greatest).
 * Tick arithmetic uses subtraction to remain correct across uint32_t overflow.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of task slots. Adjust to fit your RAM budget. */
#define SCHED_MAX_TASKS  8U

/* Opaque handle returned by sched_add(). SCHED_INVALID_HANDLE on failure. */
typedef uint8_t sched_handle_t;
#define SCHED_INVALID_HANDLE  ((sched_handle_t)0xFFU)

/*
 * Task function signature.
 * The scheduler passes the slot index so a single function can service
 * multiple slots if needed.
 */
typedef void (*sched_fn_t)(sched_handle_t handle);

/* Internal task descriptor — exposed so callers can size static storage,
 * but fields should be treated as opaque outside sched.c. */
typedef struct {
    sched_fn_t  fn;           /* function to call; NULL = slot empty        */
    uint32_t    period_ticks; /* minimum ticks between successive calls      */
    uint32_t    last_run;     /* tick value at last execution                */
    bool        enabled;      /* false = skip without removing from table    */
} sched_task_t;

/* -------------------------------------------------------------------------
 * Scheduler API
 * ------------------------------------------------------------------------- */

/**
 * sched_init — zero the task table and reset the internal tick counter.
 * Call once before any other scheduler function.
 */
void sched_init(void);

/**
 * sched_add — register a new task.
 *
 * @param fn             Task function (must not be NULL).
 * @param period_ticks   Minimum ticks between calls (0 = run every tick).
 * @param enabled        Initial enabled state.
 * @return               Slot handle, or SCHED_INVALID_HANDLE if table is full.
 */
sched_handle_t sched_add(sched_fn_t fn, uint32_t period_ticks, bool enabled);

/**
 * sched_remove — free a slot by handle.
 * Safe to call from outside the scheduler (not from within a running task
 * removing itself — use sched_set_enabled instead to defer cleanly).
 */
void sched_remove(sched_handle_t handle);

/**
 * sched_set_enabled — enable or disable a task without removing it.
 */
void sched_set_enabled(sched_handle_t handle, bool enabled);

/**
 * sched_tick — advance the internal tick counter and dispatch at most one
 * ready task (longest-wait-first).
 *
 * Place a call to sched_tick() inside SysTick_Handler:
 *
 *   void SysTick_Handler(void) { sched_tick(); }
 *
 * Because this runs in an ISR the task functions must be short; for heavier
 * work set a flag in the task function and process it in the main loop.
 */
void sched_tick(void);

/**
 * sched_get_ticks — return the current tick counter value.
 * Useful for timestamping in task functions.
 */
uint32_t sched_get_ticks(void);

#endif /* SCHED_H */
