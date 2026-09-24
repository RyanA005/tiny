#define _GNU_SOURCE

#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "logger.h"

#define STATIC_HDR_SIZE 256
#define STATIC_PATH_MAX 4096

static int32_t docroot_fd = -1;

static const char RESPONSE_403[] =
    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_405[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "Allow: GET, HEAD\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

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

#define SEND_STATIC(fd, r) write_all((fd), (r), (uint32_t)(sizeof(r) - 1))

static const char *mime_from_path(const char *path, uint32_t len) {
    const char *dot = 0;

    for (uint32_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            dot = 0;
        } else if (path[i] == '.') {
            dot = path + i;
        }
    }
    if (!dot || dot + 1 >= path + len) {
        return "application/octet-stream";
    }

    const char *ext = dot + 1;
    uint32_t elen = (uint32_t)(path + len - ext);

    if (elen == 4 && memcmp(ext, "html", 4) == 0) return "text/html; charset=utf-8";
    if (elen == 3 && memcmp(ext, "htm", 3) == 0)  return "text/html; charset=utf-8";
    if (elen == 3 && memcmp(ext, "css", 3) == 0)  return "text/css; charset=utf-8";
    if (elen == 2 && memcmp(ext, "js", 2) == 0)   return "text/javascript; charset=utf-8";
    if (elen == 4 && memcmp(ext, "json", 4) == 0) return "application/json";
    if (elen == 3 && memcmp(ext, "png", 3) == 0)  return "image/png";
    if (elen == 3 && memcmp(ext, "jpg", 3) == 0)  return "image/jpeg";
    if (elen == 4 && memcmp(ext, "jpeg", 4) == 0) return "image/jpeg";
    if (elen == 3 && memcmp(ext, "gif", 3) == 0)  return "image/gif";
    if (elen == 3 && memcmp(ext, "svg", 3) == 0)  return "image/svg+xml";
    if (elen == 3 && memcmp(ext, "ico", 3) == 0)  return "image/x-icon";
    if (elen == 3 && memcmp(ext, "txt", 3) == 0)  return "text/plain; charset=utf-8";
    if (elen == 4 && memcmp(ext, "wasm", 4) == 0) return "application/wasm";
    return "application/octet-stream";
}

