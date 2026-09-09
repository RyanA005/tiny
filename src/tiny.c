#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#define DEBUG 1

#define DEFAULT_PORT 20000
#define WORKER_BUMP_SIZE 102400
#define CONNECTION_QUEUE_SIZE 1024
#define LOG_QUEUE_SIZE 256
#define LOG_MESSAGE_SIZE 160
#define LOG_FLUSH_EVERY 32
#define WORKER_COUNT 2
#define MAX_BACKLOG 1024

enum log_level { INFO, WARNING, ERROR };
enum request_type { GET, POST, PUT, DELETE };

typedef struct {
    uint64_t accept_time;
    uint64_t ip[2];
    int32_t fd;
    uint16_t port;
    uint8_t flags;
    uint8_t ready;
} connection;

typedef struct {
    enum log_level level;
    uint8_t ready;
    uint8_t len;
    uint8_t message[LOG_MESSAGE_SIZE];
} log;

typedef struct {
    connection data[CONNECTION_QUEUE_SIZE];
    uint64_t head_id __attribute__((aligned(64)));
    uint64_t tail_id __attribute__((aligned(64)));
} connection_queue;

typedef struct {
    log data[LOG_QUEUE_SIZE];
    uint32_t head_id __attribute__((aligned(64)));
    uint32_t tail_id __attribute__((aligned(64)));
} log_queue;

typedef struct {
  uint64_t offset;
  uint64_t size;
  uint8_t* mem;
} bump;

typedef struct {
    pthread_t thread;
    uint32_t id;
} worker_args;

void connection_copy_payload(connection *dst, const connection *src);
uint8_t enqueue_connection(connection_queue *q, connection c);
uint8_t dequeue_connection(connection_queue *q, connection *c);

uint8_t enqueue_log(log_queue *q, log l);
uint8_t dequeue_log(log_queue *q, log *l);
void tiny_log(enum log_level level, const uint8_t *fmt, ...);
void log_shutdown(void);

void *bump_init(bump *b, uint64_t size);
void *bump_alloc(bump *b, size_t size, size_t alignment);
void bump_reset(bump *b);

void *worker(void *p);
void *logger(void *p);

connection_queue queue = { 0 };
log_queue logs = { 0 };
pthread_t logger_thread;
uint8_t logger_run = 1;
uint64_t connection_drops = 0;
uint64_t log_drops = 0;

int32_t main(void) {

    worker_args args[WORKER_COUNT];

    struct sockaddr_in sa, ca;
    int32_t sd, cd;
    socklen_t slen;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(DEFAULT_PORT);

    pthread_create(&logger_thread, NULL, logger, NULL);

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] socket create failed: %s\n", strerror(err));
        log_shutdown();
        return 1;
    }
    if (bind(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] bind failed on port %d: %s\n", DEFAULT_PORT, strerror(err));
        log_shutdown();
        return 1;
    }
    if (listen(sd, MAX_BACKLOG) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] listen failed on port %d (backlog %d): %s\n",
                 DEFAULT_PORT, MAX_BACKLOG, strerror(err));
        log_shutdown();
        return 1;
    }

    tiny_log(INFO, "[SYS] listening on 0.0.0.0:%d (backlog %d, queue %d)\n",
             DEFAULT_PORT, MAX_BACKLOG, CONNECTION_QUEUE_SIZE);
    tiny_log(INFO, "[SYS] starting %d workers (bump %d, log queue %d)\n",
             WORKER_COUNT, WORKER_BUMP_SIZE, LOG_QUEUE_SIZE);

    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        args[i].id = i;
        pthread_create(&args[i].thread, NULL, worker, &args[i]);
    }

    struct timeval t;
    while (1) {
        slen = sizeof(ca);
        cd = accept(sd, (struct sockaddr *)&ca, &slen);
        if (cd < 0) {
            int err = errno;
            if (err == EINTR) {
                continue;
            }
            tiny_log(ERROR, "[SYS] accept failed: %s\n", strerror(err));
            continue;
        }

        gettimeofday(&t, NULL);
        
        uint64_t accept_time = ((uint64_t)t.tv_sec * 1000000ULL) + (uint64_t)t.tv_usec;
        uint64_t client_ip[2] = {0, 0};
        uint16_t client_port = 0;
        uint8_t client_flags = 0;

        if (ca.sin_family == AF_INET) {
            struct sockaddr_in *addr_v4 = (struct sockaddr_in *)&ca;
            client_port = ntohs(addr_v4->sin_port);
            memcpy(&client_ip[0], &addr_v4->sin_addr.s_addr, sizeof(addr_v4->sin_addr.s_addr));
            client_flags |= 0x01; 
        } 
        else if (ca.sin_family == AF_INET6) {
            struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)&ca;
            client_port = ntohs(addr_v6->sin6_port);
            memcpy(client_ip, &addr_v6->sin6_addr, 16);
            client_flags |= 0x02;
        }
        connection c = {
            .accept_time = accept_time,
            .ip = { client_ip[0], client_ip[1] },
            .fd = cd,
            .port = client_port,
            .flags = client_flags,
            .ready = 0
        };
        if (!enqueue_connection(&queue, c)) {
            uint64_t total = __atomic_load_n(&connection_drops, __ATOMIC_RELAXED);
            tiny_log(WARNING, "[SYS] connection queue full, dropped fd=%d (total %llu)\n",
                     cd, (unsigned long long)total);
        }
    }

    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        pthread_join(args[i].thread, NULL);
    }
    tiny_log(INFO, "[SYS] joined %d workers, shutting down\n", WORKER_COUNT);
    log_shutdown();

    return 0;
}

