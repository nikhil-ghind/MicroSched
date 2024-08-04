#define _GNU_SOURCE
#include "micro_sched/scheduler.h"
#include "micro_sched/mutex.h"
#include "micro_sched/semaphore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Forward declarations for io_sim
 * ---------------------------------------------------------------------- */
typedef void (*callback_fn)(void *, int);
typedef enum { IO_DISK = 0, IO_NETWORK = 1, IO_TIMER = 2 } IoType;
void io_sim_init(void);
void io_sim_shutdown(void);
void io_request(IoType type, uint32_t latency_ms, callback_fn cb, void *user_data);

/* -------------------------------------------------------------------------
 * Shared state
 * ---------------------------------------------------------------------- */

#define BUFFER_SIZE 8
static int       g_buffer[BUFFER_SIZE];
static int       g_buf_head  = 0;
static int       g_buf_tail  = 0;
static Semaphore g_empty;        /* slots available for producer */
static Semaphore g_full;         /* items available for consumer */
static Mutex     g_buf_mutex;    /* protect head/tail */
static Mutex     g_shared_mutex; /* worker shared resource */

static uint64_t  g_demo_start_ms;
static volatile int g_stop = 0;

/* -------------------------------------------------------------------------
 * Producer task (priority 1)
 * ---------------------------------------------------------------------- */
static void producer_fn(void *arg)
{
    (void)arg;
    int item = 0;
    while (!g_stop) {
        sem_wait(&g_empty);
        mutex_lock(&g_buf_mutex);

        g_buffer[g_buf_head] = item;
        g_buf_head = (g_buf_head + 1) % BUFFER_SIZE;
        printf("[producer] produced item %d  (t=+%llums)\n",
               item, (unsigned long long)(sched_now_ms() - g_demo_start_ms));
        item++;

        mutex_unlock(&g_buf_mutex);
        sem_post(&g_full);
        sched_sleep_ms(80);
    }
    sched_exit();
}

/* -------------------------------------------------------------------------
 * Consumer task (priority 2)
 * ---------------------------------------------------------------------- */
static void consumer_fn(void *arg)
{
    (void)arg;
    while (!g_stop) {
        sem_wait(&g_full);
        mutex_lock(&g_buf_mutex);

        int item = g_buffer[g_buf_tail];
        g_buf_tail = (g_buf_tail + 1) % BUFFER_SIZE;
        printf("[consumer] consumed item %d  (t=+%llums)\n",
               item, (unsigned long long)(sched_now_ms() - g_demo_start_ms));

        mutex_unlock(&g_buf_mutex);
        sem_post(&g_empty);
        sched_sleep_ms(150);
    }
    sched_exit();
}

/* -------------------------------------------------------------------------
 * Worker A (priority 3) — uses shared mutex
 * ---------------------------------------------------------------------- */
static void worker_a_fn(void *arg)
{
    (void)arg;
    int iter = 0;
    while (!g_stop) {
        mutex_lock(&g_shared_mutex);
        printf("[workerA]  critical section enter (iter=%d, t=+%llums)\n",
               iter, (unsigned long long)(sched_now_ms() - g_demo_start_ms));
        sched_sleep_ms(30);
        printf("[workerA]  critical section exit  (iter=%d)\n", iter);
        mutex_unlock(&g_shared_mutex);
        iter++;
        sched_sleep_ms(120);
    }
    sched_exit();
}

/* -------------------------------------------------------------------------
 * Worker B (priority 4) — also uses shared mutex (lower priority)
 * ---------------------------------------------------------------------- */
static void worker_b_fn(void *arg)
{
    (void)arg;
    int iter = 0;
    while (!g_stop) {
        mutex_lock(&g_shared_mutex);
        printf("[workerB]  critical section enter (iter=%d, t=+%llums)\n",
               iter, (unsigned long long)(sched_now_ms() - g_demo_start_ms));
        sched_sleep_ms(20);
        printf("[workerB]  critical section exit  (iter=%d)\n", iter);
        mutex_unlock(&g_shared_mutex);
        iter++;
        sched_sleep_ms(200);
    }
    sched_exit();
}

/* -------------------------------------------------------------------------
 * I/O-bound task (priority 2)
 * ---------------------------------------------------------------------- */
static void io_cb(void *ud, int status)
{
    (void)ud; (void)status;
    printf("[io_task]  I/O completion callback fired\n");
}

static void io_task_fn(void *arg)
{
    (void)arg;
    int req = 0;
    while (!g_stop) {
        printf("[io_task]  submitting I/O request #%d  (t=+%llums)\n",
               req, (unsigned long long)(sched_now_ms() - g_demo_start_ms));
        io_request(IO_NETWORK, 200, io_cb, NULL);
        printf("[io_task]  I/O request #%d completed\n", req);
        req++;
        sched_sleep_ms(50);
    }
    sched_exit();
}

/* -------------------------------------------------------------------------
 * Watchdog — stops demo after 5 seconds
 * ---------------------------------------------------------------------- */
static void watchdog_fn(void *arg)
{
    (void)arg;
    sched_sleep_ms(5000);
    g_stop = 1;
    printf("\n[watchdog] 5-second demo complete — stopping all tasks\n");
    sched_print_stats();

    /* Drain semaphores so blocked tasks can exit */
    for (int i = 0; i < BUFFER_SIZE; ++i) sem_post(&g_empty);
    for (int i = 0; i < BUFFER_SIZE; ++i) sem_post(&g_full);

    io_sim_shutdown();
    exit(0);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void)
{
    printf("=== MicroSched Demo ===\n");
    printf("Running 5 tasks for 5 seconds.\n\n");

    sched_init(2 * 1024 * 1024); /* 2 MiB stack pool */
    io_sim_init();

    /* Initialise primitives */
    sem_init(&g_empty, BUFFER_SIZE);
    sem_init(&g_full,  0);
    mutex_init(&g_buf_mutex);
    mutex_init(&g_shared_mutex);

    g_demo_start_ms = sched_now_ms();

    /* Create tasks (priority 0 = highest) */
    sched_create("producer",  producer_fn,  NULL, 1, 65536);
    sched_create("consumer",  consumer_fn,  NULL, 2, 65536);
    sched_create("workerA",   worker_a_fn,  NULL, 3, 65536);
    sched_create("workerB",   worker_b_fn,  NULL, 4, 65536);
    sched_create("io_task",   io_task_fn,   NULL, 2, 65536);
    sched_create("watchdog",  watchdog_fn,  NULL, 0, 65536);

    sched_run(); /* never returns */
    return 0;
}
