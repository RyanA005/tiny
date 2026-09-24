#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/eventfd.h>

#include <pthread.h>

#include "allocator.h"
#include "connection.h"
#include "logger.h"
#include "static.h"
#include "stats.h"
#include "worker.h"

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 20000
#endif
#define DEFAULT_DOCROOT "./www"
#define MAX_BACKLOG 1024

connection_queue queue = { 0 };

static volatile sig_atomic_t stats_pending = 0;

static void on_stats_signal(int32_t sig) {
    (void)sig;
    stats_pending = 1;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int32_t main(int32_t argc, char **argv) {
    worker_args args[WORKER_COUNT];
    const char *docroot = DEFAULT_DOCROOT;
    uint16_t port = DEFAULT_PORT;

    if (argc >= 2) {
        docroot = argv[1];
    }
    if (argc >= 3) {
        int32_t p = atoi(argv[2]);
        if (p > 0 && p < 65536) {
            port = (uint16_t)p;
        }
    }

    struct sockaddr_in sa;
    struct sockaddr_storage ca;
    int32_t sd, cd;
    socklen_t slen;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    /* Block SIGUSR1 before any threads so only the accept loop handles it. */
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &set, 0);
    }

    log_init();
    stats_init();
    {
        struct sigaction sa_stat;
        memset(&sa_stat, 0, sizeof(sa_stat));
        sa_stat.sa_handler = on_stats_signal;
        sigemptyset(&sa_stat.sa_mask);
        sa_stat.sa_flags = 0; /* interrupt blocking accept */
        sigaction(SIGUSR1, &sa_stat, 0);
    }
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &set, 0);
    }

    if (static_init(docroot) < 0) {
        log_shutdown();
        return 1;
    }

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] socket create failed: %s\n", strerror(err));
        static_shutdown();
        log_shutdown();
        return 1;
    }
    {
        int on = 1;
        if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
            int err = errno;
            tiny_log(ERROR, "[SYS] SO_REUSEADDR failed: %s\n", strerror(err));
            static_shutdown();
            log_shutdown();
            return 1;
        }
    }
    if (bind(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] bind failed on port %d: %s\n", port, strerror(err));
        static_shutdown();
        log_shutdown();
        return 1;
    }
    if (listen(sd, MAX_BACKLOG) < 0) {
        int err = errno;
        tiny_log(ERROR, "[SYS] listen failed on port %d (backlog %d): %s\n",
                 port, MAX_BACKLOG, strerror(err));
        static_shutdown();
        log_shutdown();
        return 1;
    }

    TINY_LOG_INFO("[SYS] listening on 0.0.0.0:%d (backlog %d, queue %d)\n",
             port, MAX_BACKLOG, CONNECTION_QUEUE_SIZE);
    TINY_LOG_INFO("[SYS] starting %d workers (%d slots x %d bump, log queue %d)\n",
             WORKER_COUNT, CONNS_PER_WORKER, SLOT_BUMP_SIZE, LOG_QUEUE_SIZE);

    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        worker_wake_fds[i] = -1;
    }
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        int32_t wfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wfd < 0) {
            tiny_log(ERROR, "[SYS] eventfd failed: %s\n", strerror(errno));
            static_shutdown();
            log_shutdown();
            return 1;
        }
        worker_wake_fds[i] = wfd;
        args[i].id = i;
        args[i].wake_fd = wfd;
        pthread_create(&args[i].thread, NULL, worker, &args[i]);
    }

    while (1) {
        if (stats_pending) {
            stats_pending = 0;
            stats_dump();
        }

        slen = sizeof(ca);
        cd = accept4(sd, (struct sockaddr *)&ca, &slen,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cd < 0) {
            int err = errno;
            if (err == EINTR) {
                continue;
            }
            tiny_log(ERROR, "[SYS] accept failed: %s\n", strerror(err));
            continue;
        }

        uint64_t accept_time = mono_ms();
        uint64_t client_ip[2] = {0, 0};
        uint16_t client_port = 0;
        uint8_t client_flags = 0;

        if (ca.ss_family == AF_INET) {
            struct sockaddr_in *addr_v4 = (struct sockaddr_in *)&ca;
            client_port = ntohs(addr_v4->sin_port);
            memcpy(&client_ip[0], &addr_v4->sin_addr.s_addr, sizeof(addr_v4->sin_addr.s_addr));
            client_flags |= 0x01;
        } else if (ca.ss_family == AF_INET6) {
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
            STAT_INC(STAT_QUEUE_DROP);
            tiny_log(WARNING, "[SYS] connection queue full, dropped fd=%d\n",
                     cd);
        } else {
            STAT_INC(STAT_ACCEPT);
            worker_notify();
        }
    }

    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        pthread_join(args[i].thread, NULL);
    }
    TINY_LOG_INFO("[SYS] joined %d workers, shutting down\n", WORKER_COUNT);
    log_shutdown();

    return 0;
}
