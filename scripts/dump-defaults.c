/* Print the config.h values the compiler actually sees, one TSV row each.
 * make uses this so the welcome page matches the binary, including -D overrides.
 */
#include <stdio.h>

#include "config.h"

int main(void) {
    printf("TINY_CACHE_LINE\t%d\n", TINY_CACHE_LINE);
    printf("TINY_BIND\t%s\n", TINY_BIND);
    printf("TINY_BACKLOG\t%d\n", TINY_BACKLOG);
    printf("TINY_WORKERS\t%d\n", TINY_WORKERS);
    printf("TINY_CONNS_PER_WORKER\t%d\n", TINY_CONNS_PER_WORKER);
    printf("TINY_QUEUE_SIZE\t%d\n", TINY_QUEUE_SIZE);
    printf("TINY_KEEPALIVE_MS\t%d\n", TINY_KEEPALIVE_MS);
    printf("TINY_READ_TIMEOUT_MS\t%d\n", TINY_READ_TIMEOUT_MS);
    printf("TINY_SEND_TIMEOUT_MS\t%d\n", TINY_SEND_TIMEOUT_MS);
    printf("TINY_QUEUE_TIMEOUT_MS\t%d\n", TINY_QUEUE_TIMEOUT_MS);
    printf("TINY_HEADER_SIZE\t%d\n", TINY_HEADER_SIZE);
    printf("TINY_BUMP_SIZE\t%d\n", TINY_BUMP_SIZE);
    printf("TINY_EPOLL_WAIT_MS\t%d\n", TINY_EPOLL_WAIT_MS);
    printf("TINY_EPOLL_EVENTS_MAX\t%d\n", TINY_EPOLL_EVENTS_MAX);
    printf("TINY_ALLOW_DOTFILES\t%d\n", TINY_ALLOW_DOTFILES);
    printf("TINY_ALLOW_SYMLINKS\t%d\n", TINY_ALLOW_SYMLINKS);
    return 0;
}
