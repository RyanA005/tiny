#define _GNU_SOURCE

#include "runtime.h"
#include "logger.h"
#include "static.h"
#include "stats.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

static void *mmap_pool(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    return p;
}

static void munmap_pool(void *p, size_t bytes) {
    if (p && bytes) {
        munmap(p, bytes);
    }
}

void tiny_runtime_notify(tiny_runtime *rt) {
    if (!rt || rt->wake_fd < 0) {
        return;
    }
    uint64_t one = 1;
    ssize_t n = write(rt->wake_fd, &one, sizeof(one));
    (void)n;
}

static int check_nofile(void) {
    uint64_t need =
        (uint64_t)TINY_WORKERS * (uint64_t)TINY_CONNS_PER_WORKER * 2ull +
        (uint64_t)TINY_QUEUE_SIZE +
        (uint64_t)TINY_WORKERS * 2ull +
        64ull;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        fprintf(stderr, "tiny: getrlimit(NOFILE) failed: %s\n", strerror(errno));
        return -1;
    }
    if (rl.rlim_cur >= need) {
        return 0;
    }

    rlim_t want = (rlim_t)need;
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) {
        fprintf(stderr,
                "tiny: configuration requires ~%llu file descriptors,\n"
                "      but RLIMIT_NOFILE is %llu (hard %llu)\n",
                (unsigned long long)need,
                (unsigned long long)rl.rlim_cur,
                (unsigned long long)rl.rlim_max);
        return -1;
    }

    struct rlimit up = rl;
    up.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &up) < 0) {
        fprintf(stderr, "tiny: setrlimit(NOFILE, %llu) failed: %s\n",
                (unsigned long long)want, strerror(errno));
        return -1;
    }
    return 0;
}

