#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <sys/types.h>

#include "connection.h"
#include "allocator.h"

#define HTTP_HEADER_BUF_SIZE 4096
#define HTTP_READ_DEADLINE_MS 10000
#define HTTP_SEND_DEADLINE_MS 30000
#define HTTP_KEEPALIVE_IDLE_MS 15000

#define HTTP_PARSE_BAD         -1
#define HTTP_PARSE_INCOMPLETE  -2
#define HTTP_PARSE_UNSUPPORTED -3

#define HTTP_FLAG_CONNECTION_CLOSE      0x01
#define HTTP_FLAG_CONNECTION_KEEP_ALIVE 0x02
#define HTTP_FLAG_TRANSFER_ENCODING     0x04
#define HTTP_FLAG_CONTENT_LENGTH        0x08

/* Nonblocking conn progress for the worker epoll loop. */
#define HTTP_IO_DONE        0  /* response finished; reuse or close via keep */
#define HTTP_IO_WANT_READ   1
#define HTTP_IO_WANT_WRITE  2
#define HTTP_IO_CLOSE       3  /* error / peer gone; close slot */

#define HTTP_PHASE_READ        0
#define HTTP_PHASE_WRITE_FIXED 1
#define HTTP_PHASE_WRITE_HDR   2
#define HTTP_PHASE_SENDFILE    3

typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE
} http_method;

typedef struct {
    uint16_t off;
    uint16_t len;
} http_slice;

typedef struct {
    http_slice target;
    http_slice path;
    http_slice query;
    http_slice host;
    http_slice header_block;

    uint32_t content_length;

    uint16_t header_bytes;

    uint8_t method;
    uint8_t minor_version;
    uint8_t flags;
} http_request;

typedef struct http_conn {
    connection conn;
    bump *arena;

    /* Owned header buffer (survives bump_reset / reuse). */
    char header_buf[HTTP_HEADER_BUF_SIZE];
    char *buf;
    uint32_t used;
    http_request req;

    uint8_t phase;
    uint64_t deadline_ms;
    uint64_t last_active_ms;

    const char *fixed;
    uint32_t fixed_len;
    uint32_t fixed_off;

    char *out_hdr;
    uint32_t out_hdr_len;
    uint32_t out_hdr_off;

    int32_t file_fd;
    off_t file_size;
    off_t file_off;
    uint8_t send_body;
    uint8_t cork_on;
    uint8_t keep; /* 1 = reuse fd after this response */
    uint64_t t_start_ns;
} http_conn;

int32_t http_parse_request(const char *buf, uint32_t len, http_request *req);
uint8_t http_get_header(const char *buf, const http_request *req, const char *name,
                        uint16_t name_len, http_slice *value);

/* Bind arena + allocate request buffer. Call after bump_reset. */
int32_t http_conn_prepare(http_conn *hc, bump *arena, uint64_t now_ms);

/* Readable / writable event handlers. */
int32_t http_conn_on_read(http_conn *hc, uint64_t now_ms);
int32_t http_conn_on_write(http_conn *hc, uint64_t now_ms);

/*
 * After HTTP_IO_DONE: if hc->keep, memmove leftovers in-place, reset
 * per-request state (no full memset / buf realloc), and arm the next
 * request. Else CLOSE.
 */
int32_t http_conn_reuse(http_conn *hc, bump *arena, uint64_t now_ms);

/* Returns HTTP_IO_CLOSE if past deadline. */
int32_t http_conn_check_deadline(http_conn *hc, uint64_t now_ms);

void http_conn_cleanup(http_conn *hc);

/* Decide keep-alive from the parsed request (GET/HEAD, no body). */
uint8_t http_should_keepalive(const http_request *req);

#endif
