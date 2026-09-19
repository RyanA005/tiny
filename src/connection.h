#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <unistd.h>

#define CONNECTION_QUEUE_SIZE 1024

typedef struct {
    uint64_t accept_time;
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

#endif
