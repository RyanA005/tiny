#ifndef HTTP_H
#define HTTP_H

#include "connection.h"
#include "allocator.h"

#include <errno.h>
#include <unistd.h>

#define REQUEST_BUF_SIZE 4096

void do_http(connection *c, bump *b);

#endif
