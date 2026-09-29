#ifndef TINY_CONFIG_H
#define TINY_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define TINY_CACHE_LINE           64

/* Listen. Port and document root come from the command line. */
#define TINY_BIND                 "::"
#define TINY_BACKLOG              1024

/* Pool sizes. Queue length must be a power of two. */
#define TINY_WORKERS              6
#define TINY_CONNS_PER_WORKER     128
#define TINY_QUEUE_SIZE           1024

/* Timeouts, milliseconds. */
#define TINY_KEEPALIVE_MS         15000
#define TINY_READ_TIMEOUT_MS      10000
#define TINY_SEND_TIMEOUT_MS      30000
#define TINY_QUEUE_TIMEOUT_MS     5000

/* Per-connection memory. Header buffer is the tail of http_conn. */
#define TINY_HEADER_SIZE          4096
#define TINY_BUMP_SIZE            12288

#define TINY_EPOLL_WAIT_MS        50
#define TINY_EPOLL_EVENTS_MAX     128

/* Filesystem policy. */
#define TINY_ALLOW_DOTFILES       0
#define TINY_ALLOW_SYMLINKS       1

_Static_assert(TINY_WORKERS >= 1, "TINY_WORKERS");
_Static_assert(TINY_CONNS_PER_WORKER >= 1, "TINY_CONNS_PER_WORKER");
_Static_assert(TINY_QUEUE_SIZE >= 2 && (TINY_QUEUE_SIZE & (TINY_QUEUE_SIZE - 1)) == 0,
               "TINY_QUEUE_SIZE must be a power of two");
_Static_assert(TINY_BACKLOG >= 1, "TINY_BACKLOG");
_Static_assert(TINY_HEADER_SIZE >= 256, "TINY_HEADER_SIZE");
_Static_assert(TINY_KEEPALIVE_MS >= 1 && TINY_READ_TIMEOUT_MS >= 1 &&
               TINY_SEND_TIMEOUT_MS >= 1 && TINY_QUEUE_TIMEOUT_MS >= 1,
               "timeouts");

static inline int tiny_size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return -1;
    }
    *out = a * b;
    return 0;
}

#endif
