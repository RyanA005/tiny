#define _GNU_SOURCE

#include "proxy.h"
#include "stats.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

/*
 * Proxied slot, one worker, two sockets:
 *
 *   client fd  -- tiny slot -- upstream fd
 *
 * Both fds can sit in that worker's epoll. The upstream registration
 * sets bit 31 of epoll data so the worker can tell the ends apart.
 * The upstream fd is registered only when a read or write returns
 * EAGAIN, so a request that finishes inline does not touch epoll.
 * There is no proxy thread. Idle upstream sockets sit in a per-worker
 * stack (no lock), the same idea as the open-file cache.
 *
 *   take idle upstream, or connect
 *        -> send rewritten request (Connection: keep-alive)
 *        -> send buffered body, then unread Content-Length bytes
 *        -> read upstream headers
 *        -> send rewritten response headers
 *        -> stream the body (Content-Length, raw chunked, or until EOF)
 *
 * Hop-by-hop headers are stripped. The client is kept when the request
 * allows it. The upstream socket goes back on the idle stack when the
 * body had a known end and the response did not say Connection: close.
 * A response with no length still closes the upstream. A reused socket
 * that fails before any byte of this request is written is dropped and
 * a new connect is tried once. A bodyless upstream request is one send.
 * A response whose body is already buffered is one sendmsg. Cork stays
 * for a request body and for a response that is still streaming.
 *
 * Backpressure is a single shuttle buffer. If it still holds bytes,
 * only the writer is armed. If it is empty, only the reader is armed.
 */

enum {
    PX_SEND_REQ = 1,
    PX_SEND_BODY,
    PX_RECV_HDR,
    PX_SEND_RESP,
    PX_STREAM
};

enum {
    CH_SIZE = 0,
    CH_EXT,
    CH_SIZE_LF,
    CH_DATA,
    CH_DATA_CR,
    CH_DATA_LF,
    CH_TRAIL,
    CH_TRAIL_CR,
    CH_SKIP,
    CH_SKIP_CR
};