static int open_listener(tiny_runtime *rt) {
    int fd = -1;
    int is_v6 = 0;

    if (strchr(TINY_BIND, ':') != NULL) {
        is_v6 = 1;
    }

    if (is_v6) {
        fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            fprintf(stderr, "tiny: socket: %s\n", strerror(errno));
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        int v6only = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(rt->port);
        if (inet_pton(AF_INET6, TINY_BIND, &sa.sin6_addr) != 1) {
            fprintf(stderr, "tiny: invalid IPv6 bind '%s'\n", TINY_BIND);
            close(fd);
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr, "tiny: bind: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            fprintf(stderr, "tiny: socket: %s\n", strerror(errno));
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(rt->port);
        if (inet_pton(AF_INET, TINY_BIND, &sa.sin_addr) != 1) {
            fprintf(stderr, "tiny: invalid IPv4 bind '%s'\n", TINY_BIND);
            close(fd);
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr, "tiny: bind: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    if (listen(fd, TINY_BACKLOG) < 0) {
        fprintf(stderr, "tiny: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    rt->listen_fd = fd;
    return 0;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void on_stats_signal(int sig) {
    (void)sig;
    /* Handled via runtime pointer set before threads; see tiny_run. */
}

/* Process-wide pointer only for the stats signal -> accept loop flag. */
static tiny_runtime *g_rt_for_signal;

static void on_stats_signal_rt(int sig) {
    (void)sig;
    if (g_rt_for_signal) {
        g_rt_for_signal->stats_pending = 1;
    }
}

int tiny_runtime_init(tiny_runtime *rt, const char *root, uint16_t port) {
    memset(rt, 0, sizeof(*rt));
    rt->root = root;
    rt->port = port;
    rt->listen_fd = -1;
    rt->wake_fd = -1;
    rt->root_fd = -1;

    if (check_nofile() < 0) {
        return -1;
    }

    size_t nslots;
    if (tiny_size_mul(TINY_WORKERS, TINY_CONNS_PER_WORKER, &nslots) < 0) {
        fprintf(stderr, "tiny: workers * connections overflows\n");
        return -1;
    }
    if (tiny_size_mul(nslots, sizeof(tiny_slot), &rt->slots_bytes) < 0 ||
        tiny_size_mul(nslots, TINY_BUMP_SIZE, &rt->bump_bytes) < 0 ||
        tiny_size_mul(TINY_WORKERS, sizeof(tiny_worker), &rt->workers_bytes) < 0 ||
        tiny_size_mul(TINY_QUEUE_SIZE, sizeof(connection), &rt->queue_bytes) < 0) {
        fprintf(stderr, "tiny: pool size calculation overflows\n");
        return -1;
    }

    rt->workers = mmap_pool(rt->workers_bytes);
    rt->slots = mmap_pool(rt->slots_bytes);
    rt->bump_pool = mmap_pool(rt->bump_bytes);
    rt->queue.data = mmap_pool(rt->queue_bytes);
    if (!rt->workers || !rt->slots || !rt->bump_pool || !rt->queue.data) {
        fprintf(stderr, "tiny: mmap pools failed: %s\n", strerror(errno));
        tiny_runtime_destroy(rt);
        return -1;
    }

    memset(rt->workers, 0, rt->workers_bytes);
    memset(rt->slots, 0, rt->slots_bytes);
    memset(rt->queue.data, 0, rt->queue_bytes);
    rt->queue.capacity = TINY_QUEUE_SIZE;
    rt->queue.head_id = 0;
    rt->queue.tail_id = 0;

    for (uint32_t wi = 0; wi < TINY_WORKERS; wi++) {
        tiny_worker *w = &rt->workers[wi];
        w->id = wi;
        w->rt = rt;
        w->slot_count = TINY_CONNS_PER_WORKER;
        w->slots = rt->slots + (size_t)wi * TINY_CONNS_PER_WORKER;
        w->status = 0;
        for (uint32_t si = 0; si < TINY_CONNS_PER_WORKER; si++) {
            tiny_slot *slot = &w->slots[si];
            size_t bump_i = (size_t)wi * TINY_CONNS_PER_WORKER + si;
            bump_bind(&slot->arena,
                      rt->bump_pool + bump_i * TINY_BUMP_SIZE,
                      TINY_BUMP_SIZE);
            slot->hc.file_fd = -1;
            slot->hc.conn.fd = -1;
            slot->active = 0;
        }
    }

    {
        static_policy pol = {
            .allow_dotfiles = TINY_ALLOW_DOTFILES,
            .allow_symlinks = TINY_ALLOW_SYMLINKS,
        };
        static_set_policy(pol);
    }

    if (static_init(rt->root) < 0) {
        tiny_runtime_destroy(rt);
        return -1;
    }
    /* static_init opens root; track via static module. root_fd left -1 here. */

    rt->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (rt->wake_fd < 0) {
        fprintf(stderr, "tiny: eventfd: %s\n", strerror(errno));
        tiny_runtime_destroy(rt);
        return -1;
    }

    if (open_listener(rt) < 0) {
        tiny_runtime_destroy(rt);
        return -1;
    }

    return 0;
}

void tiny_runtime_destroy(tiny_runtime *rt) {
    if (!rt) {
        return;
    }
    __atomic_store_n(&rt->stop, 1, __ATOMIC_RELEASE);
    tiny_runtime_notify(rt);

    if (rt->workers && rt->workers_started > 0) {
        for (uint32_t i = 0; i < rt->workers_started; i++) {
            pthread_join(rt->workers[i].thread, NULL);
        }
        rt->workers_started = 0;
    }

    if (rt->listen_fd >= 0) {
        close(rt->listen_fd);
        rt->listen_fd = -1;
    }
    if (rt->wake_fd >= 0) {
        close(rt->wake_fd);
        rt->wake_fd = -1;
    }

    static_shutdown();

    munmap_pool(rt->queue.data, rt->queue_bytes);
    munmap_pool(rt->bump_pool, rt->bump_bytes);
    munmap_pool(rt->slots, rt->slots_bytes);
    munmap_pool(rt->workers, rt->workers_bytes);

    rt->queue.data = 0;
    rt->bump_pool = 0;
    rt->slots = 0;
    rt->workers = 0;
}

int tiny_run(tiny_runtime *rt) {
    /* Block SIGUSR1 before workers; unblock only in accept thread. */
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &set, 0);
    }
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_stats_signal_rt;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, 0);
    }
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, 0);
    }

    g_rt_for_signal = rt;

    TINY_LOG_INFO("[SYS] listening on %s:%u (backlog %u, queue %u)\n",
                  TINY_BIND, rt->port, TINY_BACKLOG, TINY_QUEUE_SIZE);
    TINY_LOG_INFO("[SYS] workers=%u connections=%u bump=%d header=%u root=%s\n",
                  TINY_WORKERS, TINY_CONNS_PER_WORKER,
                  TINY_BUMP_SIZE, TINY_HEADER_SIZE, rt->root);

    for (uint32_t i = 0; i < TINY_WORKERS; i++) {
        if (pthread_create(&rt->workers[i].thread, NULL, tiny_worker_main,
                           &rt->workers[i]) != 0) {
            fprintf(stderr, "tiny: pthread_create worker %u failed: %s\n",
                    i, strerror(errno));
            rt->workers_started = i;
            rt->stop = 1;
            tiny_runtime_notify(rt);
            return 1;
        }
        rt->workers_started = i + 1;
    }

    for (uint32_t i = 0; i < TINY_WORKERS; i++) {
        for (;;) {
            int32_t st = __atomic_load_n(&rt->workers[i].status, __ATOMIC_ACQUIRE);
            if (st == 1) {
                break;
            }
            if (st < 0) {
                fprintf(stderr, "tiny: worker %u failed to initialize\n", i);
                rt->stop = 1;
                tiny_runtime_notify(rt);
                return 1;
            }
            usleep(1000);
        }
    }

    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &set, 0);
    }

    struct sockaddr_storage ca;
    socklen_t slen;
    int cd;

    while (!__atomic_load_n(&rt->stop, __ATOMIC_ACQUIRE)) {
        if (rt->stats_pending) {
            rt->stats_pending = 0;
            stats_dump();
        }

        slen = sizeof(ca);
        cd = accept4(rt->listen_fd, (struct sockaddr *)&ca, &slen,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cd < 0) {
            if (errno == EINTR) {
                continue;
            }
            tiny_log(ERROR, "[SYS] accept failed: %s\n", strerror(errno));
            continue;
        }

        uint64_t accept_time = mono_ms();
        uint64_t client_ip[2] = {0, 0};
        uint16_t client_port = 0;
        uint8_t client_flags = 0;

        if (ca.ss_family == AF_INET) {
            struct sockaddr_in *a4 = (struct sockaddr_in *)&ca;
            client_port = ntohs(a4->sin_port);
            memcpy(&client_ip[0], &a4->sin_addr.s_addr, 4);
            client_flags |= 0x01;
        } else if (ca.ss_family == AF_INET6) {
            struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ca;
            client_port = ntohs(a6->sin6_port);
            memcpy(client_ip, &a6->sin6_addr, 16);
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
        if (!enqueue_connection(&rt->queue, c)) {
            STAT_INC(STAT_QUEUE_DROP);
            tiny_log(WARNING, "[SYS] connection queue full, dropped fd=%d\n", cd);
        } else {
            STAT_INC(STAT_ACCEPT);
            tiny_runtime_notify(rt);
        }
    }

    (void)on_stats_signal;
    return 0;
}
