#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#include "allocator.h"
#include "connection.h"
#include "logger.h"
#include "static.h"
#include "stats.h"
#include "worker.h"

#include <signal.h>

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 20000
#endif
#define DEFAULT_DOCROOT "./www"
#define MAX_BACKLOG 1024

connection_queue queue = { 0 };

static void on_stats_signal(int32_t sig) {
    (void)sig;
    stats_dump();
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

    log_init();
    stats_init();
    signal(SIGUSR1, on_stats_signal);

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

    tiny_log(INFO, "[SYS] listening on 0.0.0.0:%d (backlog %d, queue %d)\n",
             port, MAX_BACKLOG, CONNECTION_QUEUE_SIZE);
    tiny_log(INFO, "[SYS] starting %d workers (%d slots x %d bump, log queue %d)\n",
             WORKER_COUNT, CONNS_PER_WORKER, SLOT_BUMP_SIZE, LOG_QUEUE_SIZE);

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
        }
    }

    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
        pthread_join(args[i].thread, NULL);
    }
    tiny_log(INFO, "[SYS] joined %d workers, shutting down\n", WORKER_COUNT);
    log_shutdown();

    return 0;
}
