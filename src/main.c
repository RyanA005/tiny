#include "logger.h"
#include "routes.h"
#include "runtime.h"
#include "stats.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <config>\n"
            "\n"
            "  listen <port>\n"
            "  static <prefix> <path>\n"
            "  proxy  <prefix> <host:port>\n"
            "\n"
            "  listen 8080\n"
            "  static / ./www\n"
            "\n"
            "  listen 8080\n"
            "  proxy / localhost:3000\n",
            argv0);
}

int main(int argc, char **argv) {
    tiny_runtime rt;
    tiny_site site;

    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }
    /* Reject a bad file before the logger or workers start. */
    if (tiny_site_load(&site, argv[1]) < 0) {
        return 1;
    }

    if (log_init() < 0) {
        fprintf(stderr, "tiny: logger failed to start\n");
        return 1;
    }
    stats_init();

    memset(&rt, 0, sizeof(rt));
    if (tiny_runtime_init(&rt, &site) < 0) {
        log_shutdown();
        return 1;
    }

    int rc = tiny_run(&rt);
    tiny_runtime_destroy(&rt);
    log_shutdown();
    return rc;
}
