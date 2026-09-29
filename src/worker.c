#define _GNU_SOURCE

#include "runtime.h"
#include "stats.h"
#include "logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>

#define DEADLINE_SWEEP_EVERY 32

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void slot_close(int32_t epfd, tiny_slot *slot, uint32_t *nactive) {
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
    if (*nactive > 0) {
        (*nactive)--;
    }
    STAT_INC(STAT_CONN_CLOSE);
}

static void arm_events(int32_t epfd, tiny_slot *slots, uint32_t idx, int32_t io,
                        uint32_t *nactive) {
    tiny_slot *slot = &slots[idx];
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.u32 = idx;
    ev.events = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (io == HTTP_IO_WANT_READ) {
        ev.events |= EPOLLIN;
    } else if (io == HTTP_IO_WANT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, slot->hc.conn.fd, &ev) < 0) {
        tiny_log(WARNING, "[WORKER] epoll MOD fd=%d failed: %s\n",
                 slot->hc.conn.fd, strerror(errno));
        slot_close(epfd, slot, nactive);
    }
}

static void apply_io(int32_t epfd, tiny_slot *slots, uint32_t idx, int32_t io,
                     uint64_t now, uint32_t *nactive) {
    while (io == HTTP_IO_DONE) {
        io = http_conn_reuse(&slots[idx].hc, &slots[idx].arena, now);
    }
    if (io == HTTP_IO_CLOSE) {
        slot_close(epfd, &slots[idx], nactive);
        return;
    }
    if (!slots[idx].active) {
        return;
    }
    arm_events(epfd, slots, idx, io, nactive);
}

static void sweep_deadlines(int32_t epfd, tiny_slot *slots, uint32_t nslots,
                            uint64_t now, uint32_t *nactive) {
    for (uint32_t i = 0; i < nslots; i++) {
        if (!slots[i].active) {
            continue;
        }
        if (http_conn_check_deadline(&slots[i].hc, now) == HTTP_IO_CLOSE) {
            STAT_INC(STAT_TIMEOUT);
            slot_close(epfd, &slots[i], nactive);
        }
    }
}

static int32_t try_accept_one(tiny_worker *w, int32_t epfd, uint64_t now,
                              uint32_t *nactive) {
    tiny_runtime *rt = w->rt;
    tiny_slot *slots = w->slots;
    uint32_t nslots = w->slot_count;
    uint32_t i;

    for (i = 0; i < nslots; i++) {
        if (!slots[i].active) {
            break;
        }
    }
    if (i >= nslots) {
        return 0;
    }

    connection c = { 0 };
    if (!dequeue_connection(&rt->queue, &c)) {
        return 0;
    }
    if (c.fd < 0) {
        return 1;
    }

    if (now > c.accept_time &&
        (now - c.accept_time) > TINY_QUEUE_TIMEOUT_MS) {
        close(c.fd);
        STAT_INC(STAT_TIMEOUT);
        return 1;
    }

    tiny_slot *slot = &slots[i];
    bump_reset(&slot->arena);
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
    (*nactive)++;
    STAT_INC(STAT_CONN_OPEN);
    TINY_LOG_INFO("[WORKER %u] slot %u fd=%d\n", w->id, i, c.fd);

    int32_t io = http_conn_on_read(&slot->hc, now);
    apply_io(epfd, slots, i, io, now, nactive);
    return 1;
}

static void drain_wake(int32_t wake_fd) {
    uint64_t cnt;
    while (read(wake_fd, &cnt, sizeof(cnt)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

void *tiny_worker_main(void *p) {
    tiny_worker *w = (tiny_worker *)p;
    tiny_runtime *rt = w->rt;
    tiny_slot *slots = w->slots;
    uint32_t nslots = w->slot_count;
    struct epoll_event *events;
    uint32_t nactive = 0;
    uint32_t loop_i = 0;
    int32_t wake_fd = rt->wake_fd;
    size_t ev_bytes;

    TINY_LOG_INFO("[WORKER %u] starting (slots %u, bump %d)\n",
                  w->id, nslots, TINY_BUMP_SIZE);

    if (tiny_size_mul(TINY_EPOLL_EVENTS_MAX, sizeof(struct epoll_event), &ev_bytes) < 0) {
        __atomic_store_n(&w->status, -1, __ATOMIC_RELEASE);
        return NULL;
    }
    events = mmap(NULL, ev_bytes, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (events == MAP_FAILED) {
        tiny_log(ERROR, "[WORKER %u] mmap events failed\n", w->id);
        __atomic_store_n(&w->status, -1, __ATOMIC_RELEASE);
        return NULL;
    }

    int32_t epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        tiny_log(ERROR, "[WORKER %u] epoll_create1 failed: %s\n",
                 w->id, strerror(errno));
        munmap(events, ev_bytes);
        __atomic_store_n(&w->status, -1, __ATOMIC_RELEASE);
        return NULL;
    }

    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.u32 = TINY_WORKER_WAKE_IDX;
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &ev) < 0) {
            tiny_log(ERROR, "[WORKER %u] epoll add wake_fd failed: %s\n",
                     w->id, strerror(errno));
            close(epfd);
            munmap(events, ev_bytes);
            __atomic_store_n(&w->status, -1, __ATOMIC_RELEASE);
            return NULL;
        }
    }

    __atomic_store_n(&w->status, 1, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&rt->stop, __ATOMIC_ACQUIRE)) {
        uint64_t now = mono_ms();
        while (try_accept_one(w, epfd, now, &nactive)) {
        }

        int32_t timeout = nactive ? TINY_EPOLL_WAIT_MS : -1;
        int32_t n = epoll_wait(epfd, events, TINY_EPOLL_EVENTS_MAX, timeout);
        now = mono_ms();
        loop_i++;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            tiny_log(ERROR, "[WORKER %u] epoll_wait failed: %s\n",
                     w->id, strerror(errno));
            break;
        }

        for (int32_t ei = 0; ei < n; ei++) {
            uint32_t idx = events[ei].data.u32;
            if (idx == TINY_WORKER_WAKE_IDX) {
                drain_wake(wake_fd);
                continue;
            }
            if (idx >= nslots || !slots[idx].active) {
                continue;
            }

            tiny_slot *slot = &slots[idx];
            uint32_t ev = events[ei].events;

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                if (!(ev & EPOLLIN) && !(ev & EPOLLOUT)) {
                    slot_close(epfd, slot, &nactive);
                    continue;
                }
            }

            if ((ev & EPOLLIN) && slot->hc.phase == HTTP_PHASE_READ) {
                int32_t io = http_conn_on_read(&slot->hc, now);
                apply_io(epfd, slots, idx, io, now, &nactive);
                if (!slots[idx].active) {
                    continue;
                }
            }

            if ((ev & EPOLLOUT) && slot->hc.phase != HTTP_PHASE_READ) {
                int32_t io = http_conn_on_write(&slot->hc, now);
                apply_io(epfd, slots, idx, io, now, &nactive);
            }
        }

        if (n == 0 || (loop_i % DEADLINE_SWEEP_EVERY) == 0) {
            sweep_deadlines(epfd, slots, nslots, now, &nactive);
        }
    }

    for (uint32_t i = 0; i < nslots; i++) {
        slot_close(epfd, &slots[i], &nactive);
    }
    close(epfd);
    munmap(events, ev_bytes);
    TINY_LOG_INFO("[WORKER %u] exiting\n", w->id);
    return NULL;
}
