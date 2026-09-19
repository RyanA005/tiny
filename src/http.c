#include "http.h"

void do_http(connection *c, bump *b) {
    char *buf;
    int32_t n;

    buf = bump_alloc(b, REQUEST_BUF_SIZE, 1);
    if (!buf) return;

    n = read(c->fd, buf, REQUEST_BUF_SIZE - 1);
    if (n <= 0) return;

    buf[n] = '\0';
}

