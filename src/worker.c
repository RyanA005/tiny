#define _GNU_SOURCE

#include "worker.h"
#include "stats.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* Sweep idle deadlines every N loops (or on epoll timeout). Avoids
 * clock_gettime + O(slots) work on every hot event. */
#define DEADLINE_SWEEP_EVERY 32

int32_t worker_wake_fds[WORKER_COUNT];
static uint32_t wake_rr;

typedef struct {
    uint8_t active;
    bump arena;
    http_conn hc;
} worker_slot;

void worker_notify(void) {
    uint32_t i = __atomic_fetch_add(&wake_rr, 1, __ATOMIC_RELAXED) % WORKER_COUNT;
    int32_t fd = worker_wake_fds[i];
    if (fd < 0) {
        return;
    }
    uint64_t one = 1;
    ssize_t n = write(fd, &one, sizeof(one));
    (void)n; /* EAGAIN: counter saturated; worker already has pending wake */
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void slot_close(int32_t epfd, worker_slot *slot) {
    if (!slot->active) {
        return;
    }
    http_conn_cleanup(&slot->hc);
    if (slot->hc.conn.fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, slot->hc.conn.fd, 0);
        close(slot->hc.conn.fd);
        slot->hc.conn.fd = -1;
    }
    slot->active = 0;
    STAT_INC(STAT_CONN_CLOSE);
}

static void arm_events(int32_t epfd, worker_slot *slots, uint32_t idx, int32_t io) {
    worker_slot *slot = &slots[idx];
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.u32 = idx;
    ev.events = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (io == HTTP_IO_WANT_READ) {
        ev.events |= EPOLLIN;
    } else if (io == HTTP_IO_WANT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    epoll_ctl(epfd, EPOLL_CTL_MOD, slot->hc.conn.fd, &ev);
}

static void apply_io(int32_t epfd, worker_slot *slots, uint32_t idx, int32_t io,
                     uint64_t now) {
    /* Pipelined requests can finish synchronously: DONE -> reuse -> DONE... */
    while (io == HTTP_IO_DONE) {
        io = http_conn_reuse(&slots[idx].hc, &slots[idx].arena, now);
    }
    if (io == HTTP_IO_CLOSE) {
        slot_close(epfd, &slots[idx]);
        return;
    }
    arm_events(epfd, slots, idx, io);
}

static void sweep_deadlines(int32_t epfd, worker_slot *slots, uint64_t now) {
    for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
        if (!slots[i].active) {
            continue;
        }
        if (http_conn_check_deadline(&slots[i].hc, now) == HTTP_IO_CLOSE) {
            STAT_INC(STAT_TIMEOUT);
            slot_close(epfd, &slots[i]);
        }
    }
}

static int32_t try_accept_one(int32_t epfd, worker_slot *slots, uint32_t worker_id,
                              uint64_t now) {
    uint32_t i;
    for (i = 0; i < CONNS_PER_WORKER; i++) {
        if (!slots[i].active) {
            break;
        }
    }
    if (i >= CONNS_PER_WORKER) {
        return 0;
    }

    connection c = { 0 };
    if (!dequeue_connection(&queue, &c)) {
        return 0;
    }
    if (c.fd < 0) {
        return 1;
    }

    /* Drop connections that aged out waiting for a worker slot. */
    if (now > c.accept_time && (now - c.accept_time) > CONN_QUEUE_MAX_AGE_MS) {
        close(c.fd);
        STAT_INC(STAT_TIMEOUT);
        return 1;
    }

    worker_slot *slot = &slots[i];
    bump_reset(&slot->arena);
    /* Keep header_buf; only clear connection/request state. */
    slot->hc.conn.fd = -1;
    slot->hc.file_fd = -1;
    slot->hc.arena = 0;
    slot->hc.buf = 0;
    slot->hc.used = 0;
    connection_copy_payload(&slot->hc.conn, &c);

    if (http_conn_prepare(&slot->hc, &slot->arena, now) < 0) {
        close(c.fd);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.u32 = i;
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c.fd, &ev) < 0) {
        http_conn_cleanup(&slot->hc);
        close(c.fd);
        return 1;
    }

    slot->active = 1;
    STAT_INC(STAT_CONN_OPEN);

    (void)worker_id;
    TINY_LOG_INFO("[WORKER %u] slot %u fd=%d\n", worker_id, i, c.fd);

    /* Data may already be waiting in the kernel buffer. */
    int32_t io = http_conn_on_read(&slot->hc, now);
    apply_io(epfd, slots, i, io, now);
    return 1;
}

