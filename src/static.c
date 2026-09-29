#define _GNU_SOURCE

#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "logger.h"
#include "stats.h"

#define STATIC_HDR_SIZE 256
#define STATIC_PATH_MAX 4096
#define OPENAT2_EAGAIN_RETRIES 8
#define FILE_CACHE_SLOTS 64
#define FILE_CACHE_PATH 192
#define FILE_CACHE_RECHECK_MS 1000
#define FILE_CACHE_RETIRED 8

/*
 * Per-worker table. Metadata is one cache line; path[192] is the next three.
 * A hit shares fd across requests (sendfile takes an explicit offset).
 * Recheck the path at most once a second so an edit shows up without a
 * stat on every request. fd == -1 is an empty probe stop, -2 is a tombstone.
 */
typedef struct {
    uint64_t hash;
    uint64_t checked_ms;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    off_t size;
    uint64_t ino;
    const char *mime;
    int32_t fd;
    uint16_t path_len;
    uint16_t uses;
    char path[FILE_CACHE_PATH];
} cached_file;

_Static_assert(sizeof(off_t) == 8, "off_t width");
_Static_assert(sizeof(cached_file) == 256, "cached_file is 4 cache lines");
_Static_assert(offsetof(cached_file, path) == 64, "path starts on the second cache line");

typedef struct {
    int32_t fd;
    uint16_t uses;
} retired_fd;

static __thread int32_t file_cache_ready;
static __thread cached_file file_cache[FILE_CACHE_SLOTS];
static __thread retired_fd file_cache_retired[FILE_CACHE_RETIRED];

static int32_t docroot_fd = -1;
static static_policy g_policy = {
    .allow_dotfiles = 1, /* .well-known etc.; still blocks . and .. */
    .allow_symlinks = 0,
};

static const char RESPONSE_403_CLOSE[] =
    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
static const char RESPONSE_403_KEEP[] =
    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Length: 0\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char RESPONSE_404_CLOSE[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
static const char RESPONSE_404_KEEP[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char RESPONSE_405_CLOSE[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "Allow: GET, HEAD\r\n"
    "Connection: close\r\n"
    "\r\n";
static const char RESPONSE_405_KEEP[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "Allow: GET, HEAD\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char RESPONSE_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char RESPONSE_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static void note_done(http_conn *hc) {
    STAT_NOTE_STATUS(hc->status_code);
#ifdef TINY_STATS
    if (hc->t_start_ns) {
        STAT_ADD(STAT_NS_TOTAL, stats_now_ns() - hc->t_start_ns);
        STAT_INC(STAT_NS_TOTAL_N);
    }
#endif
}

/* allow_keep: use request keepalive policy (404/403/405). Else force close. */
static void arm_fixed(http_conn *hc, const char *close_resp, uint32_t close_len,
                      const char *keep_resp, uint32_t keep_len,
                      uint8_t allow_keep, uint16_t status, uint64_t now_ms) {
    uint8_t keep = allow_keep && http_should_keepalive(&hc->req);
    if (keep && keep_resp) {
        hc->fixed = keep_resp;
        hc->fixed_len = keep_len;
        hc->keep = 1;
    } else {
        hc->fixed = close_resp;
        hc->fixed_len = close_len;
        hc->keep = 0;
    }
    hc->fixed_off = 0;
    hc->phase = HTTP_PHASE_WRITE_FIXED;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
    hc->status_code = status;
    if (hc->file_fd >= 0) {
        if (hc->file_owned) {
            close(hc->file_fd);
        } else {
            static_cache_release(hc->file_fd);
        }
        hc->file_fd = -1;
        hc->file_owned = 1;
    }
}

#define ARM_CLOSE(hc, resp, code, now) \
    arm_fixed((hc), (resp), (uint32_t)(sizeof(resp) - 1), 0, 0, 0, (code), (now))
#define ARM_KEEPABLE(hc, close_r, keep_r, code, now) \
    arm_fixed((hc), (close_r), (uint32_t)(sizeof(close_r) - 1), \
              (keep_r), (uint32_t)(sizeof(keep_r) - 1), 1, (code), (now))

static inline uint8_t ascii_lower(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c + ('a' - 'A'));
    }
    return c;
}

static uint8_t ext_ieq(const char *ext, uint32_t elen, const char *lit, uint32_t n) {
    if (elen != n) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (ascii_lower((uint8_t)ext[i]) != (uint8_t)lit[i]) {
            return 0;
        }
    }
    return 1;
}

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

    if (ext_ieq(ext, elen, "html", 4)) return "text/html; charset=utf-8";
    if (ext_ieq(ext, elen, "htm", 3))  return "text/html; charset=utf-8";
    if (ext_ieq(ext, elen, "css", 3))  return "text/css; charset=utf-8";
    if (ext_ieq(ext, elen, "js", 2))   return "text/javascript; charset=utf-8";
    if (ext_ieq(ext, elen, "json", 4)) return "application/json";
    if (ext_ieq(ext, elen, "png", 3))  return "image/png";
    if (ext_ieq(ext, elen, "jpg", 3))  return "image/jpeg";
    if (ext_ieq(ext, elen, "jpeg", 4)) return "image/jpeg";
    if (ext_ieq(ext, elen, "gif", 3))  return "image/gif";
    if (ext_ieq(ext, elen, "svg", 3))  return "image/svg+xml";
    if (ext_ieq(ext, elen, "ico", 3))  return "image/x-icon";
    if (ext_ieq(ext, elen, "txt", 3))  return "text/plain; charset=utf-8";
    if (ext_ieq(ext, elen, "wasm", 4)) return "application/wasm";
    return "application/octet-stream";
}

