#ifndef MICRO_SCHED_SEMAPHORE_H
#define MICRO_SCHED_SEMAPHORE_H

#include <stdint.h>
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Counting semaphore.
 *
 * Tasks that call sem_wait() when the count is zero are queued and
 * woken in priority order when sem_post() is called.
 */
typedef struct Semaphore {
    int   count;        /* current count */
    Task *wait_head;    /* head of waiting-task queue */
} Semaphore;

/**
 * sem_init - initialise a semaphore with the given initial count.
 */
void sem_init(Semaphore *s, int initial_count);

/**
 * sem_wait - decrement the count, blocking if it is already zero.
 */
void sem_wait(Semaphore *s);

/**
 * sem_post - increment the count and wake one waiting task (if any).
 */
void sem_post(Semaphore *s);

/**
 * sem_trywait - try to decrement without blocking.
 * Returns 1 on success (count decremented), 0 if count was zero.
 */
int sem_trywait(Semaphore *s);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_SCHED_SEMAPHORE_H */
