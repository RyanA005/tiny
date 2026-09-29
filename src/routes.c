#define _GNU_SOURCE

#include "routes.h"
#include "runtime.h"
#include "proxy.h"
#include "static.h"
#include "logger.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The router only picks a handler. STATIC keeps the existing file path.
 * PROXY speaks HTTP to one upstream. APP is a distinct type so a future
 * backend protocol can be added without folding it into the proxy.
 */

static tiny_site g_site;
static int g_ready;

static __thread int g_epfd = -1;
static __thread tiny_slot *g_slot;
static __thread uint32_t g_idx;

static const char RESP_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESP_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESP_501[] =
    "HTTP/1.1 501 Not Implemented\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static int32_t arm_close(http_conn *hc, const char *resp, uint16_t status,
                         uint64_t now_ms) {
    hc->fixed = resp;
    hc->fixed_len = (uint32_t)strlen(resp);
    hc->fixed_off = 0;
    hc->phase = HTTP_PHASE_WRITE_FIXED;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
    hc->keep = 0;
    hc->status_code = status;
    return HTTP_IO_WANT_WRITE;
}

static void site_clear(tiny_site *site) {
    memset(site, 0, sizeof(*site));
    for (uint16_t i = 0; i < TINY_MAX_ROUTES; i++) {
        site->routes[i].dir_fd = -1;
    }
}

static int copy_token(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n == 0 || n >= cap) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return (int)n;
}

