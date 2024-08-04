#ifndef MICRO_SCHED_SCHEDULER_H
#define MICRO_SCHED_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef int task_t;

typedef enum {
    TASK_READY      = 0,
    TASK_RUNNING    = 1,
    TASK_BLOCKED    = 2,
    TASK_SLEEPING   = 3,
    TASK_TERMINATED = 4
} TaskState;

#define MAX_TASKS       64
#define TASK_NAME_LEN   32
#define TIME_SLICE_MS   10

/* Forward declarations for synchronisation primitives */
struct Mutex;
struct Semaphore;

typedef struct Task {
    task_t      id;
    char        name[TASK_NAME_LEN];
    uint8_t     priority;           /* 0 = highest priority */
    uint8_t     base_priority;      /* saved base priority (for inheritance) */
    ucontext_t  ctx;
    void       *stack_base;
    size_t      stack_size;
    TaskState   state;
    uint64_t    wake_time;          /* monotonic ms, valid when SLEEPING */
    void       *blocked_on;         /* pointer to Mutex or Semaphore */

    /* Scheduling statistics */
    uint64_t    total_wait_ms;
    uint64_t    wait_start_ms;
    uint32_t    switch_count;

    /* Intrusive list link — used by ready / wait queues */
    struct Task *next;
} Task;

/* -------------------------------------------------------------------------
 * Scheduler API
 * ---------------------------------------------------------------------- */

/**
 * sched_init - initialise the scheduler.
 * @stack_pool_size: total bytes to reserve for task stacks.
 */
void sched_init(size_t stack_pool_size);

/**
 * sched_create - create a new task.
 * Returns a non-negative task id on success, -1 on failure.
 */
task_t sched_create(const char *name,
                    void (*fn)(void *),
                    void *arg,
                    uint8_t priority,
                    size_t stack_size);

/**
 * sched_run - enter the scheduler loop.  Never returns under normal
 * operation; returns only when every non-idle task has terminated.
 */
void sched_run(void);

/**
 * sched_yield - voluntarily give up the CPU.
 * The calling task re-enters the READY queue.
 */
void sched_yield(void);

/**
 * sched_sleep_ms - put the current task to sleep for at least @ms
 * milliseconds before being re-queued as READY.
 */
void sched_sleep_ms(uint32_t ms);

/**
 * sched_exit - terminate the current task.
 */
void sched_exit(void);

/**
 * sched_current - return the id of the currently running task.
 */
task_t sched_current(void);

/**
 * sched_get_task - return a pointer to the Task struct for @id.
 * Returns NULL if @id is invalid.
 */
Task *sched_get_task(task_t id);

/**
 * sched_now_ms - return monotonic time in milliseconds.
 */
uint64_t sched_now_ms(void);

/**
 * sched_print_stats - print per-task statistics to stdout.
 */
void sched_print_stats(void);

/* Internal: called by synchronisation primitives */
void _sched_block(void *sync_obj);
void _sched_unblock_one(Task *t);
void _sched_preempt(void);   /* called from SIGALRM handler */

#ifdef __cplusplus
}
#endif

#endif /* MICRO_SCHED_SCHEDULER_H */