static const char RESP_405[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "Allow: GET, HEAD, POST\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESP_411[] =
    "HTTP/1.1 411 Length Required\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESP_502[] =
    "HTTP/1.1 502 Bad Gateway\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char CONT_100[] = "HTTP/1.1 100 Continue\r\n\r\n";
#define CONT_100_LEN 25
_Static_assert(sizeof(CONT_100) - 1 == CONT_100_LEN, "100 Continue length");

static const char CONNECTION_CLOSE[] = "Connection: close\r\n\r\n";
static const char CONNECTION_KEEP[] = "Connection: keep-alive\r\n\r\n";

/* One idle upstream per slot is enough: a finished request returns its
 * fd here, and a new request takes it. Peak fds stay one per slot. */
#define PROXY_POOL_MAX TINY_CONNS_PER_WORKER

typedef struct {
    int32_t fd;
    socklen_t len;
    struct sockaddr_storage addr;
} proxy_idle;

static __thread proxy_idle proxy_pool[PROXY_POOL_MAX];
static __thread uint32_t proxy_pool_n;

static int addr_eq(const struct sockaddr_storage *a, socklen_t alen,
                   const struct sockaddr *b, socklen_t blen) {
    return alen == blen && alen > 0 &&
           memcmp(a, b, (size_t)alen) == 0;
}

static int proxy_pool_take(const struct sockaddr *addr, socklen_t len) {
    uint32_t i;

    for (i = proxy_pool_n; i-- > 0;) {
        if (addr_eq(&proxy_pool[i].addr, proxy_pool[i].len, addr, len)) {
            int32_t fd = proxy_pool[i].fd;
            proxy_pool[i] = proxy_pool[proxy_pool_n - 1];
            proxy_pool_n--;
            return fd;
        }
    }
    return -1;
}

/* 1: stored. 0: caller still owns fd. */
static int proxy_pool_put(int fd, const struct sockaddr_storage *addr, socklen_t len) {
    if (fd < 0 || len <= 0 || proxy_pool_n >= PROXY_POOL_MAX) {
        return 0;
    }
    proxy_pool[proxy_pool_n].fd = fd;
    proxy_pool[proxy_pool_n].len = len;
    proxy_pool[proxy_pool_n].addr = *addr;
    proxy_pool_n++;
    return 1;
}

void proxy_pool_clear(void) {
    while (proxy_pool_n > 0) {
        proxy_pool_n--;
        if (proxy_pool[proxy_pool_n].fd >= 0) {
            close(proxy_pool[proxy_pool_n].fd);
        }
        proxy_pool[proxy_pool_n].fd = -1;
    }
}

void proxy_conn_reset(proxy_conn *px) {
    px->fd = -1;
    px->phase = 0;
    px->armed = 0;
    px->in_epoll = 0;
    px->flags = 0;
    px->chunk_state = 0;
    px->saw_digit = 0;
    px->cont_off = 0;
    px->status = 0;
    px->hdr_off = 0;
    px->hdr_len = 0;
    px->buf_off = 0;
    px->buf_len = 0;
    px->body_off = 0;
    px->body_end = 0;
    px->body_left = 0;
    px->resp_left = 0;
    px->chunk_size = 0;
    px->up_len = 0;
}

void proxy_close(int epfd, proxy_conn *px) {
    int off;

    if (!px || px->fd < 0) {
        return;
    }
    if (px->in_epoll && epfd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, px->fd, NULL);
    }
    if (px->flags & PX_CORK) {
        off = 0;
        setsockopt(px->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
    }
    close(px->fd);
    proxy_conn_reset(px);
}

static void touch(http_conn *hc, uint64_t now_ms) {
    hc->last_active_ms = now_ms;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
}

static int32_t arm_fixed(http_conn *hc, const char *resp, uint16_t status,
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

static int32_t proxy_fail(http_conn *hc, proxy_conn *px, int epfd, uint64_t now_ms) {
    uint8_t sent = (uint16_t)(px->flags & PX_RESP_SENT);

    proxy_close(epfd, px);
    if (sent) {
        return HTTP_IO_CLOSE;
    }
    return arm_fixed(hc, RESP_502, 502, now_ms);
}

static int proxy_can_pool(const proxy_conn *px) {
    if (px->fd < 0 || px->up_len == 0) {
        return 0;
    }
    if (px->flags & (PX_UP_CLOSE | PX_OVERREAD | PX_RESP_EOF)) {
        return 0;
    }
    return (px->flags & PX_BODY_DONE) ? 1 : 0;
}

/* Drop epoll and cork, then park the fd. Does not touch the bytes. */
static void proxy_release(int epfd, proxy_conn *px) {
    int fd = px->fd;
    struct sockaddr_storage addr = px->up_addr;
    socklen_t len = px->up_len;
    int off;

    if (px->in_epoll && epfd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, px->fd, NULL);
    }
    if (px->flags & PX_CORK) {
        off = 0;
        setsockopt(px->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
    }
    proxy_conn_reset(px);
    if (!proxy_pool_put(fd, &addr, len)) {
        close(fd);
    }
}

static void cork_up(proxy_conn *px, int on);

/* Reused fd died before this request was written. Connect a fresh one. */
static int proxy_reopen(proxy_conn *px, int epfd) {
    int fd;
    int rc;
    int off;

    if (px->fd >= 0) {
        if (px->in_epoll && epfd >= 0) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, px->fd, NULL);
        }
        if (px->flags & PX_CORK) {
            off = 0;
            setsockopt(px->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
        }
        close(px->fd);
    }
    px->fd = -1;
    px->in_epoll = 0;
    px->armed = 0;
    px->flags = (uint16_t)(px->flags & ~(PX_CORK | PX_REUSED));
    px->hdr_off = 0;

    fd = socket(px->up_addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    px->fd = fd;
    if (px->body_off < px->body_end || px->body_left > 0) {
        cork_up(px, 1);
    }
    rc = connect(fd, (struct sockaddr *)&px->up_addr, px->up_len);
    if (rc < 0 && errno != EINPROGRESS) {
        return -1;
    }
    return 0;
}

/* Idle fd accepted nothing we can trust. GET/HEAD only, and only before
 * any response byte. 1: resent. 0: not eligible. -1: reconnect failed. */
static int proxy_retry_idle(http_conn *hc, proxy_conn *px, int epfd) {
    if (!(px->flags & PX_REUSED) || (px->flags & PX_RESP_SENT) || px->buf_len != 0) {
        return 0;
    }
    if (hc->req.method != HTTP_METHOD_GET && hc->req.method != HTTP_METHOD_HEAD) {
        return 0;
    }
    if (proxy_reopen(px, epfd) < 0) {
        return -1;
    }
    px->buf_off = 0;
    px->buf_len = 0;
    px->phase = PX_SEND_REQ;
    return 1;
}

static void cork_client(http_conn *hc, int on) {
    int v = on ? 1 : 0;

    if (hc->conn.fd < 0) {
        return;
    }
    if (on && hc->cork_on) {
        return;
    }
    if (!on && !hc->cork_on) {
        return;
    }
    if (setsockopt(hc->conn.fd, IPPROTO_TCP, TCP_CORK, &v, sizeof(v)) < 0) {
        return;
    }
    hc->cork_on = on ? 1 : 0;
}

static int32_t proxy_done(http_conn *hc, proxy_conn *px, int epfd) {
    uint16_t code = px->status ? px->status : 200;
    uint8_t keep = (px->flags & PX_CLIENT_KEEP) ? 1 : 0;

    /* Streaming responses corked the client. A buffered response did not. */
    cork_client(hc, 0);
    if (proxy_can_pool(px)) {
        proxy_release(epfd, px);
    } else {
        proxy_close(epfd, px);
    }
    hc->keep = keep;
    hc->status_code = code;
    STAT_NOTE_STATUS(code);
    return HTTP_IO_DONE;
}

static void cork_up(proxy_conn *px, int on) {
    int v = on ? 1 : 0;

    if (px->fd < 0) {
        return;
    }
    if (!on && !(px->flags & PX_CORK)) {
        return;
    }
    if (setsockopt(px->fd, IPPROTO_TCP, TCP_CORK, &v, sizeof(v)) < 0) {
        return;
    }
    if (on) {
        px->flags = (uint16_t)(px->flags | PX_CORK);
    } else {
        px->flags = (uint16_t)(px->flags & ~PX_CORK);
    }
}

/* Bit 31 marks the upstream end of this slot. Wake uses 0xffffffff and
 * is handled before this mask, so it cannot be confused with a slot. */
static int32_t arm_up(proxy_conn *px, int epfd, uint32_t idx, int32_t io) {
    struct epoll_event ev;
    int op;

    if (px->fd < 0) {
        return -1;
    }
    if (px->in_epoll && px->armed == (uint8_t)io) {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.data.u32 = idx | 0x80000000u;
    ev.events = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (io == HTTP_IO_WANT_READ) {
        ev.events |= EPOLLIN;
    } else if (io == HTTP_IO_WANT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    op = px->in_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(epfd, op, px->fd, &ev) < 0) {
        return -1;
    }
    px->in_epoll = 1;
    px->armed = (uint8_t)io;
    return 0;
}

static inline uint8_t lower_byte(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c - 'A' + 'a');
    }
    return c;
}

static int ieq(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (lower_byte((uint8_t)a[i]) != (uint8_t)b[i]) {
            return 0;
        }
    }
    return 1;
}

