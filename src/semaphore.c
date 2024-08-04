#include "micro_sched/semaphore.h"
#include "micro_sched/scheduler.h"

#include <assert.h>
#include <stddef.h>

void sem_init(Semaphore *s, int initial_count)
{
    assert(initial_count >= 0);
    s->count     = initial_count;
    s->wait_head = NULL;
}

/* Insert into wait queue ordered by priority */
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

void sem_wait(Semaphore *s)
{
    if (s->count > 0) {
        s->count--;
        return;
    }

    /* Block until a post wakes us */
    task_t cur_id = sched_current();
    assert(cur_id >= 0 && "sem_wait called from outside a task");
    Task *caller = sched_get_task(cur_id);
    waitq_insert(&s->wait_head, caller);
    _sched_block(s);
    /* count was already decremented by the posting task */
}

void sem_post(Semaphore *s)
{
    Task *waiter = waitq_pop(&s->wait_head);
    if (waiter == NULL) {
        s->count++;
    } else {
        /* Wake the waiter; leave count unchanged (it was 0 → still 0) */
        _sched_unblock_one(waiter);
    }
}

int sem_trywait(Semaphore *s)
{
    if (s->count <= 0) return 0;
    s->count--;
    return 1;
}
