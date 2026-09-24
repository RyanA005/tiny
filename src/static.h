#ifndef STATIC_H
#define STATIC_H

#include <stdint.h>

#include "allocator.h"
#include "connection.h"
#include "http.h"

int32_t static_init(const char *docroot);
void static_shutdown(void);
void static_serve(connection *c, bump *b, const char *req_buf, const http_request *req);

#endif