static const char *find_crlf(const char *p, const char *end) {
    for (; p + 1 < end; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
    }
    return NULL;
}

static int hop_by_hop(const char *name, uint32_t n, int request) {
    if (n == 10 && ieq(name, "connection", 10)) return 1;
    if (n == 10 && ieq(name, "keep-alive", 10)) return 1;
    if (n == 16 && ieq(name, "proxy-connection", 16)) return 1;
    if (n == 7 && ieq(name, "upgrade", 7)) return 1;
    if (n == 2 && ieq(name, "te", 2)) return 1;
    if (n == 7 && ieq(name, "trailer", 7)) return 1;
    if (request && n == 6 && ieq(name, "expect", 6)) return 1;
    if (request && n == 17 && ieq(name, "transfer-encoding", 17)) return 1;
    return 0;
}

static int append_bytes(char *dst, uint32_t cap, uint32_t *len,
                        const char *s, uint32_t n) {
    if (n > cap - *len) {
        return -1;
    }
    memcpy(dst + *len, s, n);
    *len += n;
    return 0;
}

static int rewrite_request(http_conn *hc, proxy_conn *px) {
    const char *p = hc->buf;
    const char *end = hc->buf + hc->req.header_bytes;
    const char *eol;
    uint32_t len = 0;

    eol = find_crlf(p, end);
    if (!eol) {
        return -1;
    }
    if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, p, (uint32_t)(eol + 2 - p)) < 0) {
        return -1;
    }
    p = eol + 2;
    while (p < end) {
        const char *colon;

        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') {
            break;
        }
        eol = find_crlf(p, end);
        if (!eol) {
            return -1;
        }
        colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon) {
            return -1;
        }
        if (!hop_by_hop(p, (uint32_t)(colon - p), 1)) {
            if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, p,
                             (uint32_t)(eol + 2 - p)) < 0) {
                return -1;
            }
        }
        p = eol + 2;
    }
    if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, CONNECTION_KEEP,
                     (uint32_t)(sizeof(CONNECTION_KEEP) - 1)) < 0) {
        return -1;
    }
    px->hdr_off = 0;
    px->hdr_len = len;
    return 0;
}

static int expects_continue(const http_conn *hc) {
    const char *p = hc->buf + hc->req.header_block.off;
    const char *end = p + hc->req.header_block.len;

    while (p < end) {
        const char *eol = find_crlf(p, end);
        const char *colon;
        const char *v;
        uint32_t n;

        if (!eol) {
            break;
        }
        colon = memchr(p, ':', (size_t)(eol - p));
        if (colon && (uint32_t)(colon - p) == 6 && ieq(p, "expect", 6)) {
            v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            n = (uint32_t)(eol - v);
            while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
                n--;
            }
            if (n == 12 && ieq(v, "100-continue", 12)) {
                return 1;
            }
        }
        p = eol + 2;
    }
    return 0;
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Bytes are forwarded unchanged. The state machine only finds the end. */
static uint32_t chunk_feed(proxy_conn *px, const char *s, uint32_t n,
                           int *done, int *bad) {
    uint32_t i = 0;

    *done = 0;
    *bad = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        switch (px->chunk_state) {
        case CH_SIZE: {
            int h = hex_val(c);
            if (h >= 0) {
                if (px->chunk_size > (UINT64_MAX >> 4)) {
                    *bad = 1;
                    return i;
                }
                px->chunk_size = (px->chunk_size << 4) | (uint64_t)h;
                px->saw_digit = 1;
                i++;
                break;
            }
            if (c == ';') {
                if (!px->saw_digit) {
                    *bad = 1;
                    return i;
                }
                px->chunk_state = CH_EXT;
                i++;
                break;
            }
            if (c == '\r') {
                if (!px->saw_digit) {
                    *bad = 1;
                    return i;
                }
                px->chunk_state = CH_SIZE_LF;
                i++;
                break;
            }
            *bad = 1;
            return i;
        }
        case CH_EXT:
            if (c == '\n') {
                *bad = 1;
                return i;
            }
            if (c == '\r') {
                px->chunk_state = CH_SIZE_LF;
            }
            i++;
            break;
        case CH_SIZE_LF:
            if (c != '\n') {
                *bad = 1;
                return i;
            }
            i++;
            px->chunk_state = px->chunk_size == 0 ? CH_TRAIL : CH_DATA;
            break;
        case CH_DATA: {
            uint32_t left = n - i;
            if ((uint64_t)left > px->chunk_size) {
                left = (uint32_t)px->chunk_size;
            }
            i += left;
            px->chunk_size -= left;
            if (px->chunk_size == 0) {
                px->chunk_state = CH_DATA_CR;
            }
            break;
        }
        case CH_DATA_CR:
            if (c != '\r') {
                *bad = 1;
                return i;
            }
            px->chunk_state = CH_DATA_LF;
            i++;
            break;
        case CH_DATA_LF:
            if (c != '\n') {
                *bad = 1;
                return i;
            }
            px->chunk_state = CH_SIZE;
            px->saw_digit = 0;
            px->chunk_size = 0;
            i++;
            break;
        case CH_TRAIL:
            px->chunk_state = (c == '\r') ? CH_TRAIL_CR : CH_SKIP;
            i++;
            break;
        case CH_TRAIL_CR:
            if (c != '\n') {
                *bad = 1;
                return i;
            }
            i++;
            *done = 1;
            return i;
        case CH_SKIP:
            if (c == '\r') {
                px->chunk_state = CH_SKIP_CR;
            }
            i++;
            break;
        case CH_SKIP_CR:
            if (c != '\n') {
                *bad = 1;
                return i;
            }
            px->chunk_state = CH_TRAIL;
            i++;
            break;
        default:
            *bad = 1;
            return i;
        }
    }
    return i;
}

