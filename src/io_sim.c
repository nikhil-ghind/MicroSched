#define _GNU_SOURCE
#include "micro_sched/scheduler.h"

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Public types
 * ---------------------------------------------------------------------- */

typedef enum {
    IO_DISK    = 0,
    IO_NETWORK = 1,
    IO_TIMER   = 2
} IoType;

typedef void (*callback_fn)(void *user_data, int status);

typedef struct IoRequest {
    IoType      type;
    uint32_t    latency_ms;
    callback_fn cb;
    void       *user_data;
    task_t      waiting_task;   /* task blocked waiting for completion */
    struct IoRequest *next;
} IoRequest;

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

#define MAX_IO_REQUESTS 64

static IoRequest  g_requests[MAX_IO_REQUESTS];
static int        g_req_count   = 0;
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t  g_io_thread;
static volatile int g_io_running = 0;

/* -------------------------------------------------------------------------
 * Background completion thread
 * ---------------------------------------------------------------------- */

static void *io_worker(void *arg)
{
    (void)arg;
    while (g_io_running) {
        struct timespec ts = { 0, 10 * 1000 * 1000 }; /* 10 ms poll */
        nanosleep(&ts, NULL);

        uint64_t now = sched_now_ms();
        pthread_mutex_lock(&g_io_lock);
        for (int i = 0; i < g_req_count; ++i) {
            IoRequest *r = &g_requests[i];
            if (r->latency_ms == 0) continue; /* sentinel: completed */

            /* Simulate completion: use latency_ms as a countdown */
            if (r->latency_ms <= 10) {
                r->latency_ms = 0; /* mark completed */
                task_t tid    = r->waiting_task;
                callback_fn cb = r->cb;
                void *ud      = r->user_data;

                pthread_mutex_unlock(&g_io_lock);

                /* Fire callback */
                if (cb) cb(ud, 0 /* success */);

                /* Unblock the waiting task */
                if (tid >= 0) {
                    Task *t = sched_get_task(tid);
                    if (t && t->state == TASK_BLOCKED)
                        _sched_unblock_one(t);
                }

                pthread_mutex_lock(&g_io_lock);
            } else {
                r->latency_ms -= 10;
            }
        }
        pthread_mutex_unlock(&g_io_lock);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void io_sim_init(void)
{
    memset(g_requests, 0, sizeof(g_requests));
    g_req_count  = 0;
    g_io_running = 1;
    pthread_create(&g_io_thread, NULL, io_worker, NULL);
}

void io_sim_shutdown(void)
{
    g_io_running = 0;
    pthread_join(g_io_thread, NULL);
}

/**
 * io_request - submit an asynchronous I/O request.
 * The calling task blocks until completion.  @cb is invoked from the
 * background thread just before the task is unblocked.
 */
void io_request(IoType type, uint32_t latency_ms, callback_fn cb, void *user_data)
{
    pthread_mutex_lock(&g_io_lock);
    assert(g_req_count < MAX_IO_REQUESTS && "I/O request queue full");
    IoRequest *r  = &g_requests[g_req_count++];
    r->type       = type;
    r->latency_ms = (latency_ms == 0) ? 1 : latency_ms; /* at least 1 tick */
    r->cb         = cb;
    r->user_data  = user_data;
    r->waiting_task = sched_current();
    pthread_mutex_unlock(&g_io_lock);

    const char *type_names[] = { "DISK", "NETWORK", "TIMER" };
    printf("[io]  task '%s' waiting on %s I/O (%u ms)\n",
           sched_get_task(sched_current()) ? sched_get_task(sched_current())->name : "?",
           type_names[type], latency_ms);

    /* Block until the background thread unblocks us */
    _sched_block(NULL);
}