static int prefix_ok(const char *p) {
    if (!p || p[0] != '/') {
        return 0;
    }
    for (const char *q = p; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (c <= 0x20 || c >= 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int parse_port_token(const char *s, uint16_t *out) {
    unsigned long v = 0;
    if (!s || !s[0]) {
        return -1;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        v = v * 10ul + (unsigned long)(*p - '0');
        if (v > 65535ul) {
            return -1;
        }
    }
    if (v == 0) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int parse_upstream(const char *s, struct sockaddr_storage *ss, socklen_t *len) {
    char host[256];
    char port[8];
    const char *colon;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    size_t hlen;

    if (!s || !s[0]) {
        return -1;
    }
    if (s[0] == '[') {
        const char *rb = strchr(s, ']');
        if (!rb || rb[1] != ':' || rb == s + 1) {
            return -1;
        }
        hlen = (size_t)(rb - (s + 1));
        if (hlen >= sizeof(host) || strlen(rb + 2) >= sizeof(port)) {
            return -1;
        }
        memcpy(host, s + 1, hlen);
        host[hlen] = '\0';
        memcpy(port, rb + 2, strlen(rb + 2) + 1);
    } else {
        colon = strrchr(s, ':');
        if (!colon || colon == s || colon[1] == '\0') {
            return -1;
        }
        hlen = (size_t)(colon - s);
        if (hlen >= sizeof(host) || strlen(colon + 1) >= sizeof(port)) {
            return -1;
        }
        memcpy(host, s, hlen);
        host[hlen] = '\0';
        memcpy(port, colon + 1, strlen(colon + 1) + 1);
    }
    {
        uint16_t p;
        if (parse_port_token(port, &p) < 0) {
            return -1;
        }
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        return -1;
    }
    if (res->ai_addrlen > sizeof(*ss)) {
        freeaddrinfo(res);
        return -1;
    }
    memset(ss, 0, sizeof(*ss));
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int add_route(tiny_site *site, uint8_t type, const char *prefix, const char *target,
                     const char *file, int lineno) {
    tiny_route *r;
    int n;

    if (site->nroutes >= TINY_MAX_ROUTES) {
        fprintf(stderr, "tiny: %s:%d: too many routes\n", file, lineno);
        return -1;
    }
    if (!prefix_ok(prefix)) {
        fprintf(stderr, "tiny: %s:%d: route prefix must be an absolute path\n", file, lineno);
        return -1;
    }
    r = &site->routes[site->nroutes];
    memset(r, 0, sizeof(*r));
    r->dir_fd = -1;
    r->type = type;
    n = copy_token(r->prefix, sizeof(r->prefix), prefix);
    if (n < 0) {
        fprintf(stderr, "tiny: %s:%d: prefix too long\n", file, lineno);
        return -1;
    }
    r->prefix_len = (uint16_t)n;
    n = copy_token(r->target, sizeof(r->target), target);
    if (n < 0) {
        fprintf(stderr, "tiny: %s:%d: route target too long\n", file, lineno);
        return -1;
    }
    r->target_len = (uint16_t)n;
    if (type == ROUTE_PROXY) {
        if (parse_upstream(target, &r->upstream, &r->upstream_len) < 0) {
            fprintf(stderr, "tiny: %s:%d: bad upstream '%s'\n", file, lineno, target);
            return -1;
        }
    }
    site->nroutes++;
    return 0;
}

/* Returns 1 and writes the token, 0 if the line is done. */
static int next_tok(char **pp, char **tok) {
    char *p = *pp;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r') {
        *pp = p;
        return 0;
    }
    *tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#') {
        p++;
    }
    if (*p == '#') {
        *p = '\0';
        *pp = p;
        return 1;
    }
    if (*p) {
        *p = '\0';
        p++;
    }
    *pp = p;
    return 1;
}

static int site_check(tiny_site *site, const char *file);

int tiny_site_load(tiny_site *site, const char *path) {
    FILE *fp;
    char line[1024];
    int lineno = 0;
    int saw_listen = 0;

    site_clear(site);
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "tiny: open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *kw = NULL;
        char *a = NULL;
        char *b = NULL;
        char *extra = NULL;
        size_t n;

        lineno++;
        n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "tiny: %s:%d: line too long\n", path, lineno);
            fclose(fp);
            return -1;
        }
        if (!next_tok(&p, &kw)) {
            continue;
        }
        if (strcmp(kw, "listen") == 0) {
            uint16_t port;
            if (!next_tok(&p, &a) || next_tok(&p, &extra) || parse_port_token(a, &port) < 0) {
                fprintf(stderr, "tiny: %s:%d: listen <port>\n", path, lineno);
                fclose(fp);
                return -1;
            }
            if (saw_listen) {
                fprintf(stderr, "tiny: %s:%d: listen repeated\n", path, lineno);
                fclose(fp);
                return -1;
            }
            site->port = port;
            saw_listen = 1;
            continue;
        }
        if (strcmp(kw, "static") != 0 && strcmp(kw, "proxy") != 0 && strcmp(kw, "app") != 0) {
            fprintf(stderr, "tiny: %s:%d: unknown directive '%s'\n", path, lineno, kw);
            fclose(fp);
            return -1;
        }
        if (!next_tok(&p, &a) || !next_tok(&p, &b) || next_tok(&p, &extra)) {
            fprintf(stderr, "tiny: %s:%d: %s <prefix> <target>\n", path, lineno, kw);
            fclose(fp);
            return -1;
        }
        {
            uint8_t type = ROUTE_STATIC;
            if (strcmp(kw, "proxy") == 0) {
                type = ROUTE_PROXY;
            } else if (strcmp(kw, "app") == 0) {
                type = ROUTE_APP;
            }
            if (add_route(site, type, a, b, path, lineno) < 0) {
                fclose(fp);
                return -1;
            }
        }
    }
    fclose(fp);
    if (!saw_listen) {
        fprintf(stderr, "tiny: %s: missing listen\n", path);
        return -1;
    }
    if (site_check(site, path) < 0) {
        return -1;
    }
    return 0;
}

static int same_prefix(const tiny_route *a, const tiny_route *b) {
    return a->prefix_len == b->prefix_len &&
           memcmp(a->prefix, b->prefix, a->prefix_len) == 0;
}

static int ensure_dir_slash(tiny_route *r) {
    if (r->prefix_len == 1 && r->prefix[0] == '/') {
        return 0;
    }
    if (r->prefix[r->prefix_len - 1] == '/') {
        return 0;
    }
    if ((size_t)r->prefix_len + 1 >= sizeof(r->prefix)) {
        return -1;
    }
    r->prefix[r->prefix_len++] = '/';
    r->prefix[r->prefix_len] = '\0';
    return 0;
}

