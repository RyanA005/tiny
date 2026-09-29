#ifndef STATIC_H
#define STATIC_H

#include <stdint.h>

#include "http.h"

typedef struct {
    uint8_t allow_dotfiles; /* serve segments like .well-known (not . / ..) */
    uint8_t allow_symlinks; /* drop RESOLVE_NO_SYMLINKS; keep BENEATH */
} static_policy;

int32_t static_init(const char *docroot);
void static_shutdown(void);
/* Drop this thread's open-file cache. Call from each worker before exit. */
void static_cache_clear(void);
/* A request finished with a cached fd. Does not close the live cache fd. */
void static_cache_release(int32_t fd);
void static_set_policy(static_policy policy);
static_policy static_get_policy(void);

/*
 * Open/stat/build headers for a finished request. Fast: no waiting on the
 * socket. Sets hc phase to WRITE_HDR or WRITE_FIXED and returns
 * HTTP_IO_WANT_WRITE, or HTTP_IO_DONE/CLOSE.
 */
int32_t static_begin(http_conn *hc, uint64_t now_ms);

/*
 * Serve url (a path beginning with '/') from dir_fd.
 * redir is the original request path, used if a directory needs a slash.
 * Does not use the open-file cache. That cache is keyed for the single
 * process docroot only, so the static hot path stays unchanged.
 */
int32_t static_begin_at(http_conn *hc, int32_t dir_fd,
                        const char *url, uint16_t url_len,
                        const char *redir, uint16_t redir_len,
                        uint64_t now_ms);

/* Serve one regular file. path is a filesystem path, not a URL. */
int32_t static_begin_file(http_conn *hc, const char *path, uint64_t now_ms);

/* Continue nonblocking header / sendfile progress. */
int32_t static_on_write(http_conn *hc, uint64_t now_ms);

#endif
