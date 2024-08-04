#include "micro_sched/mutex.h"
#include "micro_sched/scheduler.h"

#include <assert.h>
#include <stddef.h>

void mutex_init(Mutex *m)
{
    m->owner     = NULL;
    m->wait_head = NULL;
    m->locked    = 0;
}

/* Insert into wait queue ordered by priority (ascending = highest first) */
static void waitq_insert(Task **head, Task *t)
{
    t->next = NULL;
    if (*head == NULL || t->priority < (*head)->priority) {
        t->next = *head;
        *head   = t;
        return;
    }
    Task *cur = *head;
    while (cur->next != NULL && cur->next->priority <= t->priority)
        cur = cur->next;
    t->next   = cur->next;
    cur->next = t;
}

static Task *waitq_pop(Task **head)
{
    if (*head == NULL) return NULL;
    Task *t = *head;
    *head   = t->next;
    t->next = NULL;
    return t;
}

void mutex_lock(Mutex *m)
{
    task_t cur_id = sched_current();
    assert(cur_id >= 0 && "mutex_lock called from outside a task");
    Task *caller = sched_get_task(cur_id);

    if (!m->locked) {
        /* Lock is free — acquire immediately */
        m->locked = 1;
        m->owner  = caller;
        return;
    }

    /* Priority inheritance: if caller has higher priority than owner,
     * boost owner's effective priority. */
    if (m->owner != NULL && caller->priority < m->owner->priority) {
        m->owner->priority = caller->priority;
    }

    /* Enqueue caller and block */
    waitq_insert(&m->wait_head, caller);
    _sched_block(m);
    /* Returned here after mutex_unlock woke us */
}

void mutex_unlock(Mutex *m)
{
    task_t cur_id = sched_current();
    assert(cur_id >= 0 && "mutex_unlock called from outside a task");
    Task *caller = sched_get_task(cur_id);
    assert(m->owner == caller && "mutex_unlock by non-owner");

    /* Restore owner's base priority */
    caller->priority = caller->base_priority;

    Task *next = waitq_pop(&m->wait_head);
    if (next == NULL) {
        m->locked = 0;
        m->owner  = NULL;
    } else {
        /* Hand off lock to next waiter without going through unlock state */
        m->owner = next;
        _sched_unblock_one(next);
    }
}

int mutex_trylock(Mutex *m)
{
    if (m->locked) return 0;

    task_t cur_id = sched_current();
    assert(cur_id >= 0);
    m->locked = 1;
    m->owner  = sched_get_task(cur_id);
    return 1;
}
