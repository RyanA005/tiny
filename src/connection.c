#include "connection.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>

void connection_copy_payload(connection *dst, const connection *src) {
    dst->accept_time = src->accept_time;
    dst->ip[0] = src->ip[0];
    dst->ip[1] = src->ip[1];
    dst->fd = src->fd;
    dst->port = src->port;
    dst->flags = src->flags;
}

void connection_set_timeouts(int32_t fd) {
    struct timeval rcv = { .tv_sec = CONN_RECV_TIMEOUT_SEC, .tv_usec = 0 };
    struct timeval snd = { .tv_sec = CONN_SEND_TIMEOUT_SEC, .tv_usec = 0 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
}

void connection_set_nonblock(int32_t fd) {
    int32_t flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

uint8_t enqueue_connection(connection_queue *q, connection c) {
    connection *slot = &q->data[q->tail_id & (CONNECTION_QUEUE_SIZE - 1)];

    if (__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {
        if (c.fd >= 0) {
            close(c.fd);
        }
        return 0;
    }

    connection_copy_payload(slot, &c);
    __atomic_store_n(&slot->ready, 1, __ATOMIC_RELEASE);
    q->tail_id++;
    return 1;
}

uint8_t dequeue_connection(connection_queue *q, connection *c) {
    uint64_t pos = __atomic_load_n(&q->head_id, __ATOMIC_RELAXED);

    while (1) {
        connection *slot = &q->data[pos & (CONNECTION_QUEUE_SIZE - 1)];

        if (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {
            uint64_t head = __atomic_load_n(&q->head_id, __ATOMIC_RELAXED);
            if (head != pos) {
                pos = head;
                continue;
            }
            return 0;
        }

        if (__atomic_compare_exchange_n(&q->head_id, &pos, pos + 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            connection_copy_payload(c, slot);
            __atomic_store_n(&slot->ready, 0, __ATOMIC_RELEASE);
            return 1;
        }
    }
}
