#define _GNU_SOURCE

#include "worker.h"
#include "stats.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/epoll.h>

typedef struct {
    uint8_t active;
    bump arena;
    http_conn hc;
} worker_slot;

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

static void apply_io(int32_t epfd, worker_slot *slots, uint32_t idx, int32_t io) {
    if (io == HTTP_IO_DONE || io == HTTP_IO_CLOSE) {
        slot_close(epfd, &slots[idx]);
        return;
    }
    arm_events(epfd, slots, idx, io);
}

static int32_t try_accept_one(int32_t epfd, worker_slot *slots, uint32_t worker_id) {
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

    connection_set_nonblock(c.fd);

    worker_slot *slot = &slots[i];
    bump_reset(&slot->arena);
    memset(&slot->hc, 0, sizeof(slot->hc));
    connection_copy_payload(&slot->hc.conn, &c);
    slot->hc.file_fd = -1;

    uint64_t now = mono_ms();
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

    char ip_str[INET6_ADDRSTRLEN] = {0};
    if (c.flags & 0x01) {
        inet_ntop(AF_INET, &c.ip[0], ip_str, sizeof(ip_str));
    } else if (c.flags & 0x02) {
        inet_ntop(AF_INET6, &c.ip[0], ip_str, sizeof(ip_str));
    } else {
        snprintf(ip_str, sizeof(ip_str), "UNKNOWN");
    }
    tiny_log(INFO, "[WORKER %u] slot %u fd=%d %s:%u\n",
             worker_id, i, c.fd, ip_str, c.port);

    /* Data may already be waiting in the kernel buffer. */
    int32_t io = http_conn_on_read(&slot->hc, now);
    apply_io(epfd, slots, i, io);
    return 1;
}

void *worker(void *p) {
    worker_args *args = (worker_args *)p;
    worker_slot slots[CONNS_PER_WORKER];
    struct epoll_event events[WORKER_EPOLL_EVENTS];
    uint32_t active = 0;

    tiny_log(INFO, "[WORKER %u] starting (slots %d, arena %d)\n",
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

    while (1) {
        while (try_accept_one(epfd, slots, args->id)) {
            /* fill free slots from the queue */
        }

        active = 0;
        for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
            active += slots[i].active;
        }

        int32_t timeout = active ? WORKER_EPOLL_WAIT_MS : 10;
        int32_t n = epoll_wait(epfd, events, WORKER_EPOLL_EVENTS, timeout);
        uint64_t now = mono_ms();

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
            if (http_conn_check_deadline(&slot->hc, now) == HTTP_IO_CLOSE) {
                STAT_INC(STAT_TIMEOUT);
                slot_close(epfd, slot);
                continue;
            }

            if ((ev & EPOLLIN) && slot->hc.phase == HTTP_PHASE_READ) {
                io = http_conn_on_read(&slot->hc, now);
                apply_io(epfd, slots, idx, io);
                if (!slots[idx].active) {
                    continue;
                }
            }

            if ((ev & EPOLLOUT) && slot->hc.phase != HTTP_PHASE_READ) {
                io = http_conn_on_write(&slot->hc, now);
                apply_io(epfd, slots, idx, io);
            }
        }

        // Deadline sweep for idle stalls with no epoll edge.
        now = mono_ms();
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

    for (uint32_t i = 0; i < CONNS_PER_WORKER; i++) {
        slot_close(epfd, &slots[i]);
        free(slots[i].arena.mem);
    }
    close(epfd);
    tiny_log(INFO, "[WORKER %u] exiting\n", args->id);
    return NULL;
}