static int parse_u64(const char *p, uint32_t n, uint64_t *out) {
    uint64_t v = 0;

    if (n == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < '0' || c > '9') {
            return -1;
        }
        if (v > (UINT64_MAX - (c - '0')) / 10) {
            return -1;
        }
        v = v * 10u + (uint64_t)(c - '0');
    }
    *out = v;
    return 0;
}

static int value_is_chunked(const char *v, uint32_t n) {
    while (n > 0 && (v[0] == ' ' || v[0] == '\t')) {
        v++;
        n--;
    }
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
        n--;
    }
    return n == 7 && ieq(v, "chunked", 7);
}

static int header_has_token(const char *v, uint32_t n, const char *tok, uint32_t tn) {
    uint32_t i = 0;

    while (i < n) {
        uint32_t s;
        uint32_t e;

        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
            i++;
        }
        s = i;
        while (i < n && v[i] != ',') {
            i++;
        }
        e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) {
            e--;
        }
        if (e - s == tn && ieq(v + s, tok, tn)) {
            return 1;
        }
    }
    return 0;
}

/* 1: skip this informational block. 0: final headers are in px->hdr. -1: bad. */
static int rewrite_response(http_conn *hc, proxy_conn *px, const char *src, uint32_t n) {
    const char *end = src + n;
    const char *eol = find_crlf(src, end);
    const char *sp;
    const char *p;
    const char *conn;
    unsigned code;
    uint32_t len = 0;
    int saw_cl = 0;
    int saw_te = 0;
    int up_close = 0;
    int up_ka = 0;

    if (!eol || (size_t)(eol - src) < 12) {
        return -1;
    }
    if (memcmp(src, "HTTP/1.", 7) != 0) {
        return -1;
    }
    sp = memchr(src, ' ', (size_t)(eol - src));
    if (!sp || (eol - (sp + 1)) < 3) {
        return -1;
    }
    if (sp[1] < '0' || sp[1] > '9' || sp[2] < '0' || sp[2] > '9' ||
        sp[3] < '0' || sp[3] > '9') {
        return -1;
    }
    if (sp + 4 < eol && sp[4] != ' ' && sp[4] != '\r') {
        return -1;
    }
    code = (unsigned)(sp[1] - '0') * 100u +
           (unsigned)(sp[2] - '0') * 10u +
           (unsigned)(sp[3] - '0');
    if (code >= 100 && code < 200) {
        return code == 101 ? -1 : 1;
    }

    px->status = (uint16_t)code;
    px->flags = (uint16_t)(px->flags & ~(PX_RESP_CL | PX_RESP_CHUNKED | PX_RESP_EOF | PX_BODY_DONE));
    px->resp_left = 0;
    px->chunk_state = CH_SIZE;
    px->saw_digit = 0;
    px->chunk_size = 0;

    if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, src, (uint32_t)(eol + 2 - src)) < 0) {
        return -1;
    }
    p = eol + 2;
    while (p + 1 < end) {
        const char *colon;
        const char *v;
        const char *vend;
        uint32_t nlen;

        if (p[0] == '\r' && p[1] == '\n') {
            break;
        }
        eol = find_crlf(p, end);
        if (!eol) {
            return -1;
        }
        colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon) {
            return -1;
        }
        nlen = (uint32_t)(colon - p);
        v = colon + 1;
        vend = eol;
        while (v < vend && (*v == ' ' || *v == '\t')) {
            v++;
        }
        while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) {
            vend--;
        }
        if (nlen == 14 && ieq(p, "content-length", 14)) {
            uint64_t cl;
            if (saw_cl || parse_u64(v, (uint32_t)(vend - v), &cl) < 0) {
                return -1;
            }
            saw_cl = 1;
            px->resp_left = cl;
            px->flags = (uint16_t)(px->flags | PX_RESP_CL);
        } else if (nlen == 17 && ieq(p, "transfer-encoding", 17)) {
            if (saw_te || !value_is_chunked(v, (uint32_t)(vend - v))) {
                return -1;
            }
            saw_te = 1;
            px->flags = (uint16_t)(px->flags | PX_RESP_CHUNKED);
        } else if (nlen == 10 && ieq(p, "connection", 10)) {
            uint32_t vn = (uint32_t)(vend - v);
            if (header_has_token(v, vn, "close", 5)) {
                up_close = 1;
            }
            if (header_has_token(v, vn, "keep-alive", 10)) {
                up_ka = 1;
            }
        }
        if (!hop_by_hop(p, nlen, 0)) {
            if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, p,
                             (uint32_t)(eol + 2 - p)) < 0) {
                return -1;
            }
        }
        p = eol + 2;
    }
    if ((px->flags & PX_RESP_CL) && (px->flags & PX_RESP_CHUNKED)) {
        return -1;
    }
    if (code == 204 || code == 304) {
        px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
    } else if (!(px->flags & PX_RESP_CL) && !(px->flags & PX_RESP_CHUNKED)) {
        px->flags = (uint16_t)(px->flags | PX_RESP_EOF);
    } else if ((px->flags & PX_RESP_CL) && px->resp_left == 0) {
        px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
    }
    /* HTTP/1.0 stays open only when the upstream asked for it. */
    if (src[7] == '0' && !up_ka) {
        up_close = 1;
    }
    if (up_close) {
        px->flags = (uint16_t)(px->flags | PX_UP_CLOSE);
    }
    if (http_should_keepalive(&hc->req)) {
        px->flags = (uint16_t)(px->flags | PX_CLIENT_KEEP);
    }
    conn = (px->flags & PX_CLIENT_KEEP) ? CONNECTION_KEEP : CONNECTION_CLOSE;
    if (append_bytes(px->hdr, PROXY_HDR_MAX, &len, conn,
                     (px->flags & PX_CLIENT_KEEP)
                         ? (uint32_t)(sizeof(CONNECTION_KEEP) - 1)
                         : (uint32_t)(sizeof(CONNECTION_CLOSE) - 1)) < 0) {
        return -1;
    }
    px->hdr_off = 0;
    px->hdr_len = len;
    return 0;
}