void log_shutdown(void) {
    __atomic_store_n(&logger_run, 0, __ATOMIC_RELEASE);
    pthread_join(logger_thread, NULL);
}

static void logger_report_drops(void) {
    uint64_t dropped_log = __atomic_exchange_n(&log_drops, 0, __ATOMIC_RELAXED);
    if (dropped_log == 0) {
        return;
    }
    fprintf(stdout, "[SYS] dropped %llu log message(s) (queue full)\n",
            (unsigned long long)dropped_log);
}

void *logger(void *p) {
    (void)p;
    log l = { 0 };
    uint32_t since_flush = 0;

    while (__atomic_load_n(&logger_run, __ATOMIC_ACQUIRE)) {
        if (dequeue_log(&logs, &l)) {
            fwrite(l.message, 1, l.len, stdout);
            since_flush++;
            if (since_flush >= LOG_FLUSH_EVERY) {
                fflush(stdout);
                since_flush = 0;
            }
        } else {
            if (since_flush > 0) {
                fflush(stdout);
                since_flush = 0;
            }
            logger_report_drops();
            usleep(100);
        }
    }
    while (dequeue_log(&logs, &l)) {
        fwrite(l.message, 1, l.len, stdout);
    }
    logger_report_drops();
    fflush(stdout);
    return NULL;
}

void *worker(void *p) {
    worker_args *args = (worker_args *)p;
    tiny_log(INFO, "[WORKER %u] starting (bump %d)\n", args->id, WORKER_BUMP_SIZE);

    bump b = { 0 };
    connection c = { 0 };

    if (!bump_init(&b, WORKER_BUMP_SIZE)) {
        tiny_log(ERROR, "[WORKER %u] bump malloc failed (%d bytes)\n",
                 args->id, WORKER_BUMP_SIZE);
        return NULL;
    }

    while (1) {
        bump_reset(&b);
        if (dequeue_connection(&queue, &c)) {
            uint8_t ip_str[INET6_ADDRSTRLEN] = {0};
            if (c.flags & 0x01) inet_ntop(AF_INET, &(c.ip[0]), ip_str, sizeof(ip_str));
            else if (c.flags & 0x02) inet_ntop(AF_INET6, &(c.ip[0]), ip_str, sizeof(ip_str));
            else snprintf(ip_str, sizeof(ip_str), "UNKNOWN");
            tiny_log(INFO, "[WORKER %u] accept fd=%d %s:%u t=%lu flags=0x%02X\n",
                     args->id, c.fd, ip_str, c.port, (unsigned long)c.accept_time, c.flags);
            if (c.fd >= 0) {
                close(c.fd);
            }
        } else {
            usleep(100);
        }
    }

    tiny_log(INFO, "[WORKER %u] exiting\n", args->id);
    return NULL;
}