static int32_t hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/*
 * Strict percent-decode. Rejects incomplete/invalid %XX, NUL, encoded
 * separators, and control bytes (<0x20, 0x7f) to avoid log/path injection.
 */
static uint32_t percent_decode(const char *in, uint16_t in_len,
                               char *out, uint32_t out_cap) {
    uint32_t o = 0;
    for (uint16_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if ((uint16_t)(i + 2) >= in_len) {
                return 0;
            }
            int32_t hi = hex_val(in[i + 1]);
            int32_t lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return 0;
            }
            c = (unsigned char)((hi << 4) | lo);
            i = (uint16_t)(i + 2);
            if (c == '/' || c == '\\') {
                return 0;
            }
        }
        if (c < 0x20 || c == 0x7f) {
            return 0;
        }
        if (o + 1 >= out_cap) {
            return 0;
        }
        out[o++] = (char)c;
    }
    if (o >= out_cap) {
        return 0;
    }
    out[o] = '\0';
    return o;
}

static uint32_t normalize_path(const char *in, uint32_t in_len, char *out, uint32_t out_cap) {
    if (in_len == 0 || in[0] != '/') {
        return 0;
    }

    uint32_t o = 0;
    uint32_t i = 1;

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

        /* Always reject . and .. ; other dotfiles gated by policy. */
        if (seg_len == 1 && in[seg_start] == '.') {
            return 0;
        }
        if (seg_len == 2 && in[seg_start] == '.' && in[seg_start + 1] == '.') {
            return 0;
        }
        if (in[seg_start] == '.' && !g_policy.allow_dotfiles) {
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

/* 301 to path + '/' [ + ?query ]. path/query are raw request slices. */
static int32_t arm_dir_redirect(http_conn *hc, const char *path, uint16_t path_len,
                                uint64_t now_ms) {
    uint16_t qlen = hc->req.query.len;
    const char *query = qlen ? (hc->buf + hc->req.query.off) : 0;
    uint32_t need = 160u + (uint32_t)path_len + 1u + (qlen ? (1u + qlen) : 0u);
    char *hdr = bump_alloc(hc->arena, need, 1);
    if (!hdr) {
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    hc->keep = http_should_keepalive(&hc->req);
    int32_t n;
    if (qlen && query) {
        n = snprintf(hdr, need,
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: %.*s/?%.*s\r\n"
            "Content-Length: 0\r\n"
            "Connection: %s\r\n"
            "\r\n",
            (int)path_len, path,
            (int)qlen, query,
            hc->keep ? "keep-alive" : "close");
    } else {
        n = snprintf(hdr, need,
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: %.*s/\r\n"
            "Content-Length: 0\r\n"
            "Connection: %s\r\n"
            "\r\n",
            (int)path_len, path,
            hc->keep ? "keep-alive" : "close");
    }
    if (n < 0 || (uint32_t)n >= need) {
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    hc->fixed = hdr;
    hc->fixed_len = (uint32_t)n;
    hc->fixed_off = 0;
    hc->phase = HTTP_PHASE_WRITE_FIXED;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
    hc->status_code = 301;
    if (hc->file_fd >= 0) {
        if (hc->file_owned) {
            close(hc->file_fd);
        } else {
            static_cache_release(hc->file_fd);
        }
        hc->file_fd = -1;
        hc->file_owned = 1;
    }
    return HTTP_IO_WANT_WRITE;
}

static int32_t open_under(int32_t dirfd, const char *rel) {
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(O_RDONLY | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH;
    if (!g_policy.allow_symlinks) {
        how.resolve |= RESOLVE_NO_SYMLINKS;
    } else {
        how.resolve |= RESOLVE_NO_MAGICLINKS;
    }

    for (int32_t i = 0; i < OPENAT2_EAGAIN_RETRIES; i++) {
        int32_t fd = (int32_t)syscall(SYS_openat2, dirfd, rel, &how, sizeof(how));
        if (fd >= 0 || errno != EAGAIN) {
            return fd;
        }
    }
    return -1;
}

static int32_t open_under_docroot(const char *rel) {
    return open_under(docroot_fd, rel);
}

static void arm_open_error(http_conn *hc, const char *rel, uint64_t now_ms) {
    if (errno == ENOENT || errno == ENOTDIR) {
        ARM_KEEPABLE(hc, RESPONSE_404_CLOSE, RESPONSE_404_KEEP, 404, now_ms);
    } else if (errno == EACCES || errno == EPERM || errno == ELOOP) {
        ARM_KEEPABLE(hc, RESPONSE_403_CLOSE, RESPONSE_403_KEEP, 403, now_ms);
    } else {
        tiny_log(WARNING, "[STATIC] open '%s' failed: %s\n", rel, strerror(errno));
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
    }
}

static void file_cache_init(void) {
    if (file_cache_ready) {
        return;
    }
    for (uint32_t i = 0; i < FILE_CACHE_SLOTS; i++) {
        file_cache[i].fd = -1;
    }
    for (uint32_t i = 0; i < FILE_CACHE_RETIRED; i++) {
        file_cache_retired[i].fd = -1;
        file_cache_retired[i].uses = 0;
    }
    file_cache_ready = 1;
}

static void retire_fd(int32_t fd, uint16_t uses) {
    if (fd < 0) {
        return;
    }
    if (uses == 0) {
        close(fd);
        return;
    }
    for (uint32_t i = 0; i < FILE_CACHE_RETIRED; i++) {
        if (file_cache_retired[i].fd < 0) {
            file_cache_retired[i].fd = fd;
            file_cache_retired[i].uses = uses;
            return;
        }
    }
    /* In-flight sends still hold fd. Dropping it here would break them. */
}

static void tombstone(cached_file *e) {
    int32_t fd = e->fd;
    uint16_t uses = e->uses;
    e->fd = -2;
    e->uses = 0;
    e->hash = 0;
    retire_fd(fd, uses);
}

static uint64_t path_hash(const char *s, uint32_t n) {
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static void fill_entry(cached_file *e, uint64_t hash, const char *path, uint32_t len,
                       int32_t fd, const struct stat *st, const char *mime,
                       uint64_t now_ms) {
    e->hash = hash;
    e->checked_ms = now_ms;
    e->mtime_sec = (int64_t)st->st_mtim.tv_sec;
    e->mtime_nsec = (int64_t)st->st_mtim.tv_nsec;
    e->size = st->st_size;
    e->ino = (uint64_t)st->st_ino;
    e->mime = mime;
    e->fd = fd;
    e->path_len = (uint16_t)len;
    e->uses = 0;
    memcpy(e->path, path, len);
    e->path[len] = '\0';
}

static int32_t same_meta(const cached_file *e, const struct stat *st) {
    return st->st_size == e->size &&
           (int64_t)st->st_mtim.tv_sec == e->mtime_sec &&
           (int64_t)st->st_mtim.tv_nsec == e->mtime_nsec &&
           (uint64_t)st->st_ino == e->ino;
}

static cached_file *file_cache_find(const char *path, uint32_t len) {
    file_cache_init();
    if (len == 0 || len >= FILE_CACHE_PATH) {
        return 0;
    }
    uint64_t hash = path_hash(path, len);
    uint32_t slot = (uint32_t)hash & (FILE_CACHE_SLOTS - 1);
    for (uint32_t n = 0; n < FILE_CACHE_SLOTS; n++) {
        cached_file *e = &file_cache[slot];
        if (e->fd == -1) {
            return 0;
        }
        if (e->fd >= 0 && e->hash == hash && e->path_len == len &&
            memcmp(e->path, path, len) == 0) {
            return e;
        }
        slot = (slot + 1) & (FILE_CACHE_SLOTS - 1);
    }
    return 0;
}

/* 1: entry is current. 0: dropped, caller should open the path itself. */
static int32_t file_cache_recheck(cached_file *e, uint64_t now_ms) {
    struct stat st;
    int32_t flags;
    int32_t fd;

    if (now_ms - e->checked_ms < FILE_CACHE_RECHECK_MS) {
        return 1;
    }
    flags = g_policy.allow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(docroot_fd, e->path, &st, flags) < 0 ||
        !S_ISREG(st.st_mode) || st.st_size < 0) {
        tombstone(e);
        return 0;
    }
    if (same_meta(e, &st)) {
        e->checked_ms = now_ms;
        return 1;
    }
    fd = open_under_docroot(e->path);
    if (fd < 0) {
        tombstone(e);
        return 0;
    }
    retire_fd(e->fd, e->uses);
    e->fd = fd;
    e->uses = 0;
    e->size = st.st_size;
    e->mtime_sec = (int64_t)st.st_mtim.tv_sec;
    e->mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
    e->ino = (uint64_t)st.st_ino;
    e->mime = mime_from_path(e->path, e->path_len);
    e->checked_ms = now_ms;
    return 1;
}

static void file_cache_hold(cached_file *e) {
    if (e->uses < UINT16_MAX) {
        e->uses++;
    }
}

/* Returns the entry if the cache now owns fd. */
static cached_file *file_cache_store(const char *path, uint32_t len, int32_t fd,
                                    const struct stat *st, const char *mime,
                                    uint64_t now_ms) {
    int32_t reuse = -1;
    uint64_t hash;
    uint32_t slot;

    file_cache_init();
    if (len == 0 || len >= FILE_CACHE_PATH || fd < 0) {
        return 0;
    }
    hash = path_hash(path, len);
    slot = (uint32_t)hash & (FILE_CACHE_SLOTS - 1);
    for (uint32_t n = 0; n < FILE_CACHE_SLOTS; n++) {
        cached_file *e = &file_cache[slot];
        if (e->fd == -1) {
            fill_entry(e, hash, path, len, fd, st, mime, now_ms);
            return e;
        }
        if (e->fd == -2 && reuse < 0) {
            reuse = (int32_t)slot;
        }
        slot = (slot + 1) & (FILE_CACHE_SLOTS - 1);
    }
    if (reuse >= 0) {
        cached_file *e = &file_cache[reuse];
        fill_entry(e, hash, path, len, fd, st, mime, now_ms);
        return e;
    }
    return 0;
}

void static_cache_release(int32_t fd) {
    if (!file_cache_ready || fd < 0) {
        return;
    }
    for (uint32_t i = 0; i < FILE_CACHE_SLOTS; i++) {
        if (file_cache[i].fd == fd) {
            if (file_cache[i].uses > 0) {
                file_cache[i].uses--;
            }
            return;
        }
    }
    for (uint32_t i = 0; i < FILE_CACHE_RETIRED; i++) {
        if (file_cache_retired[i].fd == fd) {
            if (file_cache_retired[i].uses > 0) {
                file_cache_retired[i].uses--;
            }
            if (file_cache_retired[i].uses == 0) {
                close(fd);
                file_cache_retired[i].fd = -1;
            }
            return;
        }
    }
}

void static_cache_clear(void) {
    if (!file_cache_ready) {
        return;
    }
    for (uint32_t i = 0; i < FILE_CACHE_SLOTS; i++) {
        if (file_cache[i].fd >= 0) {
            close(file_cache[i].fd);
            file_cache[i].fd = -1;
        }
    }
    for (uint32_t i = 0; i < FILE_CACHE_RETIRED; i++) {
        if (file_cache_retired[i].fd >= 0) {
            close(file_cache_retired[i].fd);
            file_cache_retired[i].fd = -1;
        }
    }
    file_cache_ready = 0;
}

static void cork(http_conn *hc, int32_t on);

static int32_t arm_regular(http_conn *hc, int32_t file_fd, off_t size,
                           const char *mime, uint8_t owned, uint64_t now_ms) {
    char *hdr = bump_alloc(hc->arena, STATIC_HDR_SIZE, 1);
    if (!hdr) {
        if (owned) {
            close(file_fd);
        }
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    hc->keep = http_should_keepalive(&hc->req);
    hc->status_code = 200;

    int32_t hdr_len = snprintf(hdr, STATIC_HDR_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %lld\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        (long long)size,
        mime,
        hc->keep ? "keep-alive" : "close");

    if (hdr_len < 0 || (uint32_t)hdr_len >= STATIC_HDR_SIZE) {
        if (owned) {
            close(file_fd);
        }
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    hc->out_hdr = hdr;
    hc->out_hdr_len = (uint32_t)hdr_len;
    hc->out_hdr_off = 0;
    hc->file_fd = file_fd;
    hc->file_owned = owned;
    hc->file_size = size;
    hc->file_off = 0;
    hc->send_body = (hc->req.method == HTTP_METHOD_GET && size > 0) ? 1 : 0;
    hc->phase = HTTP_PHASE_WRITE_HDR;
    hc->deadline_ms = now_ms + hc->send_timeout_ms;
    cork(hc, 1);
    return HTTP_IO_WANT_WRITE;
}

static void cork(http_conn *hc, int32_t on) {
    setsockopt(hc->conn.fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
    hc->cork_on = on ? 1 : 0;
}

void static_set_policy(static_policy policy) {
    g_policy = policy;
}

static_policy static_get_policy(void) {
    return g_policy;
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
    TINY_LOG_INFO("[STATIC] docroot '%s' (fd=%d) dotfiles=%u symlinks=%u\n",
                  docroot, fd, g_policy.allow_dotfiles, g_policy.allow_symlinks);
    return 0;
}

void static_shutdown(void) {
    if (docroot_fd >= 0) {
        close(docroot_fd);
        docroot_fd = -1;
    }
}

/*
 * use_cache is 1 only for the process docroot. Other mounts pass 0 so a
 * relative path under a second directory cannot collide in the cache.
 * url is decoded as the filesystem path. redir is the client-facing path
 * used for a trailing-slash redirect.
 */
static inline __attribute__((always_inline)) int32_t static_serve(http_conn *hc, int32_t dir_fd, int use_cache,
                            const char *url, uint16_t url_len,
                            const char *redir, uint16_t redir_len,
                            uint64_t now_ms) {
    STAT_TIME_BEGIN(begin);

    if (hc->req.method != HTTP_METHOD_GET && hc->req.method != HTTP_METHOD_HEAD) {
        ARM_KEEPABLE(hc, RESPONSE_405_CLOSE, RESPONSE_405_KEEP, 405, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    if (dir_fd < 0) {
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    char *decoded = bump_alloc(hc->arena, STATIC_PATH_MAX, 1);
    char *rel = bump_alloc(hc->arena, STATIC_PATH_MAX, 1);
    if (!decoded || !rel) {
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    uint32_t dec_len = percent_decode(url, url_len, decoded, STATIC_PATH_MAX);
    if (dec_len == 0) {
        ARM_CLOSE(hc, RESPONSE_400, 400, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    uint32_t rel_len = normalize_path(decoded, dec_len, rel, STATIC_PATH_MAX);
    if (rel_len == 0) {
        ARM_KEEPABLE(hc, RESPONSE_404_CLOSE, RESPONSE_404_KEEP, 404, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    if (use_cache) {
        cached_file *hit = file_cache_find(rel, rel_len);
        if (hit && file_cache_recheck(hit, now_ms)) {
            file_cache_hold(hit);
            int32_t rc = arm_regular(hc, hit->fd, hit->size, hit->mime, 0, now_ms);
            STAT_TIME_END(STAT_NS_BEGIN, STAT_NS_BEGIN_N, begin);
            return rc;
        }
    }

    int32_t file_fd = open_under(dir_fd, rel);
    if (file_fd < 0) {
        arm_open_error(hc, rel, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        if (decoded[dec_len - 1] != '/') {
            return arm_dir_redirect(hc, redir, redir_len, now_ms);
        }
        if (rel_len + 11 >= STATIC_PATH_MAX) {
            ARM_KEEPABLE(hc, RESPONSE_404_CLOSE, RESPONSE_404_KEEP, 404, now_ms);
            return HTTP_IO_WANT_WRITE;
        }
        memcpy(rel + rel_len, "/index.html", 11);
        rel_len += 11;
        rel[rel_len] = '\0';

        if (use_cache) {
            cached_file *hit = file_cache_find(rel, rel_len);
            if (hit && file_cache_recheck(hit, now_ms)) {
                file_cache_hold(hit);
                int32_t rc = arm_regular(hc, hit->fd, hit->size, hit->mime, 0, now_ms);
                STAT_TIME_END(STAT_NS_BEGIN, STAT_NS_BEGIN_N, begin);
                return rc;
            }
        }

        file_fd = open_under(dir_fd, rel);
        if (file_fd < 0) {
            arm_open_error(hc, rel, now_ms);
            return HTTP_IO_WANT_WRITE;
        }
        if (fstat(file_fd, &st) < 0) {
            close(file_fd);
            ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
            return HTTP_IO_WANT_WRITE;
        }
    }

    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file_fd);
        ARM_KEEPABLE(hc, RESPONSE_403_CLOSE, RESPONSE_403_KEEP, 403, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    const char *mime = mime_from_path(rel, rel_len);
    uint8_t owned = 1;
    if (use_cache) {
        cached_file *stored = file_cache_store(rel, rel_len, file_fd, &st, mime, now_ms);
        if (stored) {
            file_cache_hold(stored);
            owned = 0;
        }
    }
    int32_t rc = arm_regular(hc, file_fd, st.st_size, mime, owned, now_ms);
    STAT_TIME_END(STAT_NS_BEGIN, STAT_NS_BEGIN_N, begin);
    return rc;
}

int32_t static_begin(http_conn *hc, uint64_t now_ms) {
    const char *loc;
    uint16_t loc_len;
    char root_path[2];

    if (hc->req.path.len == 0) {
        /* Absolute-form with empty path -> "/". */
        root_path[0] = '/';
        root_path[1] = '\0';
        loc = root_path;
        loc_len = 1;
    } else {
        loc = hc->buf + hc->req.path.off;
        loc_len = hc->req.path.len;
    }
    return static_serve(hc, docroot_fd, 1, loc, loc_len, loc, loc_len, now_ms);
}

int32_t static_begin_at(http_conn *hc, int32_t dir_fd,
                        const char *url, uint16_t url_len,
                        const char *redir, uint16_t redir_len,
                        uint64_t now_ms) {
    return static_serve(hc, dir_fd, 0, url, url_len, redir, redir_len, now_ms);
}

int32_t static_begin_file(http_conn *hc, const char *path, uint64_t now_ms) {
    uint32_t plen;

    if (hc->req.method != HTTP_METHOD_GET && hc->req.method != HTTP_METHOD_HEAD) {
        ARM_KEEPABLE(hc, RESPONSE_405_CLOSE, RESPONSE_405_KEEP, 405, now_ms);
        return HTTP_IO_WANT_WRITE;
    }
    if (!path || !path[0]) {
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    int32_t file_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
        arm_open_error(hc, path, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        ARM_CLOSE(hc, RESPONSE_500, 500, now_ms);
        return HTTP_IO_WANT_WRITE;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file_fd);
        ARM_KEEPABLE(hc, RESPONSE_403_CLOSE, RESPONSE_403_KEEP, 403, now_ms);
        return HTTP_IO_WANT_WRITE;
    }

    plen = (uint32_t)strlen(path);
    return arm_regular(hc, file_fd, st.st_size, mime_from_path(path, plen), 1, now_ms);
}

int32_t static_on_write(http_conn *hc, uint64_t now_ms) {
    (void)now_ms;

    if (hc->phase == HTTP_PHASE_WRITE_HDR) {
        STAT_TIME_BEGIN(whdr);
        while (hc->out_hdr_off < hc->out_hdr_len) {
            ssize_t n = send(hc->conn.fd, hc->out_hdr + hc->out_hdr_off,
                             hc->out_hdr_len - hc->out_hdr_off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    STAT_INC(STAT_EAGAIN_WRITE);
                    STAT_TIME_END(STAT_NS_WRITE_HDR, STAT_NS_WRITE_HDR_N, whdr);
                    return HTTP_IO_WANT_WRITE;
                }
                STAT_TIME_END(STAT_NS_WRITE_HDR, STAT_NS_WRITE_HDR_N, whdr);
                return HTTP_IO_CLOSE;
            }
            if (n == 0) {
                STAT_TIME_END(STAT_NS_WRITE_HDR, STAT_NS_WRITE_HDR_N, whdr);
                return HTTP_IO_CLOSE;
            }
            hc->out_hdr_off += (uint32_t)n;
        }
        STAT_TIME_END(STAT_NS_WRITE_HDR, STAT_NS_WRITE_HDR_N, whdr);

        if (!hc->send_body) {
            cork(hc, 0);
            if (hc->file_fd >= 0) {
                if (hc->file_owned) {
                    close(hc->file_fd);
                } else {
                    static_cache_release(hc->file_fd);
                }
            }
            hc->file_fd = -1;
            hc->file_owned = 1;
            note_done(hc);
            return HTTP_IO_DONE;
        }
        hc->phase = HTTP_PHASE_SENDFILE;
    }

    STAT_TIME_BEGIN(sf);
    while (hc->file_off < hc->file_size) {
        size_t want = (size_t)(hc->file_size - hc->file_off);
        if (want > (1u << 20)) {
            want = 1u << 20;
        }
        ssize_t n = sendfile(hc->conn.fd, hc->file_fd, &hc->file_off, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                STAT_INC(STAT_EAGAIN_WRITE);
                STAT_TIME_END(STAT_NS_SENDFILE, STAT_NS_SENDFILE_N, sf);
                return HTTP_IO_WANT_WRITE;
            }
            STAT_TIME_END(STAT_NS_SENDFILE, STAT_NS_SENDFILE_N, sf);
            return HTTP_IO_CLOSE;
        }
        if (n == 0) {
            STAT_TIME_END(STAT_NS_SENDFILE, STAT_NS_SENDFILE_N, sf);
            return HTTP_IO_CLOSE;
        }
    }
    STAT_TIME_END(STAT_NS_SENDFILE, STAT_NS_SENDFILE_N, sf);

    cork(hc, 0);
    if (hc->file_fd >= 0) {
        if (hc->file_owned) {
            close(hc->file_fd);
        } else {
            static_cache_release(hc->file_fd);
        }
    }
    hc->file_fd = -1;
    hc->file_owned = 1;
    note_done(hc);
    return HTTP_IO_DONE;
}
