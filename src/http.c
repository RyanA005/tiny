#include "http.h"
#include "static.h"
#include "stats.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* Protocol / hard failures: always close. */
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

uint8_t http_should_keepalive(const http_request *req) {
    if (req->method != HTTP_METHOD_GET && req->method != HTTP_METHOD_HEAD) {
        return 0;
    }
    if (req->flags & HTTP_FLAG_CONNECTION_CLOSE) {
        return 0;
    }
    if (req->flags & HTTP_FLAG_TRANSFER_ENCODING) {
        return 0;
    }
    /* Do not keep if a request body is present; we do not drain bodies yet. */
    if ((req->flags & HTTP_FLAG_CONTENT_LENGTH) && req->content_length > 0) {
        return 0;
    }
    if (req->minor_version >= 1) {
        return 1; /* HTTP/1.1 default */
    }
    return (req->flags & HTTP_FLAG_CONNECTION_KEEP_ALIVE) ? 1 : 0;
}

static void arm_fixed_close(http_conn *hc, const char *resp, uint32_t len,
                            uint64_t now_ms) {
    hc->fixed = resp;
    hc->fixed_len = len;
    hc->fixed_off = 0;
    hc->phase = HTTP_PHASE_WRITE_FIXED;
    hc->deadline_ms = now_ms + HTTP_SEND_DEADLINE_MS;
    hc->keep = 0;
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

static int32_t dispatch_parsed(http_conn *hc, int32_t result, uint64_t now_ms) {
    if (result == HTTP_PARSE_BAD) {
        arm_fixed_close(hc, RESPONSE_400, (uint32_t)(sizeof(RESPONSE_400) - 1), now_ms);
        return http_conn_on_write(hc, now_ms);
    }
    if (result == HTTP_PARSE_UNSUPPORTED) {
        arm_fixed_close(hc, RESPONSE_501, (uint32_t)(sizeof(RESPONSE_501) - 1), now_ms);
        return http_conn_on_write(hc, now_ms);
    }

    /* result >= 0 means header_bytes; request fully parsed. */
    int32_t rc = static_begin(hc, now_ms);
    if (rc == HTTP_IO_WANT_WRITE) {
        return http_conn_on_write(hc, now_ms);
    }
    return rc;
}

/*
 * Parse whatever is already in hc->buf. Returns:
 *   WANT_READ if incomplete and room remains
 *   or a write/done/close result after dispatching a complete request / error.
 */
static int32_t try_parse_buffer(http_conn *hc, uint64_t now_ms) {
    if (hc->used == 0) {
        return HTTP_IO_WANT_READ;
    }

    int32_t result = http_parse_request(hc->buf, hc->used, &hc->req);
    if (result == HTTP_PARSE_INCOMPLETE) {
        if (hc->used >= HTTP_HEADER_BUF_SIZE) {
            arm_fixed_close(hc, RESPONSE_431, (uint32_t)(sizeof(RESPONSE_431) - 1), now_ms);
            return http_conn_on_write(hc, now_ms);
        }
        return HTTP_IO_WANT_READ;
    }
    return dispatch_parsed(hc, result, now_ms);
}

static void reset_request_state(http_conn *hc, uint64_t now_ms, uint64_t idle_ms) {
    memset(&hc->req, 0, sizeof(hc->req));
    hc->phase = HTTP_PHASE_READ;
    hc->deadline_ms = now_ms + idle_ms;
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
    hc->keep = 0;
#ifdef TINY_STATS
    hc->t_start_ns = stats_now_ns();
#else
    hc->t_start_ns = 0;
#endif
}

int32_t http_conn_prepare(http_conn *hc, bump *arena, uint64_t now_ms) {
    hc->arena = arena;
    hc->buf = hc->header_buf;
    hc->used = 0;
    reset_request_state(hc, now_ms, HTTP_READ_DEADLINE_MS);
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

int32_t http_conn_reuse(http_conn *hc, bump *arena, uint64_t now_ms) {
    if (!hc->keep) {
        return HTTP_IO_CLOSE;
    }

    /* Request body is not kept; consumed length is headers only. */
    uint32_t consumed = hc->req.header_bytes;
    if (consumed > hc->used) {
        return HTTP_IO_CLOSE;
    }

    uint32_t left = hc->used - consumed;
    if (left > HTTP_HEADER_BUF_SIZE) {
        return HTTP_IO_CLOSE;
    }

    http_conn_cleanup(hc);

    /* In-place leftover; header_buf survives arena reset. */
    if (left > 0 && consumed > 0) {
        memmove(hc->header_buf, hc->header_buf + consumed, left);
    }
    hc->used = left;
    hc->buf = hc->header_buf;

    bump_reset(arena);
    hc->arena = arena;
    reset_request_state(hc, now_ms, HTTP_KEEPALIVE_IDLE_MS);

    /* Pipelined bytes may already form a complete next request. */
    STAT_TIME_BEGIN(read);
    int32_t rc = try_parse_buffer(hc, now_ms);
    STAT_TIME_END(STAT_NS_READ, read);
    return rc;
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

    /* Prefer parsing existing bytes (reuse / partial read) before recv. */
    if (hc->used > 0) {
        int32_t rc = try_parse_buffer(hc, now_ms);
        if (rc != HTTP_IO_WANT_READ) {
            STAT_TIME_END(STAT_NS_READ, read);
            return rc;
        }
    }

    while (hc->used < HTTP_HEADER_BUF_SIZE) {
        ssize_t n = read(hc->conn.fd, hc->buf + hc->used,
                         HTTP_HEADER_BUF_SIZE - hc->used);
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
            /* Idle peer closed between keep-alive requests. */
            return HTTP_IO_CLOSE;
        }

        hc->used += (uint32_t)n;
        hc->last_active_ms = now_ms;

        int32_t rc = try_parse_buffer(hc, now_ms);
        if (rc != HTTP_IO_WANT_READ) {
            STAT_TIME_END(STAT_NS_READ, read);
            return rc;
        }
    }

    STAT_TIME_END(STAT_NS_READ, read);
    arm_fixed_close(hc, RESPONSE_431, (uint32_t)(sizeof(RESPONSE_431) - 1), now_ms);
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
