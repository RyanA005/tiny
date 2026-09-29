#ifndef TINY_RUNTIME_H
#define TINY_RUNTIME_H

#include <stdint.h>
#include <signal.h>
#include <pthread.h>

#include "config.h"
#include "connection.h"
#include "http.h"
#include "allocator.h"

#define TINY_WORKER_WAKE_IDX 0xffffffffu

typedef struct tiny_runtime tiny_runtime;

typedef struct {
    uint8_t active;
    uint8_t armed_io; /* HTTP_IO_WANT_READ or HTTP_IO_WANT_WRITE; 0 if unarmed */
    bump arena;
    http_conn hc;
} tiny_slot;

typedef struct {
    pthread_t thread;
    uint32_t id;
    volatile int32_t status; /* 0 starting, 1 ok, -1 fail */
    tiny_runtime *rt;
    tiny_slot *slots;
    uint32_t slot_count;
    uint16_t *free_stack;
    uint32_t free_top;
} __attribute__((aligned(TINY_CACHE_LINE))) tiny_worker;

_Static_assert(sizeof(tiny_worker) % TINY_CACHE_LINE == 0,
               "tiny_worker must be a multiple of cache line");

struct tiny_runtime {
    const char *root;
    uint16_t port;

    tiny_worker *workers;
    tiny_slot *slots;
    uint8_t *bump_pool;
    connection_queue queue;

    size_t workers_bytes;
    size_t slots_bytes;
    size_t bump_bytes;
    size_t queue_bytes;

    int listen_fd;
    int wake_fd;
    int root_fd;

    uint32_t workers_started;
    volatile sig_atomic_t stop;
    volatile sig_atomic_t stats_pending;
};

int tiny_runtime_init(tiny_runtime *rt, const char *root, uint16_t port);
int tiny_run(tiny_runtime *rt);
void tiny_runtime_destroy(tiny_runtime *rt);

void tiny_runtime_notify(tiny_runtime *rt);

/* Worker thread entry (internal but linked from runtime). */
void *tiny_worker_main(void *p);

#endif
