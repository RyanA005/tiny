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
void static_set_policy(static_policy policy);
static_policy static_get_policy(void);

/*
 * Open/stat/build headers for a finished request. Fast: no waiting on the
 * socket. Sets hc phase to WRITE_HDR or WRITE_FIXED and returns
 * HTTP_IO_WANT_WRITE, or HTTP_IO_DONE/CLOSE.
 */
int32_t static_begin(http_conn *hc, uint64_t now_ms);

/* Continue nonblocking header / sendfile progress. */
int32_t static_on_write(http_conn *hc, uint64_t now_ms);

#endif
