#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <unistd.h>

#define CONNECTION_QUEUE_SIZE 1024

/* Per-syscall stalls; absolute deadlines in http/static cap total time. */
#define CONN_RECV_TIMEOUT_SEC 5
#define CONN_SEND_TIMEOUT_SEC 5

/* Max time a connection may sit in the accept queue (monotonic ms). */
#define CONN_QUEUE_MAX_AGE_MS 5000

typedef struct {
    uint64_t accept_time; /* CLOCK_MONOTONIC milliseconds */
    uint64_t ip[2];
    int32_t fd;
    uint16_t port;
    uint8_t flags;
    uint8_t ready;
} connection;

typedef struct {
    connection data[CONNECTION_QUEUE_SIZE];
    uint64_t head_id __attribute__((aligned(64)));
    uint64_t tail_id __attribute__((aligned(64)));
} connection_queue;

void connection_copy_payload(connection *dst, const connection *src);
uint8_t enqueue_connection(connection_queue *q, connection c);
uint8_t dequeue_connection(connection_queue *q, connection *c);
void connection_set_timeouts(int32_t fd);

#endif
