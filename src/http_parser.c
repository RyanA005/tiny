#include "http.h"

#include <string.h>

#define HTTP_PARSE_MAX_LEN 0xFFFFu
#define SEEN_HOST           0x01
#define SEEN_CONTENT_LENGTH 0x02

static inline uint8_t ascii_lower(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c + ('a' - 'A'));
    }
    return c;
}

static uint8_t ascii_ieq(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (ascii_lower((uint8_t)a[i]) != ascii_lower((uint8_t)b[i])) {
            return 0;
        }
    }
    return 1;
}

static uint8_t is_tchar(uint8_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static inline uint8_t is_field_vchar(uint8_t c) {
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

static inline uint8_t is_target_char(uint8_t c) {
    return c > 0x20 && c < 0x7F;
}

static inline uint8_t is_ows(uint8_t c) {
    return c == ' ' || c == '\t';
}

static int32_t find_crlf(const char *p, const char *end, const char **eol) {
    const char *cr = memchr(p, '\r', (size_t)(end - p));

    if (!cr) {
        if (memchr(p, '\n', (size_t)(end - p))) {
            return HTTP_PARSE_BAD;
        }
        return HTTP_PARSE_INCOMPLETE;
    }
    if (cr + 1 == end) {
        return HTTP_PARSE_INCOMPLETE;
    }
    if (cr[1] != '\n') {
        return HTTP_PARSE_BAD;
    }
    *eol = cr;
    return 0;
}

static uint8_t parse_method(const char *p, uint32_t len) {
    if (len == 3) {
        if (memcmp(p, "GET", 3) == 0) return HTTP_METHOD_GET;
        if (memcmp(p, "PUT", 3) == 0) return HTTP_METHOD_PUT;
    } else if (len == 4) {
        if (memcmp(p, "POST", 4) == 0) return HTTP_METHOD_POST;
        if (memcmp(p, "HEAD", 4) == 0) return HTTP_METHOD_HEAD;
    } else if (len == 6) {
        if (memcmp(p, "DELETE", 6) == 0) return HTTP_METHOD_DELETE;
    }
    return HTTP_METHOD_UNKNOWN;
}

static int32_t parse_content_length(const char *p, uint32_t len, uint32_t *out) {
    uint32_t v = 0;

    if (len == 0) {
        return HTTP_PARSE_BAD;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c < '0' || c > '9') {
            return HTTP_PARSE_BAD;
        }
        uint32_t d = c - '0';
        if (v > (UINT32_MAX - d) / 10) {
            return HTTP_PARSE_BAD;
        }
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

static void parse_connection(const char *p, const char *end, uint8_t *flags) {
    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *tok_end = comma ? comma : end;
        const char *tok = p;

        while (tok < tok_end && is_ows((uint8_t)*tok)) tok++;
        while (tok_end > tok && is_ows((uint8_t)tok_end[-1])) tok_end--;

        uint32_t n = (uint32_t)(tok_end - tok);
        if (n == 5 && ascii_ieq(tok, "close", 5)) {
            *flags |= HTTP_FLAG_CONNECTION_CLOSE;
        } else if (n == 10 && ascii_ieq(tok, "keep-alive", 10)) {
            *flags |= HTTP_FLAG_CONNECTION_KEEP_ALIVE;
        }

        if (!comma) break;
        p = comma + 1;
    }
}

static int32_t parse_request_target(const char *buf, const char *target,
                                    const char *sp2, http_request *req) {
    const char *path_start = 0;
    const char *end = sp2;

    if (target[0] == '/') {
        path_start = target; /* origin-form */
    } else if ((end - target) >= 7 && ascii_ieq(target, "http://", 7)) {
        const char *auth = target + 7;
        const char *slash = memchr(auth, '/', (size_t)(end - auth));
        if (!slash) {
            /* http://host  -> path "/" */
            path_start = 0;
        } else {
            path_start = slash;
        }
    } else if ((end - target) >= 8 && ascii_ieq(target, "https://", 8)) {
        const char *auth = target + 8;
        const char *slash = memchr(auth, '/', (size_t)(end - auth));
        if (!slash) {
            path_start = 0;
        } else {
            path_start = slash;
        }
    } else {
        return HTTP_PARSE_BAD;
    }

    const char *path;
    const char *path_end;
    if (!path_start) {
        /* Absolute-form with no path: equivalent to "/". */
        req->target.off = (uint16_t)(target - buf);
        req->target.len = (uint16_t)(end - target);
        req->path.off = 0;
        req->path.len = 0; /* sentinel: treat as "/" in static_begin */
        return 0;
    }

    path = path_start;
    path_end = end;

    const char *qmark = 0;
    for (const char *q = path; q < path_end; q++) {
        uint8_t c = (uint8_t)*q;
        if (!is_target_char(c)) {
            return HTTP_PARSE_BAD;
        }
        if (c == '?' && !qmark) {
            qmark = q;
        }
    }

    req->target.off = (uint16_t)(target - buf);
    req->target.len = (uint16_t)(end - target);
    req->path.off = (uint16_t)(path - buf);
    if (qmark) {
        req->path.len = (uint16_t)(qmark - path);
        req->query.off = (uint16_t)(qmark + 1 - buf);
        req->query.len = (uint16_t)(path_end - (qmark + 1));
    } else {
        req->path.len = (uint16_t)(path_end - path);
    }
    return 0;
}

static int32_t parse_request_line(const char *buf, const char *line,
                                  const char *eol, http_request *req) {
    const char *sp1 = memchr(line, ' ', (size_t)(eol - line));
    if (!sp1 || sp1 == line) {
        return HTTP_PARSE_BAD;
    }
    for (const char *q = line; q < sp1; q++) {
        if (!is_tchar((uint8_t)*q)) {
            return HTTP_PARSE_BAD;
        }
    }
    req->method = parse_method(line, (uint32_t)(sp1 - line));

    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(eol - target));
    if (!sp2 || sp2 == target) {
        return HTTP_PARSE_BAD;
    }

    int32_t trc = parse_request_target(buf, target, sp2, req);
    if (trc) {
        return trc;
    }

    const char *ver = sp2 + 1;
    if (eol - ver != 8 || memcmp(ver, "HTTP/1.", 7) != 0) {
        return HTTP_PARSE_BAD;
    }
    if (ver[7] == '1') {
        req->minor_version = 1;
    } else if (ver[7] == '0') {
        req->minor_version = 0;
    } else {
        return HTTP_PARSE_BAD;
    }
    return 0;
}

/* RFC 9110 Host: uri-host [ ":" port ]. Reject empty / CTL / spaces. */
static int32_t validate_host(const char *p, uint32_t len) {
    if (len == 0) {
        return HTTP_PARSE_BAD;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c < 0x21 || c > 0x7E) {
            return HTTP_PARSE_BAD;
        }
    }
    /* IPv6 literal: [addr] or [addr]:port */
    if (p[0] == '[') {
        const char *rb = memchr(p, ']', len);
        if (!rb || rb == p + 1) {
            return HTTP_PARSE_BAD;
        }
        uint32_t after = (uint32_t)(p + len - (rb + 1));
        if (after == 0) {
            return 0;
        }
        if (rb[1] != ':' || after < 2) {
            return HTTP_PARSE_BAD;
        }
        for (const char *q = rb + 2; q < p + len; q++) {
            if (*q < '0' || *q > '9') {
                return HTTP_PARSE_BAD;
            }
        }
        return 0;
    }
    /* hostname / IPv4 with optional :port — digits-only port if colon present */
    const char *colon = memchr(p, ':', len);
    uint32_t host_len = colon ? (uint32_t)(colon - p) : len;
    if (host_len == 0) {
        return HTTP_PARSE_BAD;
    }
    for (uint32_t i = 0; i < host_len; i++) {
        uint8_t c = (uint8_t)p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-') {
            continue;
        }
        return HTTP_PARSE_BAD;
    }
    if (colon) {
        if (colon + 1 >= p + len) {
            return HTTP_PARSE_BAD;
        }
        for (const char *q = colon + 1; q < p + len; q++) {
            if (*q < '0' || *q > '9') {
                return HTTP_PARSE_BAD;
            }
        }
    }
    return 0;
}

