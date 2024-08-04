#define _GNU_SOURCE
#include "micro_sched/scheduler.h"
#include "micro_sched/mem_pool.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>

/* -------------------------------------------------------------------------
 * Internal globals
 * ---------------------------------------------------------------------- */

static Task      g_tasks[MAX_TASKS];
static int       g_task_count   = 0;
static task_t    g_current      = -1;   /* currently running task id */
static ucontext_t g_sched_ctx;          /* scheduler / dispatcher context */

/* Stack pool */
static uint8_t  *g_stack_pool   = NULL;
static size_t    g_pool_used    = 0;
static size_t    g_pool_size    = 0;

/* Global statistics */
static uint64_t  g_total_switches = 0;

/* -------------------------------------------------------------------------
 * Time helpers
 * ---------------------------------------------------------------------- */

uint64_t sched_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* -------------------------------------------------------------------------
 * Idle task
 * ---------------------------------------------------------------------- */

static void idle_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        /* Busy spin — in a real RTOS this would WFI */
        sched_yield();
    }
}

/* -------------------------------------------------------------------------
 * Priority queue helpers (simple sorted insertion into a singly-linked list)
 * ---------------------------------------------------------------------- */

/* Insert @t into the list rooted at *head, ordered by ascending priority
 * (lower number = higher priority) with FIFO ordering at equal priorities. */
static void pq_insert(Task **head, Task *t)
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

/* Remove and return the first task from *head (highest priority). */
static Task *pq_pop(Task **head)
{
    if (*head == NULL) return NULL;
    Task *t = *head;
    *head   = t->next;
    t->next = NULL;
    return t;
}

