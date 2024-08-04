#ifndef MICRO_SCHED_MUTEX_H
#define MICRO_SCHED_MUTEX_H

#include <stdint.h>
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mutex with priority inheritance.
 *
 * When a higher-priority task blocks on a locked mutex, the owner's
 * effective priority is raised to the waiter's priority so that it
 * pre-empts lower-priority tasks and releases the lock sooner.
 */
typedef struct Mutex {
    Task   *owner;          /* task that currently holds the lock, or NULL */
    Task   *wait_head;      /* head of waiting-task queue (FIFO w/ priority) */
    int     locked;         /* 1 if locked */
} Mutex;

/**
 * mutex_init - initialise a Mutex to the unlocked state.
 */
void mutex_init(Mutex *m);

/**
 * mutex_lock - acquire the mutex, blocking until available.
 * Implements priority inheritance: if the caller has higher priority
 * (lower numeric value) than the current owner, the owner's effective
 * priority is raised.
 */
void mutex_lock(Mutex *m);

/**
 * mutex_unlock - release the mutex.
 * Restores the owner's original base priority and wakes the highest-
 * priority waiting task (if any).
 */
void mutex_unlock(Mutex *m);

/**
 * mutex_trylock - try to acquire the mutex without blocking.
 * Returns 1 on success, 0 if already locked.
 */
int mutex_trylock(Mutex *m);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_SCHED_MUTEX_H */