static int32_t handle_header(const char *buf, const char *name, uint32_t name_len, const char *val, const char *val_end, http_request *req, uint8_t *seen) {
    uint32_t val_len = (uint32_t)(val_end - val);

    if (name_len == 4 && ascii_ieq(name, "host", 4)) {
        if (*seen & SEEN_HOST) {
            return HTTP_PARSE_BAD;
        }
        *seen |= SEEN_HOST;
        if (validate_host(val, val_len) != 0) {
            return HTTP_PARSE_BAD;
        }
        req->host.off = (uint16_t)(val - buf);
        req->host.len = (uint16_t)val_len;
    } else if (name_len == 14 && ascii_ieq(name, "content-length", 14)) {
        if (*seen & SEEN_CONTENT_LENGTH) {
            return HTTP_PARSE_BAD;
        }
        *seen |= SEEN_CONTENT_LENGTH;
        int32_t rc = parse_content_length(val, val_len, &req->content_length);
        if (rc) {
            return rc;
        }
        req->flags |= HTTP_FLAG_CONTENT_LENGTH;
    } else if (name_len == 17 && ascii_ieq(name, "transfer-encoding", 17)) {
        req->flags |= HTTP_FLAG_TRANSFER_ENCODING;
    } else if (name_len == 10 && ascii_ieq(name, "connection", 10)) {
        parse_connection(val, val_end, &req->flags);
    }
    return 0;
}

