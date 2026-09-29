#ifndef TINY_ROUTES_H
#define TINY_ROUTES_H

#include <stdint.h>
#include <sys/socket.h>

#include "http.h"

/*
 * Site config is a short line-oriented file:
 *
 *   listen 8080
 *   static / ./www
 *   proxy /api localhost:3000
 *
 * Longest prefix wins. A directory prefix matches that tree. A regular
 * file route matches that path only. ROUTE_APP is accepted in the table
 * so the router is not a static-vs-proxy switch, but it does not run yet.
 */

#define TINY_MAX_ROUTES   32
#define TINY_PREFIX_MAX   256
#define TINY_TARGET_MAX   512

enum route_type {
    ROUTE_NONE = 0,
    ROUTE_STATIC = 1,
    ROUTE_PROXY = 2,
    ROUTE_APP = 3
};

typedef struct tiny_route {
    uint8_t type;
    uint8_t exact;    /* 1: filesystem target is one file; path must be equal */
    uint8_t primary;  /* 1: static "/" directory, served by static_begin */
    uint16_t prefix_len;
    uint16_t target_len;
    int32_t dir_fd;   /* non-primary static directory, else -1 */
    char prefix[TINY_PREFIX_MAX];
    char target[TINY_TARGET_MAX];
    struct sockaddr_storage upstream;
    socklen_t upstream_len;
} tiny_route;

typedef struct tiny_site {
    uint16_t port;
    uint16_t nroutes;
    tiny_route routes[TINY_MAX_ROUTES];
} tiny_site;

/* Parse and check. Opens nothing. Fails if listen, a route, or a target is invalid. */
int tiny_site_load(tiny_site *site, const char *path);

/* Stat, open directory fds, and install the process-wide table. */
int routes_init(const tiny_site *site);
void routes_shutdown(void);

/* Pointer into the installed table. Empty string if there is no docroot. */
const char *routes_docroot(void);

/*
 * Worker sets this before a client read that might finish a request.
 * slot is a tiny_slot *. Kept as void * so this header does not include
 * the runtime.
 */
void route_bind(int epfd, void *slot, uint32_t idx);

/* After a request is parsed. STATIC, PROXY, or APP. */
int32_t route_begin(http_conn *hc, uint64_t now_ms);

#endif
