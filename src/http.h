#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

#include "connection.h"
#include "allocator.h"

#define HTTP_HEADER_BUF_SIZE 4096

#define HTTP_PARSE_BAD         -1
#define HTTP_PARSE_INCOMPLETE  -2
#define HTTP_PARSE_UNSUPPORTED -3

#define HTTP_FLAG_CONNECTION_CLOSE      0x01
#define HTTP_FLAG_CONNECTION_KEEP_ALIVE 0x02
#define HTTP_FLAG_TRANSFER_ENCODING     0x04
#define HTTP_FLAG_CONTENT_LENGTH        0x08

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
    http_slice target;          /* "/users/123?full=1" */
    http_slice path;            /* "/users/123" */
    http_slice query;           /* "full=1" (len 0 if no '?') */
    http_slice host;            /* Host header value */
    http_slice header_block;    /* all header lines, each ending in CRLF */

    uint32_t content_length;    /* valid if HTTP_FLAG_CONTENT_LENGTH */

    uint16_t header_bytes;      /* bytes up to and including final CRLFCRLF */

    uint8_t method;             /* http_method */
    uint8_t minor_version;      /* 0 -> HTTP/1.0, 1 -> HTTP/1.1 */
    uint8_t flags;              /* HTTP_FLAG_* */
} http_request;

int32_t http_parse_request(const char *buf, uint32_t len, http_request *req); 
uint8_t http_get_header(const char *buf, const http_request *req, const char *name, uint16_t name_len, http_slice *value);
void do_http(connection *c, bump *b);

#endif