/* Stat static targets and reject duplicate prefixes. Does not open fds. */
static int site_check(tiny_site *site, const char *file) {
    int service = 0;

    if (site->nroutes == 0) {
        fprintf(stderr, "tiny: %s: missing route\n", file);
        return -1;
    }
    for (uint16_t i = 0; i < site->nroutes; i++) {
        tiny_route *r = &site->routes[i];
        struct stat st;

        if (r->type == ROUTE_PROXY) {
            service = 1;
            continue;
        }
        if (r->type != ROUTE_STATIC) {
            continue;
        }
        service = 1;
        if (stat(r->target, &st) < 0) {
            fprintf(stderr, "tiny: %s: stat '%s': %s\n", file, r->target, strerror(errno));
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (ensure_dir_slash(r) < 0) {
                fprintf(stderr, "tiny: %s: prefix too long\n", file);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (r->prefix_len > 1 && r->prefix[r->prefix_len - 1] == '/') {
                fprintf(stderr, "tiny: %s: file route '%s' should not end with /\n",
                        file, r->prefix);
                return -1;
            }
        } else {
            fprintf(stderr, "tiny: %s: '%s' is not a file or directory\n", file, r->target);
            return -1;
        }
    }
    if (!service) {
        fprintf(stderr, "tiny: %s: need a static or proxy route\n", file);
        return -1;
    }
    for (uint16_t i = 0; i < site->nroutes; i++) {
        for (uint16_t j = 0; j < i; j++) {
            if (same_prefix(&site->routes[i], &site->routes[j])) {
                fprintf(stderr, "tiny: %s: duplicate route '%s'\n",
                        file, site->routes[i].prefix);
                return -1;
            }
        }
    }
    return 0;
}

void routes_shutdown(void) {
    uint16_t n = g_site.nroutes;
    int ready = g_ready;

    for (uint16_t i = 0; i < n; i++) {
        if (g_site.routes[i].dir_fd >= 0) {
            close(g_site.routes[i].dir_fd);
            g_site.routes[i].dir_fd = -1;
        }
    }
    if (ready || n) {
        static_shutdown();
    }
    g_ready = 0;
    g_site.nroutes = 0;
}

int routes_init(const tiny_site *site) {
    int primary = 0;

    routes_shutdown();
    site_clear(&g_site);
    if (!site || site->port == 0) {
        fprintf(stderr, "tiny: missing listen port\n");
        return -1;
    }
    g_site.port = site->port;
    g_site.nroutes = site->nroutes;
    for (uint16_t i = 0; i < site->nroutes; i++) {
        const tiny_route *src = &site->routes[i];
        tiny_route *r = &g_site.routes[i];
        struct stat st;

        *r = *src;
        r->dir_fd = -1;
        r->exact = 0;
        r->primary = 0;
        if (r->type != ROUTE_STATIC) {
            if (r->type == ROUTE_APP) {
                fprintf(stderr, "tiny: app route '%s' is not implemented; requests get 501\n",
                        r->prefix);
            }
            continue;
        }
        if (stat(r->target, &st) < 0) {
            fprintf(stderr, "tiny: stat '%s': %s\n", r->target, strerror(errno));
            routes_shutdown();
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (ensure_dir_slash(r) < 0) {
                fprintf(stderr, "tiny: prefix too long\n");
                routes_shutdown();
                return -1;
            }
            if (r->prefix_len == 1 && r->prefix[0] == '/') {
                if (primary) {
                    fprintf(stderr, "tiny: more than one static / route\n");
                    routes_shutdown();
                    return -1;
                }
                if (static_init(r->target) < 0) {
                    routes_shutdown();
                    return -1;
                }
                r->primary = 1;
                primary = 1;
            } else {
                int fd = open(r->target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (fd < 0) {
                    fprintf(stderr, "tiny: open '%s': %s\n", r->target, strerror(errno));
                    routes_shutdown();
                    return -1;
                }
                r->dir_fd = fd;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (r->prefix[r->prefix_len - 1] == '/' && r->prefix_len > 1) {
                fprintf(stderr, "tiny: file route '%s' should not end with /\n", r->prefix);
                routes_shutdown();
                return -1;
            }
            r->exact = 1;
        } else {
            fprintf(stderr, "tiny: '%s' is not a file or directory\n", r->target);
            routes_shutdown();
            return -1;
        }
    }
    for (uint16_t i = 0; i < g_site.nroutes; i++) {
        for (uint16_t j = 0; j < i; j++) {
            if (same_prefix(&g_site.routes[i], &g_site.routes[j])) {
                fprintf(stderr, "tiny: duplicate route '%s'\n", g_site.routes[i].prefix);
                routes_shutdown();
                return -1;
            }
        }
    }
    g_ready = 1;
#ifdef TINY_VERBOSE
    for (uint16_t i = 0; i < g_site.nroutes; i++) {
        tiny_route *r = &g_site.routes[i];
        TINY_LOG_INFO("[ROUTE] %s %s -> %s\n",
                      r->type == ROUTE_PROXY ? "proxy" :
                      r->type == ROUTE_APP ? "app" : "static",
                      r->prefix, r->target);
    }
#endif
    return 0;
}

const char *routes_docroot(void) {
    for (uint16_t i = 0; i < g_site.nroutes; i++) {
        if (g_site.routes[i].primary) {
            return g_site.routes[i].target;
        }
    }
    return "";
}

void route_bind(int epfd, void *slot, uint32_t idx) {
    g_epfd = epfd;
    g_slot = (tiny_slot *)slot;
    g_idx = idx;
}

static int route_matches(const tiny_route *r, const char *path, uint16_t plen) {
    uint16_t n = r->prefix_len;

    if (r->exact) {
        return plen == n && memcmp(path, r->prefix, n) == 0;
    }
    if (n == 1 && r->prefix[0] == '/') {
        return 1;
    }
    if (n > 0 && r->prefix[n - 1] == '/') {
        if (plen + 1 == n && memcmp(path, r->prefix, plen) == 0) {
            return 1;
        }
        if (plen >= n && memcmp(path, r->prefix, n) == 0) {
            return 1;
        }
        return 0;
    }
    if (plen < n || memcmp(path, r->prefix, n) != 0) {
        return 0;
    }
    return plen == n || path[n] == '/';
}

static const tiny_route *route_match(const char *path, uint16_t plen) {
    const tiny_route *best = NULL;

    for (uint16_t i = 0; i < g_site.nroutes; i++) {
        const tiny_route *r = &g_site.routes[i];
        if (!route_matches(r, path, plen)) {
            continue;
        }
        if (!best || r->prefix_len > best->prefix_len) {
            best = r;
        }
    }
    return best;
}

static int32_t serve_mount(http_conn *hc, const tiny_route *r,
                           const char *path, uint16_t plen, uint64_t now_ms) {
    uint16_t cut = r->prefix_len;
    const char *url;
    uint16_t ulen;
    char slash[2];

    if (cut > 0 && r->prefix[cut - 1] == '/') {
        cut--;
    }
    if (plen <= cut) {
        slash[0] = '/';
        slash[1] = '\0';
        url = slash;
        ulen = 1;
    } else {
        url = path + cut;
        ulen = (uint16_t)(plen - cut);
        if (url[0] != '/') {
            return arm_close(hc, RESP_500, 500, now_ms);
        }
    }
    return static_begin_at(hc, r->dir_fd, url, ulen, path, plen, now_ms);
}

/* APP is intentionally a stub. The backend protocol is not HTTP. */
static int32_t app_begin(http_conn *hc, const tiny_route *route, uint64_t now_ms) {
    (void)route;
    return arm_close(hc, RESP_501, 501, now_ms);
}

int32_t route_begin(http_conn *hc, uint64_t now_ms) {
    const char *path;
    uint16_t plen;
    char root_path[2];
    const tiny_route *r;

    /* Bench and the old CLI are one static "/". Skip the table walk. */
    if (g_site.nroutes == 1 && g_site.routes[0].primary) {
        return static_begin(hc, now_ms);
    }

    if (hc->req.path.len == 0) {
        root_path[0] = '/';
        root_path[1] = '\0';
        path = root_path;
        plen = 1;
    } else {
        path = hc->buf + hc->req.path.off;
        plen = hc->req.path.len;
    }

    r = route_match(path, plen);
    if (!r) {
        return arm_close(hc, RESP_404, 404, now_ms);
    }
    switch (r->type) {
    case ROUTE_STATIC:
        if (r->exact) {
            return static_begin_file(hc, r->target, now_ms);
        }
        if (r->primary) {
            return static_begin(hc, now_ms);
        }
        return serve_mount(hc, r, path, plen, now_ms);
    case ROUTE_PROXY:
        if (!g_slot || g_epfd < 0) {
            return arm_close(hc, RESP_500, 500, now_ms);
        }
        {
            int32_t rc = proxy_begin(hc, &g_slot->px, r, g_epfd, g_idx, now_ms);
            g_slot->proxying = g_slot->px.fd >= 0;
            return rc;
        }
    case ROUTE_APP:
        return app_begin(hc, r, now_ms);
    default:
        return arm_close(hc, RESP_500, 500, now_ms);
    }
}