void tiny_log(enum log_level level, const uint8_t *fmt, ...) {
    log l = { 0 };
    va_list ap;
    int n;

    l.level = level;
    va_start(ap, fmt);
    n = vsnprintf((char *)l.message, sizeof(l.message), (const char *)fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(l.message)) {
        l.len = (uint8_t)(sizeof(l.message) - 1);
    } else {
        l.len = (uint8_t)n;
    }
    if (!enqueue_log(&logs, l)) {
        __atomic_fetch_add(&log_drops, 1, __ATOMIC_RELAXED);
    }
}

void connection_copy_payload(connection *dst, const connection *src) {
    dst->accept_time = src->accept_time;
    dst->ip[0] = src->ip[0];
    dst->ip[1] = src->ip[1];
    dst->fd = src->fd;
    dst->port = src->port;
    dst->flags = src->flags;
}

uint8_t enqueue_connection(connection_queue *q, connection c) {
    connection *slot = &q->data[q->tail_id & (CONNECTION_QUEUE_SIZE - 1)];

    if (__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {
        if (c.fd >= 0) {
            close(c.fd);
        }
        __atomic_fetch_add(&connection_drops, 1, __ATOMIC_RELAXED);
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

uint8_t enqueue_log(log_queue *q, log l) {
    uint32_t current_tail_id;

    do {
        current_tail_id = __atomic_load_n(&q->tail_id, __ATOMIC_RELAXED);
        uint32_t current_head_id = __atomic_load_n(&q->head_id, __ATOMIC_ACQUIRE);
        if ((current_tail_id - current_head_id) >= LOG_QUEUE_SIZE) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&q->tail_id, &current_tail_id, current_tail_id + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

    log *slot = &q->data[current_tail_id & (LOG_QUEUE_SIZE - 1)];
    slot->level = l.level;
    slot->len = l.len;
    memcpy(slot->message, l.message, l.len);
    slot->message[l.len] = '\0';
    __atomic_store_n(&slot->ready, 1, __ATOMIC_RELEASE);
    return 1; 
}

uint8_t dequeue_log(log_queue *q, log *l) {
    uint32_t current_head_id = __atomic_load_n(&q->head_id, __ATOMIC_RELAXED);
    uint32_t current_tail_id = __atomic_load_n(&q->tail_id, __ATOMIC_ACQUIRE);

    if (current_head_id == current_tail_id)
        return 0;
    log *slot = &q->data[current_head_id & (LOG_QUEUE_SIZE - 1)];
    while (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {}

    l->level = slot->level;
    l->len = slot->len;
    memcpy(l->message, slot->message, slot->len);
    l->message[slot->len] = '\0';
    __atomic_store_n(&slot->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&q->head_id, current_head_id + 1, __ATOMIC_RELEASE);
    return 1;
}

void *bump_init(bump *b, uint64_t size) {
    b->offset = 0;
    b->size = size;
    b->mem = malloc(size);
    if (!b->mem) {
        b->size = 0;
        return NULL;
    }
    return b->mem;
}

void *bump_alloc(bump *b, size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    uintptr_t base = (uintptr_t)b->mem;
    uintptr_t current = base + (uintptr_t)b->offset;
    uintptr_t aligned = (current + (uintptr_t)alignment - 1) & ~((uintptr_t)alignment - 1);

    if (aligned < current) {
        return NULL;
    }

    size_t offset = (size_t)(aligned - base);
    if (offset > b->size) {
        return NULL;
    }
    if (size > b->size - offset) {
        return NULL;
    }

    b->offset = offset + size;
    return (void *)aligned;
}

void bump_reset(bump *b) {
    b->offset = 0;
}