static void drain_wake(int32_t wake_fd) {
    uint64_t cnt;
    while (read(wake_fd, &cnt, sizeof(cnt)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break; /* EAGAIN: drained */
    }
}

void *worker(void *p) {
    worker_args *args = (worker_args *)p;
    worker_slot slots[CONNS_PER_WORKER];
    struct epoll_event events[WORKER_EPOLL_EVENTS];
    uint32_t active = 0;
    uint32_t loop_i = 0;
    int32_t wake_fd = args->wake_fd;

    TINY_LOG_INFO("[WORKER %u] starting (slots %d, arena %d)\n",
                  args->id, CONNS_PER_WORKER, SLOT_BUMP_SIZE);

    memset(slots, 0, sizeof(slots));
    for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
        if (!bump_init(&slots[i].arena, SLOT_BUMP_SIZE)) {
            tiny_log(ERROR, "[WORKER %u] slot %u bump failed\n", args->id, i);
            return NULL;
        }
        slots[i].hc.file_fd = -1;
        slots[i].hc.conn.fd = -1;
    }

    int32_t epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        tiny_log(ERROR, "[WORKER %u] epoll_create1 failed: %s\n",
                 args->id, strerror(errno));
        return NULL;
    }

    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.u32 = WORKER_WAKE_IDX;
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &ev) < 0) {
            tiny_log(ERROR, "[WORKER %u] epoll add wake_fd failed: %s\n",
                     args->id, strerror(errno));
            close(epfd);
            return NULL;
        }
    }

    while (1) {
        uint64_t now = mono_ms();
        while (try_accept_one(epfd, slots, args->id, now)) {
            /* fill free slots from the queue */
        }

        active = 0;
        for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
            active += slots[i].active;
        }

        /* Idle workers sleep until eventfd wake; busy ones use deadline timeout. */
        int32_t timeout = active ? WORKER_EPOLL_WAIT_MS : -1;
        int32_t n = epoll_wait(epfd, events, WORKER_EPOLL_EVENTS, timeout);
        now = mono_ms();
        loop_i++;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            tiny_log(ERROR, "[WORKER %u] epoll_wait failed: %s\n",
                     args->id, strerror(errno));
            break;
        }

        for (int32_t ei = 0; ei < n; ei++) {
            uint32_t idx = events[ei].data.u32;
            if (idx == WORKER_WAKE_IDX) {
                drain_wake(wake_fd);
                continue;
            }
            if (idx >= CONNS_PER_WORKER || !slots[idx].active) {
                continue;
            }

            worker_slot *slot = &slots[idx];
            uint32_t ev = events[ei].events;

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                /* Still try to drain a readable hangup if possible. */
                if (!(ev & EPOLLIN) && !(ev & EPOLLOUT)) {
                    slot_close(epfd, slot);
                    continue;
                }
            }

            int32_t io = HTTP_IO_WANT_READ;

            if ((ev & EPOLLIN) && slot->hc.phase == HTTP_PHASE_READ) {
                io = http_conn_on_read(&slot->hc, now);
                apply_io(epfd, slots, idx, io, now);
                if (!slots[idx].active) {
                    continue;
                }
            }

            if ((ev & EPOLLOUT) && slot->hc.phase != HTTP_PHASE_READ) {
                io = http_conn_on_write(&slot->hc, now);
                apply_io(epfd, slots, idx, io, now);
            }
        }

        /* Idle deadline sweep: on epoll timeout, or every N busy loops. */
        if (n == 0 || (loop_i % DEADLINE_SWEEP_EVERY) == 0) {
            sweep_deadlines(epfd, slots, now);
        }
    }

    for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
        slot_close(epfd, &slots[i]);
        free(slots[i].arena.mem);
    }
    close(epfd);
    TINY_LOG_INFO("[WORKER %u] exiting\n", args->id);
    return NULL;
}
