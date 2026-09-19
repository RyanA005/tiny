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

#define WORKER_BUMP_SIZE 102400
#define WORKER_COUNT 4

extern connection_queue queue;

typedef struct {
    pthread_t thread;
    uint32_t id;
} worker_args;

void *worker(void *p);

#endif
