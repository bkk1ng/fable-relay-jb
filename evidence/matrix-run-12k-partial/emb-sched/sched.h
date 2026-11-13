/*
 * sched.h — cooperative scheduler for ARM Cortex-M4, no HAL
 *
 * Usage:
 *   sched_tick()     — place in SysTick_Handler
 *   sched_dispatch() — place in the main loop (never in an ISR)
 *
 * sched_add / sched_remove must be called with interrupts disabled,
 * or before the scheduler is started (before SysTick is enabled).
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of concurrent task slots. */
#define SCHED_MAX_TASKS  8U

typedef struct {
    void     (*fn)(void);  /* task entry point; must run to completion */
    uint32_t  period;      /* run interval in ticks; 0 = every dispatch */
    uint32_t  last_run;    /* tick value recorded at last execution      */
    bool      enabled;     /* true when the slot is occupied and active  */
} sched_task_t;

/**
 * sched_init — zero the tick counter and clear all task slots.
 * Call once before any other sched_ function.
 */
void sched_init(void);

/**
 * sched_add — register a periodic task.
 *
 * @param fn            task function pointer (non-NULL)
 * @param period_ticks  run interval in ticks; 0 = run on every dispatch
 * @return slot index in [0, SCHED_MAX_TASKS), or -1 when no slot is free
 */
int8_t sched_add(void (*fn)(void), uint32_t period_ticks);

/**
 * sched_remove — unregister a task by its slot index.
 *
 * @param id  value returned by sched_add; silently ignored if out of range
 */
void sched_remove(int8_t id);

/**
 * sched_tick — advance the tick counter by one.
 * Place directly in SysTick_Handler; keep the body short.
 */
void sched_tick(void);

/**
 * sched_dispatch — run one ready task per call, choosing the task
 * that has waited longest (longest-wait-first policy).
 * Call from the main loop at whatever rate suits the application.
 */
void sched_dispatch(void);

/**
 * sched_ticks — return a snapshot of the current tick counter.
 * The value may advance asynchronously between the read and its use.
 */
uint32_t sched_ticks(void);

#endif /* SCHED_H */
