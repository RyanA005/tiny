#ifndef STATIC_H
#define STATIC_H

#include <stdint.h>

#include "http.h"

int32_t static_init(const char *docroot);
void static_shutdown(void);

/*
 * Open/stat/build headers for a finished request. Fast: no waiting on the
 * socket. Sets hc phase to WRITE_HDR or WRITE_FIXED and returns
 * HTTP_IO_WANT_WRITE, or HTTP_IO_DONE/CLOSE.
 */
int32_t static_begin(http_conn *hc, uint64_t now_ms);

/* Continue nonblocking header / sendfile progress. */
int32_t static_on_write(http_conn *hc, uint64_t now_ms);

#endif
