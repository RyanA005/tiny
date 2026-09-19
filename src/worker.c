#include "worker.h"

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

            if (c.fd >= 0) {
                char ip_str[INET6_ADDRSTRLEN] = {0};
                if (c.flags & 0x01) {
                    inet_ntop(AF_INET, &c.ip[0], ip_str, sizeof(ip_str));
                } else if (c.flags & 0x02) {
                    inet_ntop(AF_INET6, &c.ip[0], ip_str, sizeof(ip_str));
                } else {
                    snprintf(ip_str, sizeof(ip_str), "UNKNOWN");
                }
                tiny_log(INFO, "[WORKER %u] accept fd=%d %s:%u flags=0x%02X\n",
                         args->id, c.fd, ip_str, c.port, c.flags);

                do_http(&c, &b);

                close(c.fd);
            }

        } else {
            usleep(100);
        }
    }

    tiny_log(INFO, "[WORKER %u] exiting\n", args->id);
    return NULL;
}
