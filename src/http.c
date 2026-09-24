#include "http.h"
#include "static.h"
#include "stats.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/socket.h>

static const char RESPONSE_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_431[] =
    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_501[] =
    "HTTP/1.1 501 Not Implemented\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static void arm_fixed(http_conn *hc, const char *resp, uint32_t len, uint64_t now_ms) {
    hc->fixed = resp;
    hc->fixed_len = len;
    hc->fixed_off = 0;
    hc->phase = HTTP_PHASE_WRITE_FIXED;
    hc->deadline_ms = now_ms + HTTP_SEND_DEADLINE_MS;
}

static int32_t write_fixed(http_conn *hc) {
    while (hc->fixed_off < hc->fixed_len) {
        ssize_t n = send(hc->conn.fd, hc->fixed + hc->fixed_off,
                         hc->fixed_len - hc->fixed_off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return HTTP_IO_WANT_WRITE;
            }
            return HTTP_IO_CLOSE;
        }
        if (n == 0) {
            return HTTP_IO_CLOSE;
        }
        hc->fixed_off += (uint32_t)n;
    }
    return HTTP_IO_DONE;
}

int32_t http_conn_prepare(http_conn *hc, bump *arena, uint64_t now_ms) {
    hc->arena = arena;
    hc->buf = bump_alloc(arena, HTTP_HEADER_BUF_SIZE, 1);
    if (!hc->buf) {
        return -1;
    }
    hc->used = 0;
    hc->phase = HTTP_PHASE_READ;
    hc->deadline_ms = now_ms + HTTP_READ_DEADLINE_MS;
    hc->last_active_ms = now_ms;
    hc->fixed = 0;
    hc->fixed_len = 0;
    hc->fixed_off = 0;
    hc->out_hdr = 0;
    hc->out_hdr_len = 0;
    hc->out_hdr_off = 0;
    hc->file_fd = -1;
    hc->file_size = 0;
    hc->file_off = 0;
    hc->send_body = 0;
    hc->cork_on = 0;
#ifdef TINY_STATS
    hc->t_start_ns = stats_now_ns();
#else
    hc->t_start_ns = 0;
#endif
    return 0;
}

void http_conn_cleanup(http_conn *hc) {
    if (hc->file_fd >= 0) {
        close(hc->file_fd);
        hc->file_fd = -1;
    }
    if (hc->cork_on && hc->conn.fd >= 0) {
        int32_t on = 0;
        setsockopt(hc->conn.fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
        hc->cork_on = 0;
    }
}

int32_t http_conn_check_deadline(http_conn *hc, uint64_t now_ms) {
    if (now_ms >= hc->deadline_ms) {
        return HTTP_IO_CLOSE;
    }
    return HTTP_IO_WANT_READ; /* placeholder; caller ignores */
}

int32_t http_conn_on_read(http_conn *hc, uint64_t now_ms) {
    if (hc->phase != HTTP_PHASE_READ) {
        return HTTP_IO_WANT_WRITE;
    }
    if (now_ms >= hc->deadline_ms) {
        STAT_INC(STAT_TIMEOUT);
        return HTTP_IO_CLOSE;
    }

    STAT_TIME_BEGIN(read);
    while (hc->used < HTTP_HEADER_BUF_SIZE) {
        ssize_t n = read(hc->conn.fd, hc->buf + hc->used, HTTP_HEADER_BUF_SIZE - hc->used);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                STAT_INC(STAT_EAGAIN_READ);
                STAT_TIME_END(STAT_NS_READ, read);
                return HTTP_IO_WANT_READ;
            }
            STAT_TIME_END(STAT_NS_READ, read);
            return HTTP_IO_CLOSE;
        }
        if (n == 0) {
            STAT_TIME_END(STAT_NS_READ, read);
            return HTTP_IO_CLOSE;
        }

        hc->used += (uint32_t)n;
        hc->last_active_ms = now_ms;

        int32_t result = http_parse_request(hc->buf, hc->used, &hc->req);
        if (result == HTTP_PARSE_INCOMPLETE) {
            continue;
        }
        STAT_TIME_END(STAT_NS_READ, read);

        if (result == HTTP_PARSE_BAD) {
            arm_fixed(hc, RESPONSE_400, (uint32_t)(sizeof(RESPONSE_400) - 1), now_ms);
            return http_conn_on_write(hc, now_ms);
        }
        if (result == HTTP_PARSE_UNSUPPORTED) {
            arm_fixed(hc, RESPONSE_501, (uint32_t)(sizeof(RESPONSE_501) - 1), now_ms);
            return http_conn_on_write(hc, now_ms);
        }

        int32_t rc = static_begin(hc, now_ms);
        if (rc == HTTP_IO_WANT_WRITE) {
            return http_conn_on_write(hc, now_ms);
        }
        return rc;
    }

    STAT_TIME_END(STAT_NS_READ, read);
    arm_fixed(hc, RESPONSE_431, (uint32_t)(sizeof(RESPONSE_431) - 1), now_ms);
    return http_conn_on_write(hc, now_ms);
}

int32_t http_conn_on_write(http_conn *hc, uint64_t now_ms) {
    if (now_ms >= hc->deadline_ms) {
        STAT_INC(STAT_TIMEOUT);
        return HTTP_IO_CLOSE;
    }
    hc->last_active_ms = now_ms;

    if (hc->phase == HTTP_PHASE_WRITE_FIXED) {
        int32_t rc = write_fixed(hc);
        if (rc == HTTP_IO_WANT_WRITE) {
            STAT_INC(STAT_EAGAIN_WRITE);
        } else if (rc == HTTP_IO_DONE) {
            STAT_INC(STAT_REQ_OK);
#ifdef TINY_STATS
            if (hc->t_start_ns) {
                STAT_ADD(STAT_NS_TOTAL, stats_now_ns() - hc->t_start_ns);
            }
#endif
        }
        return rc;
    }
    if (hc->phase == HTTP_PHASE_WRITE_HDR || hc->phase == HTTP_PHASE_SENDFILE) {
        return static_on_write(hc, now_ms);
    }
    return HTTP_IO_WANT_READ;
}
