#ifndef WORKER_H
#define WORKER_H

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "connection.h"
#include "allocator.h"
#include "logger.h"
#include "http.h"

/* Per-slot arena: request buf 4K + path 4K + resp hdr + padding. */
#define SLOT_BUMP_SIZE 12288
#define CONNS_PER_WORKER 64
#define WORKER_COUNT 8
#define WORKER_EPOLL_WAIT_MS 50
#define WORKER_EPOLL_EVENTS 128

/* epoll data.u32 sentinel for the per-worker wake eventfd */
#define WORKER_WAKE_IDX 0xffffffffu

extern connection_queue queue;
extern int32_t worker_wake_fds[WORKER_COUNT];

typedef struct {
    pthread_t thread;
    uint32_t id;
    int32_t wake_fd;
} worker_args;

void *worker(void *p);

/* Accept thread: wake one worker after a successful enqueue. */
void worker_notify(void);

#endif
