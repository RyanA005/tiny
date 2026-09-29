#ifndef TINY_PROXY_H
#define TINY_PROXY_H

#include <stdint.h>

#include <sys/socket.h>

#include "http.h"
#include "routes.h"

/*
 * One upstream socket owned by the same worker slot as the client.
 * Buffers live in the slot so a proxied request does not malloc.
 * Static requests leave fd at -1 and do not read the buffers.
 */

#define PROXY_HDR_MAX 8192
#define PROXY_BUF_MAX 8192

#define PX_SEND_100     0x01
#define PX_CORK         0x02
#define PX_RESP_CL      0x04
#define PX_RESP_CHUNKED 0x08
#define PX_RESP_EOF     0x10
#define PX_BODY_DONE    0x20
#define PX_RESP_SENT    0x40
#define PX_UP_CLOSE     0x80   /* upstream response forbids reuse */
#define PX_CLIENT_KEEP  0x100  /* client may send another request */
#define PX_REUSED       0x200  /* fd came from the idle pool */
#define PX_OVERREAD     0x400  /* read past the framed body; do not pool */

typedef struct proxy_conn {
    int32_t fd;
    uint8_t phase;
    uint8_t armed;    /* HTTP_IO_* currently registered for fd */
    uint8_t in_epoll;
    uint8_t chunk_state;
    uint8_t saw_digit;
    uint8_t cont_off; /* bytes of "100 Continue" already written */
    uint16_t flags;
    uint16_t status;
    socklen_t up_len;
    struct sockaddr_storage up_addr;
    uint32_t hdr_off;
    uint32_t hdr_len;
    uint32_t buf_off;
    uint32_t buf_len;
    uint32_t body_off;  /* into hc->buf: already-read request body */
    uint32_t body_end;
    uint32_t body_left; /* still to read from the client */
    uint64_t resp_left;
    uint64_t chunk_size;
    char hdr[PROXY_HDR_MAX]; /* rewritten request, then rewritten response */
    char buf[PROXY_BUF_MAX]; /* body shuttle and upstream header accumulate */
} proxy_conn;

void proxy_conn_reset(proxy_conn *px);
void proxy_close(int epfd, proxy_conn *px);
void proxy_pool_clear(void);

int32_t proxy_begin(http_conn *hc, proxy_conn *px, const tiny_route *route,
                    int epfd, uint32_t slot_idx, uint64_t now_ms);
int32_t proxy_pump(http_conn *hc, proxy_conn *px, int epfd, uint32_t slot_idx,
                   uint64_t now_ms);

#endif
