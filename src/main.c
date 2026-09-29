#include "logger.h"
#include "runtime.h"
#include "stats.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s <port> <root>\n", argv0);
}

static uint16_t parse_port(const char *s) {
    unsigned long v = 0;
    if (!s || !s[0]) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
        v = v * 10ul + (unsigned long)(*p - '0');
        if (v > 65535ul) {
            return 0;
        }
    }
    return (uint16_t)v;
}

int main(int argc, char **argv) {
    tiny_runtime rt;
    uint16_t port;

    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    port = parse_port(argv[1]);
    if (port == 0 || argv[2][0] == '\0') {
        usage(argv[0]);
        return 1;
    }

    if (log_init() < 0) {
        fprintf(stderr, "tiny: logger failed to start\n");
        return 1;
    }
    stats_init();

    memset(&rt, 0, sizeof(rt));
    if (tiny_runtime_init(&rt, argv[2], port) < 0) {
        log_shutdown();
        return 1;
    }

    int rc = tiny_run(&rt);
    tiny_runtime_destroy(&rt);
    log_shutdown();
    return rc;
}