static int find_hdr_end(const char *b, uint32_t n, uint32_t *at) {
    if (n < 4) {
        return 0;
    }
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
            *at = i;
            return 1;
        }
    }
    return 0;
}

/* Apply framing to the last n bytes of px->buf. Shrinks buf past the end. */
static int note_body(proxy_conn *px, uint32_t n) {
    int done = 0;
    int bad = 0;

    if (n == 0 || (px->flags & PX_BODY_DONE)) {
        return 0;
    }
    if (px->flags & PX_RESP_CHUNKED) {
        uint32_t base = px->buf_len - n;
        uint32_t c = chunk_feed(px, px->buf + base, n, &done, &bad);
        if (bad) {
            return -1;
        }
        if (c < n) {
            px->buf_len = base + c;
            px->flags = (uint16_t)(px->flags | PX_OVERREAD);
        }
        if (done) {
            px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
        }
        return 0;
    }
    if (px->flags & PX_RESP_CL) {
        if ((uint64_t)n > px->resp_left) {
            px->flags = (uint16_t)(px->flags | PX_OVERREAD);
            px->buf_len -= n - (uint32_t)px->resp_left;
            n = (uint32_t)px->resp_left;
        }
        px->resp_left -= n;
        if (px->resp_left == 0) {
            px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
        }
    }
    return 0;
}

/* 1: EINTR, caller retries. 0: progress, EAGAIN (*again), or EOF. -1: error. */
static int sock_read(int fd, char *p, uint32_t *len, uint32_t cap,
                     int *again, int *eof) {
    ssize_t n;

    *again = 0;
    *eof = 0;
    if (*len >= cap) {
        return 0;
    }
    n = read(fd, p + *len, cap - *len);
    if (n < 0) {
        if (errno == EINTR) {
            return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *again = 1;
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        *eof = 1;
        return 0;
    }
    *len += (uint32_t)n;
    return 0;
}

/* Header plus an already-buffered body, one sendmsg. */
static int send_gathered(http_conn *hc, proxy_conn *px, int *again, int *dead) {
    struct iovec iov[2];
    struct msghdr msg;
    uint32_t hdr_n = 0;
    int nvec = 0;
    ssize_t n;

    *again = 0;
    *dead = 0;
    if (px->hdr_off < px->hdr_len) {
        hdr_n = px->hdr_len - px->hdr_off;
        iov[nvec].iov_base = px->hdr + px->hdr_off;
        iov[nvec].iov_len = hdr_n;
        nvec++;
    }
    if (px->buf_off < px->buf_len) {
        iov[nvec].iov_base = px->buf + px->buf_off;
        iov[nvec].iov_len = px->buf_len - px->buf_off;
        nvec++;
    }
    if (nvec == 0) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (size_t)nvec;
    do {
        n = sendmsg(hc->conn.fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *again = 1;
            return 0;
        }
        *dead = 1;
        return -1;
    }
    if (n == 0) {
        *dead = 1;
        return -1;
    }
    if ((uint32_t)n < hdr_n) {
        px->hdr_off += (uint32_t)n;
    } else {
        px->hdr_off += hdr_n;
        px->buf_off += (uint32_t)n - hdr_n;
    }
    return 0;
}

static int sock_send(int fd, const char *p, uint32_t *off, uint32_t len,
                     int *again, int *dead) {
    *again = 0;
    *dead = 0;
    while (*off < len) {
        ssize_t n = send(fd, p + *off, len - *off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                *again = 1;
                return 0;
            }
            *dead = 1;
            return -1;
        }
        if (n == 0) {
            *dead = 1;
            return -1;
        }
        *off += (uint32_t)n;
    }
    return 0;
}

