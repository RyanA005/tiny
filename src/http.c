#include "http.h"

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

static const char RESPONSE_200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

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

#define SEND_STATIC(fd, r) write_all((fd), (r), (uint32_t)(sizeof(r) - 1))

static int32_t write_all(int32_t fd, const void *buf, uint32_t len) {
    const uint8_t *p = buf;
    uint32_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (uint32_t)n;
    }
    return 0;
}

void do_http(connection *c, bump *b) {
    http_request req;
    uint32_t used = 0;
    char *buf;

    buf = bump_alloc(b, HTTP_HEADER_BUF_SIZE, 1);
    if (!buf) {
        return;
    }

    while (used < HTTP_HEADER_BUF_SIZE) {
        ssize_t n = read(c->fd, buf + used, HTTP_HEADER_BUF_SIZE - used);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        used += (uint32_t)n;

        int32_t result = http_parse_request(buf, used, &req);

        if (result == HTTP_PARSE_INCOMPLETE) {
            continue;
        }
        if (result == HTTP_PARSE_BAD) {
            SEND_STATIC(c->fd, RESPONSE_400);
            return;
        }
        if (result == HTTP_PARSE_UNSUPPORTED) {
            SEND_STATIC(c->fd, RESPONSE_501);
            return;
        }

        uint32_t body_in_buffer = used - req.header_bytes;
        (void)body_in_buffer;

        SEND_STATIC(c->fd, RESPONSE_200);
        return;
    }

    SEND_STATIC(c->fd, RESPONSE_431);
}