/* Remove a specific task from the list. Returns 1 if found. */
static int pq_remove(Task **head, Task *t)
{
    if (*head == NULL) return 0;
    if (*head == t) { *head = t->next; t->next = NULL; return 1; }
    Task *cur = *head;
    while (cur->next != NULL) {
        if (cur->next == t) {
            cur->next = t->next;
            t->next   = NULL;
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

/* Global ready queue */
static Task *g_ready_head = NULL;

/* -------------------------------------------------------------------------
 * SIGALRM handler — simulates timer interrupt / preemption
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_preempt_flag = 0;

static void sigalrm_handler(int sig)
{
    (void)sig;
    g_preempt_flag = 1;
}

static void arm_timer(void)
{
    struct itimerval itv;
    itv.it_interval.tv_sec  = 0;
    itv.it_interval.tv_usec = TIME_SLICE_MS * 1000;
    itv.it_value.tv_sec     = 0;
    itv.it_value.tv_usec    = TIME_SLICE_MS * 1000;
    setitimer(ITIMER_REAL, &itv, NULL);
}

/* -------------------------------------------------------------------------
 * Scheduler core
 * ---------------------------------------------------------------------- */

/* Wake any sleeping tasks whose deadline has arrived. */
static void wake_sleeping_tasks(void)
{
    uint64_t now = sched_now_ms();
    for (int i = 0; i < g_task_count; ++i) {
        Task *t = &g_tasks[i];
        if (t->state == TASK_SLEEPING && now >= t->wake_time) {
            t->state      = TASK_READY;
            t->wait_start_ms = now;
            pq_insert(&g_ready_head, t);
        }
    }
}

/* Pick the next task to run.  Never returns NULL (idle is always ready). */
static Task *pick_next(void)
{
    wake_sleeping_tasks();
    Task *next = pq_pop(&g_ready_head);
    return next; /* may be NULL if truly nothing ready — shouldn't happen with idle */
}

/* Perform a context switch from the currently running task to @next. */
static void switch_to(Task *next)
{
    task_t  old_id  = g_current;
    Task   *old     = (old_id >= 0) ? &g_tasks[old_id] : NULL;

    g_current = next->id;
    next->state = TASK_RUNNING;
    g_total_switches++;
    next->switch_count++;

    if (old && old->state == TASK_RUNNING) {
        /* Still runnable — re-queue */
        old->state = TASK_READY;
        pq_insert(&g_ready_head, old);
    }

    /* Swap context: returns here when old task is resumed */
    if (old)
        swapcontext(&old->ctx, &next->ctx);
    else
        setcontext(&next->ctx);
}

/* Dispatcher — runs in the scheduler context (g_sched_ctx). */
static void dispatcher(void)
{
    for (;;) {
        g_preempt_flag = 0;

        Task *next = pick_next();
        if (next == NULL) {
            /* Nothing to run — should not happen since idle always ready */
            continue;
        }

        /* Track wait time */
        uint64_t now = sched_now_ms();
        if (next->wait_start_ms > 0)
            next->total_wait_ms += now - next->wait_start_ms;
        next->wait_start_ms = 0;

        /* Switch into the chosen task */
        task_t old_cur = g_current;
        g_current      = next->id;
        next->state    = TASK_RUNNING;
        g_total_switches++;
        next->switch_count++;

        swapcontext(&g_sched_ctx, &next->ctx);

        /* We come back here when any task calls swapcontext(&task->ctx, &g_sched_ctx) */
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void sched_init(size_t stack_pool_size)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count  = 0;
    g_current     = -1;
    g_ready_head  = NULL;
    g_total_switches = 0;

    /* Allocate stack pool */
    g_stack_pool = (uint8_t *)malloc(stack_pool_size);
    assert(g_stack_pool != NULL && "stack pool allocation failed");
    g_pool_size  = stack_pool_size;
    g_pool_used  = 0;

    /* Install SIGALRM handler for preemption */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    /* Create idle task (lowest priority = 255) */
    task_t idle_id = sched_create("idle", idle_task_fn, NULL, 255, 32768);
    assert(idle_id >= 0 && "failed to create idle task");
    (void)idle_id;
}

task_t sched_create(const char *name, void (*fn)(void *), void *arg,
                    uint8_t priority, size_t stack_size)
{
    if (g_task_count >= MAX_TASKS) return -1;

    /* Align stack size */
    stack_size = (stack_size + 15) & ~(size_t)15;

    if (g_pool_used + stack_size > g_pool_size) return -1;

    task_t id = g_task_count++;
    Task  *t  = &g_tasks[id];

    memset(t, 0, sizeof(*t));
    t->id            = id;
    t->priority      = priority;
    t->base_priority = priority;
    t->state         = TASK_READY;
    t->stack_size    = stack_size;
    t->stack_base    = g_stack_pool + g_pool_used;
    g_pool_used     += stack_size;

    strncpy(t->name, name ? name : "task", TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = '\0';

    /* Initialise ucontext */
    if (getcontext(&t->ctx) != 0) return -1;
    t->ctx.uc_stack.ss_sp    = t->stack_base;
    t->ctx.uc_stack.ss_size  = t->stack_size;
    t->ctx.uc_link           = NULL;   /* we handle termination ourselves */
    makecontext(&t->ctx, (void (*)(void))fn, 1, arg);

    /* Queue the task as ready (except idle which gets queued too) */
    t->wait_start_ms = sched_now_ms();
    pq_insert(&g_ready_head, t);

    return id;
}

void sched_run(void)
{
    arm_timer();

    /* Create scheduler dispatcher context */
    static uint8_t sched_stack[65536];
    if (getcontext(&g_sched_ctx) != 0) {
        perror("getcontext");
        return;
    }
    g_sched_ctx.uc_stack.ss_sp   = sched_stack;
    g_sched_ctx.uc_stack.ss_size = sizeof(sched_stack);
    g_sched_ctx.uc_link          = NULL;
    makecontext(&g_sched_ctx, dispatcher, 0);

    /* Jump into dispatcher */
    setcontext(&g_sched_ctx);

    /* Never reached */
}

void sched_yield(void)
{
    if (g_current < 0) return;
    Task *t = &g_tasks[g_current];
    t->state = TASK_READY;
    t->wait_start_ms = sched_now_ms();
    pq_insert(&g_ready_head, t);
    swapcontext(&t->ctx, &g_sched_ctx);
}

void sched_sleep_ms(uint32_t ms)
{
    if (g_current < 0) return;
    Task *t      = &g_tasks[g_current];
    t->state     = TASK_SLEEPING;
    t->wake_time = sched_now_ms() + ms;
    swapcontext(&t->ctx, &g_sched_ctx);
}

void sched_exit(void)
{
    if (g_current < 0) return;
    Task *t  = &g_tasks[g_current];
    t->state = TASK_TERMINATED;
    printf("[sched] task '%s' terminated\n", t->name);
    swapcontext(&t->ctx, &g_sched_ctx);
    /* Should never return */
    abort();
}

task_t sched_current(void)
{
    return g_current;
}

Task *sched_get_task(task_t id)
{
    if (id < 0 || id >= g_task_count) return NULL;
    return &g_tasks[id];
}

void _sched_block(void *sync_obj)
{
    if (g_current < 0) return;
    Task *t      = &g_tasks[g_current];
    t->state     = TASK_BLOCKED;
    t->blocked_on = sync_obj;
    t->wait_start_ms = sched_now_ms();
    swapcontext(&t->ctx, &g_sched_ctx);
}

void _sched_unblock_one(Task *t)
{
    if (t == NULL) return;
    t->state      = TASK_READY;
    t->blocked_on = NULL;
    t->wait_start_ms = sched_now_ms();
    pq_insert(&g_ready_head, t);
}

void _sched_preempt(void)
{
    sched_yield();
}

void sched_print_stats(void)
{
    printf("\n========= Scheduler Statistics =========\n");
    printf("Total context switches: %llu\n",
           (unsigned long long)g_total_switches);
    printf("\n%-20s %8s %8s %12s %10s\n",
           "Task", "Pri", "Switches", "Wait(ms)", "State");
    printf("%-20s %8s %8s %12s %10s\n",
           "----", "---", "--------", "--------", "-----");
    for (int i = 0; i < g_task_count; ++i) {
        Task *t = &g_tasks[i];
        const char *state_str[] = {"READY","RUNNING","BLOCKED","SLEEPING","TERMINATED"};
        const char *ss = (t->state <= TASK_TERMINATED) ? state_str[t->state] : "?";
        printf("%-20s %8u %8u %12llu %10s\n",
               t->name,
               (unsigned)t->priority,
               (unsigned)t->switch_count,
               (unsigned long long)t->total_wait_ms,
               ss);
    }
    printf("=========================================\n\n");
}