/* 1: still need a client write. 0: finished. -1: client died. */
static int flush_100(http_conn *hc, proxy_conn *px) {
    if (!(px->flags & PX_SEND_100)) {
        return 0;
    }
    while (px->cont_off < CONT_100_LEN) {
        ssize_t n = send(hc->conn.fd, CONT_100 + px->cont_off,
                         (size_t)(CONT_100_LEN - px->cont_off), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        px->cont_off = (uint8_t)(px->cont_off + (uint8_t)n);
    }
    px->flags = (uint16_t)(px->flags & ~PX_SEND_100);
    return 0;
}

static int32_t yield_up(proxy_conn *px, int epfd, uint32_t idx, int32_t up_io,
                        int32_t client_io) {
    if (arm_up(px, epfd, idx, up_io) < 0) {
        return -1;
    }
    if (px->flags & PX_SEND_100) {
        return HTTP_IO_WANT_WRITE;
    }
    return client_io;
}

int32_t proxy_pump(http_conn *hc, proxy_conn *px, int epfd, uint32_t slot_idx,
                   uint64_t now_ms) {
    int budget = 16;

    if (px->fd < 0) {
        return HTTP_IO_CLOSE;
    }
    if (now_ms >= hc->deadline_ms) {
        STAT_INC(STAT_TIMEOUT);
        proxy_close(epfd, px);
        return HTTP_IO_CLOSE;
    }

    for (;;) {
        int again = 0;
        int dead = 0;
        int eof = 0;
        int fr;
        uint32_t before;

        fr = flush_100(hc, px);
        if (fr < 0) {
            proxy_close(epfd, px);
            return HTTP_IO_CLOSE;
        }
        if (fr > 0) {
            return HTTP_IO_WANT_WRITE;
        }

        if (budget <= 0) {
            if (px->phase == PX_SEND_REQ ||
                (px->phase == PX_SEND_BODY &&
                 (px->buf_off < px->buf_len || px->body_off < px->body_end))) {
                int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_WRITE, HTTP_IO_IDLE);
                return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
            }
            if (px->phase == PX_SEND_BODY && px->body_left > 0) {
                int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_IDLE, HTTP_IO_WANT_READ);
                return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
            }
            if (px->phase == PX_RECV_HDR ||
                (px->phase == PX_STREAM && px->buf_off >= px->buf_len &&
                 !(px->flags & PX_BODY_DONE))) {
                int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_READ, HTTP_IO_IDLE);
                return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
            }
            if (px->phase == PX_SEND_RESP || px->phase == PX_STREAM) {
                int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_IDLE, HTTP_IO_WANT_WRITE);
                return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
            }
            return proxy_fail(hc, px, epfd, now_ms);
        }

        switch (px->phase) {
        case PX_SEND_REQ:
            if (px->hdr_off < px->hdr_len) {
                before = px->hdr_off;
                if (sock_send(px->fd, px->hdr, &px->hdr_off, px->hdr_len, &again, &dead) < 0 ||
                    dead) {
                    /* Nothing from this request was written. The idle fd
                     * was already dead. Drop it and connect once. */
                    if ((px->flags & PX_REUSED) && px->hdr_off == 0) {
                        if (proxy_reopen(px, epfd) < 0) {
                            return proxy_fail(hc, px, epfd, now_ms);
                        }
                        break;
                    }
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                if (px->hdr_off != before) {
                    touch(hc, now_ms);
                    budget--;
                }
                if (again || px->hdr_off < px->hdr_len) {
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_WRITE, HTTP_IO_IDLE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
            }
            if (px->body_off < px->body_end || px->body_left > 0) {
                px->phase = PX_SEND_BODY;
            } else {
                cork_up(px, 0);
                px->buf_off = 0;
                px->buf_len = 0;
                px->phase = PX_RECV_HDR;
            }
            break;

        case PX_SEND_BODY:
            if (px->buf_off < px->buf_len) {
                before = px->buf_off;
                if (sock_send(px->fd, px->buf, &px->buf_off, px->buf_len, &again, &dead) < 0 ||
                    dead) {
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                if (px->buf_off != before) {
                    touch(hc, now_ms);
                    budget--;
                }
                if (again) {
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_WRITE, HTTP_IO_IDLE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                px->buf_off = 0;
                px->buf_len = 0;
                break;
            }
            if (px->body_off < px->body_end) {
                before = px->body_off;
                if (sock_send(px->fd, hc->buf, &px->body_off, px->body_end, &again, &dead) < 0 ||
                    dead) {
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                if (px->body_off != before) {
                    touch(hc, now_ms);
                    budget--;
                }
                if (again || px->body_off < px->body_end) {
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_WRITE, HTTP_IO_IDLE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                break;
            }
            if (px->body_left > 0) {
                uint32_t cap = PROXY_BUF_MAX;
                int rr;

                if (cap > px->body_left) {
                    cap = px->body_left;
                }
                px->buf_off = 0;
                px->buf_len = 0;
                for (;;) {
                    rr = sock_read(hc->conn.fd, px->buf, &px->buf_len, cap, &again, &eof);
                    if (rr > 0) {
                        continue;
                    }
                    if (rr < 0) {
                        proxy_close(epfd, px);
                        return HTTP_IO_CLOSE;
                    }
                    break;
                }
                if (px->buf_len == 0) {
                    if (eof) {
                        proxy_close(epfd, px);
                        return HTTP_IO_CLOSE;
                    }
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_IDLE, HTTP_IO_WANT_READ);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                px->body_left -= px->buf_len;
                touch(hc, now_ms);
                budget--;
                if (eof && px->body_left > 0) {
                    proxy_close(epfd, px);
                    return HTTP_IO_CLOSE;
                }
                break;
            }
            cork_up(px, 0);
            px->buf_off = 0;
            px->buf_len = 0;
            px->phase = PX_RECV_HDR;
            break;

        case PX_RECV_HDR: {
            uint32_t at = 0;
            uint32_t total;
            uint32_t prefix;
            int kind;

            /* Headers may already be buffered (a 1xx block sat in front
             * of them, or the previous read ended on the blank line). */
            if (!find_hdr_end(px->buf, px->buf_len, &at)) {
                if (px->buf_len >= PROXY_BUF_MAX) {
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                before = px->buf_len;
                {
                    int rr = sock_read(px->fd, px->buf, &px->buf_len, PROXY_BUF_MAX, &again, &eof);
                    if (rr > 0) {
                        break;
                    }
                    if (rr < 0) {
                        int retry = (px->buf_len == 0)
                                        ? proxy_retry_idle(hc, px, epfd)
                                        : 0;
                        if (retry > 0) {
                            break;
                        }
                        return proxy_fail(hc, px, epfd, now_ms);
                    }
                }
                if (px->buf_len == before) {
                    if (eof) {
                        int retry = (px->buf_len == 0)
                                        ? proxy_retry_idle(hc, px, epfd)
                                        : 0;
                        if (retry > 0) {
                            break;
                        }
                        return proxy_fail(hc, px, epfd, now_ms);
                    }
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_READ, HTTP_IO_IDLE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                touch(hc, now_ms);
                budget--;
                if (!find_hdr_end(px->buf, px->buf_len, &at)) {
                    if (eof || px->buf_len >= PROXY_BUF_MAX) {
                        return proxy_fail(hc, px, epfd, now_ms);
                    }
                    break;
                }
            }
            total = at + 4;
            kind = rewrite_response(hc, px, px->buf, total);
            if (kind < 0) {
                return proxy_fail(hc, px, epfd, now_ms);
            }
            prefix = px->buf_len - total;
            memmove(px->buf, px->buf + total, prefix);
            px->buf_len = prefix;
            px->buf_off = 0;
            if (kind == 1) {
                /* 1xx block dropped. Keep reading the real response. */
                break;
            }
            if (hc->req.method == HTTP_METHOD_HEAD || (px->flags & PX_BODY_DONE)) {
                if (prefix > 0) {
                    px->flags = (uint16_t)(px->flags | PX_OVERREAD);
                }
                px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
                px->buf_len = 0;
            } else if (prefix > 0 && note_body(px, prefix) < 0) {
                return proxy_fail(hc, px, epfd, now_ms);
            }
            px->phase = PX_SEND_RESP;
            break;
        }

        case PX_SEND_RESP:
            if (px->flags & PX_BODY_DONE) {
                uint32_t hdr_before = px->hdr_off;
                uint32_t buf_before = px->buf_off;

                if (send_gathered(hc, px, &again, &dead) < 0 || dead) {
                    if (px->hdr_off != hdr_before || px->buf_off != buf_before) {
                        px->flags = (uint16_t)(px->flags | PX_RESP_SENT);
                    }
                    if (px->flags & PX_RESP_SENT) {
                        proxy_close(epfd, px);
                        return HTTP_IO_CLOSE;
                    }
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                if (px->hdr_off != hdr_before || px->buf_off != buf_before) {
                    px->flags = (uint16_t)(px->flags | PX_RESP_SENT);
                    touch(hc, now_ms);
                    budget--;
                }
                if (px->hdr_off < px->hdr_len || px->buf_off < px->buf_len) {
                    if (again) {
                        return HTTP_IO_WANT_WRITE;
                    }
                    break;
                }
                return proxy_done(hc, px, epfd);
            }
            cork_client(hc, 1);
            if (px->hdr_off < px->hdr_len) {
                before = px->hdr_off;
                if (sock_send(hc->conn.fd, px->hdr, &px->hdr_off, px->hdr_len, &again, &dead) < 0 ||
                    dead) {
                    if (px->hdr_off > before) {
                        px->flags = (uint16_t)(px->flags | PX_RESP_SENT);
                    }
                    if (px->flags & PX_RESP_SENT) {
                        proxy_close(epfd, px);
                        return HTTP_IO_CLOSE;
                    }
                    return proxy_fail(hc, px, epfd, now_ms);
                }
                if (px->hdr_off != before) {
                    px->flags = (uint16_t)(px->flags | PX_RESP_SENT);
                    touch(hc, now_ms);
                    budget--;
                }
                if (again || px->hdr_off < px->hdr_len) {
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_IDLE, HTTP_IO_WANT_WRITE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
            }
            if ((px->flags & PX_BODY_DONE) && px->buf_off >= px->buf_len) {
                return proxy_done(hc, px, epfd);
            }
            px->phase = PX_STREAM;
            break;

        case PX_STREAM:
            if (px->buf_off < px->buf_len) {
                before = px->buf_off;
                if (sock_send(hc->conn.fd, px->buf, &px->buf_off, px->buf_len, &again, &dead) < 0 ||
                    dead) {
                    proxy_close(epfd, px);
                    return HTTP_IO_CLOSE;
                }
                if (px->buf_off != before) {
                    px->flags = (uint16_t)(px->flags | PX_RESP_SENT);
                    touch(hc, now_ms);
                    budget--;
                }
                if (again || px->buf_off < px->buf_len) {
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_IDLE, HTTP_IO_WANT_WRITE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                px->buf_off = 0;
                px->buf_len = 0;
                if (px->flags & PX_BODY_DONE) {
                    return proxy_done(hc, px, epfd);
                }
                break;
            }
            if (px->flags & PX_BODY_DONE) {
                return proxy_done(hc, px, epfd);
            }
            {
                uint32_t cap = PROXY_BUF_MAX;
                int rr;

                if ((px->flags & PX_RESP_CL) && px->resp_left < cap) {
                    cap = (uint32_t)px->resp_left;
                }
                if (cap == 0) {
                    px->flags = (uint16_t)(px->flags | PX_BODY_DONE);
                    return proxy_done(hc, px, epfd);
                }
                px->buf_off = 0;
                px->buf_len = 0;
                rr = sock_read(px->fd, px->buf, &px->buf_len, cap, &again, &eof);
                if (rr > 0) {
                    break;
                }
                if (rr < 0) {
                    proxy_close(epfd, px);
                    return HTTP_IO_CLOSE;
                }
                if (px->buf_len == 0) {
                    if (eof && (px->flags & PX_RESP_EOF)) {
                        return proxy_done(hc, px, epfd);
                    }
                    if (eof) {
                        proxy_close(epfd, px);
                        return HTTP_IO_CLOSE;
                    }
                    int32_t y = yield_up(px, epfd, slot_idx, HTTP_IO_WANT_READ, HTTP_IO_IDLE);
                    return y < 0 ? proxy_fail(hc, px, epfd, now_ms) : y;
                }
                if (note_body(px, px->buf_len) < 0) {
                    proxy_close(epfd, px);
                    return HTTP_IO_CLOSE;
                }
                touch(hc, now_ms);
                budget--;
            }
            break;

        default:
            return proxy_fail(hc, px, epfd, now_ms);
        }
    }
}

int32_t proxy_begin(http_conn *hc, proxy_conn *px, const tiny_route *route,
                    int epfd, uint32_t slot_idx, uint64_t now_ms) {
    uint32_t hdrs;
    uint32_t need = 0;
    uint32_t have = 0;
    int fd;
    int rc;

    if (hc->req.method != HTTP_METHOD_GET &&
        hc->req.method != HTTP_METHOD_HEAD &&
        hc->req.method != HTTP_METHOD_POST) {
        return arm_fixed(hc, RESP_405, 405, now_ms);
    }
    if (hc->req.method == HTTP_METHOD_POST &&
        !(hc->req.flags & HTTP_FLAG_CONTENT_LENGTH)) {
        return arm_fixed(hc, RESP_411, 411, now_ms);
    }

    proxy_conn_reset(px);
    px->up_addr = route->upstream;
    px->up_len = route->upstream_len;
    if (expects_continue(hc)) {
        px->flags = (uint16_t)(px->flags | PX_SEND_100);
    }

    hdrs = hc->req.header_bytes;
    if (hc->req.flags & HTTP_FLAG_CONTENT_LENGTH) {
        need = hc->req.content_length;
        if (hc->used > hdrs) {
            have = hc->used - hdrs;
            if (have > need) {
                have = need;
            }
        }
    }
    px->body_off = hdrs;
    px->body_end = hdrs + have;
    px->body_left = need - have;
    if (rewrite_request(hc, px) < 0) {
        return arm_fixed(hc, RESP_502, 502, now_ms);
    }

    fd = proxy_pool_take((struct sockaddr *)&route->upstream, route->upstream_len);
    if (fd >= 0) {
        px->flags = (uint16_t)(px->flags | PX_REUSED);
    } else {
        fd = socket(route->upstream.ss_family,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return arm_fixed(hc, RESP_502, 502, now_ms);
        }
    }
    px->fd = fd;
    px->phase = PX_SEND_REQ;
    if (px->body_off < px->body_end || px->body_left > 0) {
        cork_up(px, 1);
    }
    if (!(px->flags & PX_REUSED)) {
        rc = connect(fd, (struct sockaddr *)&route->upstream, route->upstream_len);
        if (rc < 0 && errno != EINPROGRESS) {
            return proxy_fail(hc, px, epfd, now_ms);
        }
    }
    hc->keep = 0;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
    return proxy_pump(hc, px, epfd, slot_idx, now_ms);
}