static uint32_t normalize_path(const char *in, uint16_t in_len, char *out, uint32_t out_cap) {
    if (in_len == 0 || in[0] != '/') {
        return 0;
    }

    uint32_t o = 0;
    uint32_t i = 1; // skip leading /

    while (i < in_len) {
        while (i < in_len && in[i] == '/') {
            i++;
        }
        if (i >= in_len) {
            break;
        }

        uint32_t seg_start = i;
        while (i < in_len && in[i] != '/') {
            if (in[i] == '\\' || in[i] == '\0') {
                return 0;
            }
            i++;
        }
        uint32_t seg_len = i - seg_start;

        if (seg_len == 1 && in[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && in[seg_start] == '.' && in[seg_start + 1] == '.') {
            return 0;
        }

        if (o > 0) {
            if (o + 1 >= out_cap) {
                return 0;
            }
            out[o++] = '/';
        }
        if (o + seg_len >= out_cap) {
            return 0;
        }
        memcpy(out + o, in + seg_start, seg_len);
        o += seg_len;
    }

    if (o == 0 || in[in_len - 1] == '/') {
        const char *idx = (o == 0) ? "index.html" : "/index.html";
        uint32_t ilen = (o == 0) ? 10 : 11;
        if (o + ilen >= out_cap) {
            return 0;
        }
        memcpy(out + o, idx, ilen);
        o += ilen;
    }

    if (o >= out_cap) {
        return 0;
    }
    out[o] = '\0';
    return o;
}

static int32_t open_under_docroot(const char *rel) {
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(O_RDONLY | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;

    return (int32_t)syscall(SYS_openat2, docroot_fd, rel, &how, sizeof(how));
}

static int32_t sendfile_all(int32_t out_fd, int32_t in_fd, off_t size) {
    off_t offset = 0;

    while (offset < size) {
        size_t want = (size_t)(size - offset);
        ssize_t n = sendfile(out_fd, in_fd, &offset, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // kernel can reject large count
            if (errno == EINVAL && want > (1u << 20)) {
                n = sendfile(out_fd, in_fd, &offset, 1u << 20);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return -1;
                }
                if (n == 0) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
    }
    return 0;
}

static void cork(int32_t fd, int32_t on) {
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
}

int32_t static_init(const char *docroot) {
    if (!docroot || !docroot[0]) {
        return -1;
    }

    int32_t fd = open(docroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        tiny_log(ERROR, "[STATIC] open docroot '%s' failed: %s\n", docroot, strerror(errno));
        return -1;
    }

    docroot_fd = fd;
    tiny_log(INFO, "[STATIC] docroot '%s' (fd=%d)\n", docroot, fd);
    return 0;
}

void static_shutdown(void) {
    if (docroot_fd >= 0) {
        close(docroot_fd);
        docroot_fd = -1;
    }
}

void static_serve(connection *c, bump *b, const char *req_buf, const http_request *req) {
    if (req->method != HTTP_METHOD_GET && req->method != HTTP_METHOD_HEAD) {
        SEND_STATIC(c->fd, RESPONSE_405);
        return;
    }

    if (docroot_fd < 0) {
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    char *rel = bump_alloc(b, STATIC_PATH_MAX, 1);
    if (!rel) {
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    uint32_t rel_len = normalize_path(req_buf + req->path.off, req->path.len, rel, STATIC_PATH_MAX);
    if (rel_len == 0) {
        SEND_STATIC(c->fd, RESPONSE_404);
        return;
    }

    int32_t file_fd = open_under_docroot(rel);
    if (file_fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            SEND_STATIC(c->fd, RESPONSE_404);
        } else if (errno == EACCES || errno == EPERM || errno == ELOOP) {
            SEND_STATIC(c->fd, RESPONSE_403);
        } else {
            tiny_log(WARNING, "[STATIC] open '%s' failed: %s\n", rel, strerror(errno));
            SEND_STATIC(c->fd, RESPONSE_500);
        }
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    // /foo where foo is a directory: try foo/index.html once
    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        if (rel_len + 11 >= STATIC_PATH_MAX) {
            SEND_STATIC(c->fd, RESPONSE_404);
            return;
        }
        memcpy(rel + rel_len, "/index.html", 11);
        rel_len += 11;
        rel[rel_len] = '\0';

        file_fd = open_under_docroot(rel);
        if (file_fd < 0) {
            SEND_STATIC(c->fd, RESPONSE_404);
            return;
        }
        if (fstat(file_fd, &st) < 0) {
            close(file_fd);
            SEND_STATIC(c->fd, RESPONSE_500);
            return;
        }
    }

    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        SEND_STATIC(c->fd, RESPONSE_403);
        return;
    }

    if (st.st_size < 0) {
        close(file_fd);
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    const char *mime = mime_from_path(rel, rel_len);
    char *hdr = bump_alloc(b, STATIC_HDR_SIZE, 1);
    if (!hdr) {
        close(file_fd);
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    int32_t hdr_len = snprintf(hdr, STATIC_HDR_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %lld\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        (long long)st.st_size,
        mime);

    if (hdr_len < 0 || (uint32_t)hdr_len >= STATIC_HDR_SIZE) {
        close(file_fd);
        SEND_STATIC(c->fd, RESPONSE_500);
        return;
    }

    cork(c->fd, 1);

    if (write_all(c->fd, hdr, (uint32_t)hdr_len) < 0) {
        cork(c->fd, 0);
        close(file_fd);
        return;
    }

    if (req->method == HTTP_METHOD_GET && st.st_size > 0) {
        if (sendfile_all(c->fd, file_fd, st.st_size) < 0) {
            tiny_log(WARNING, "[STATIC] sendfile '%s' failed: %s\n", rel, strerror(errno));
        }
    }

    cork(c->fd, 0);
    close(file_fd);
}