static int32_t parse(const char *buf, uint32_t len, http_request *req) {
    const char *end = buf + len;
    const char *p = buf;
    const char *eol;
    uint8_t seen = 0;
    int32_t rc;

    rc = find_crlf(p, end, &eol);
    if (rc) {
        return rc;
    }
    rc = parse_request_line(buf, p, eol, req);
    if (rc) {
        return rc;
    }
    p = eol + 2;

    req->header_block.off = (uint16_t)(p - buf);

    while (1) {
        rc = find_crlf(p, end, &eol);
        if (rc) {
            return rc;
        }
        if (eol == p) {
            break;
        }

        if (is_ows((uint8_t)*p)) {
            return HTTP_PARSE_BAD;
        }

        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon || colon == p) {
            return HTTP_PARSE_BAD;
        }
        for (const char *q = p; q < colon; q++) {
            if (!is_tchar((uint8_t)*q)) {
                return HTTP_PARSE_BAD;
            }
        }

        const char *val = colon + 1;
        const char *val_end = eol;
        while (val < val_end && is_ows((uint8_t)*val)) val++;
        while (val_end > val && is_ows((uint8_t)val_end[-1])) val_end--;
        for (const char *q = val; q < val_end; q++) {
            if (!is_field_vchar((uint8_t)*q)) {
                return HTTP_PARSE_BAD;
            }
        }

        rc = handle_header(buf, p, (uint32_t)(colon - p), val, val_end, req, &seen);
        if (rc) {
            return rc;
        }
        p = eol + 2;
    }

    req->header_block.len = (uint16_t)(p - (buf + req->header_block.off));
    req->header_bytes = (uint16_t)((eol + 2) - buf);

    if ((req->flags & HTTP_FLAG_TRANSFER_ENCODING) &&
        (req->flags & HTTP_FLAG_CONTENT_LENGTH)) {
        return HTTP_PARSE_BAD;
    }
    if (req->minor_version == 1 && !(seen & SEEN_HOST)) {
        return HTTP_PARSE_BAD;
    }
    if (req->flags & HTTP_FLAG_TRANSFER_ENCODING) {
        return HTTP_PARSE_UNSUPPORTED;
    }
    return (int32_t)req->header_bytes;
}

int32_t http_parse_request(const char *buf, uint32_t len, http_request *req) {
    memset(req, 0, sizeof(*req));

    if (len == 0) {
        return HTTP_PARSE_INCOMPLETE;
    }

    if (len > HTTP_PARSE_MAX_LEN) {
        int32_t rc = parse(buf, HTTP_PARSE_MAX_LEN, req);
        return rc == HTTP_PARSE_INCOMPLETE ? HTTP_PARSE_BAD : rc;
    }
    return parse(buf, len, req);
}

uint8_t http_get_header(const char *buf, const http_request *req,
                        const char *name, uint16_t name_len,
                        http_slice *value) {
    const char *p = buf + req->header_block.off;
    const char *end = p + req->header_block.len;

    while (p < end) {
        const char *cr = memchr(p, '\r', (size_t)(end - p));
        if (!cr) {
            return 0;
        }
        const char *colon = memchr(p, ':', (size_t)(cr - p));
        if (!colon) {
            return 0;
        }
        if ((uint32_t)(colon - p) == name_len && ascii_ieq(p, name, name_len)) {
            const char *val = colon + 1;
            const char *val_end = cr;
            while (val < val_end && is_ows((uint8_t)*val)) val++;
            while (val_end > val && is_ows((uint8_t)val_end[-1])) val_end--;
            value->off = (uint16_t)(val - buf);
            value->len = (uint16_t)(val_end - val);
            return 1;
        }
        p = cr + 2;
    }
    return 0;
}
