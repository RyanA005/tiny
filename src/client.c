#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define DEFAULT_PORT 20000

typedef struct {
    int id;
    int hold_ms;   /* keep socket open this long after connect */
    int ok;
} job;

static void *connect_job(void *arg) {
    job *j = (job *)arg;
    int sd;
    struct sockaddr_in sa;

    j->ok = 0;

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("[%d] socket error: %s\n", j->id, strerror(errno));
        return NULL;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(DEFAULT_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("[%d] connect failed: %s\n", j->id, strerror(errno));
        close(sd);
        return NULL;
    }

    printf("[%d] connected (fd %d)\n", j->id, sd);
    j->ok = 1;

    /* hold open so workers stay busy long enough for the queue to fill */
    if (j->hold_ms > 0) {
        usleep((useconds_t)j->hold_ms * 1000);
    }

    close(sd);
    return NULL;
}

int main(int argc, char **argv) {
    int count = 16;
    int hold_ms = 200;
    int ok = 0;
    int fail = 0;

    if (argc > 1) {
        count = atoi(argv[1]);
    }
    if (argc > 2) {
        hold_ms = atoi(argv[2]);
    }
    if (count < 1) {
        printf("usage: %s [count] [hold_ms]\n", argv[0]);
        printf("  count    concurrent connections (default 16)\n");
        printf("  hold_ms  how long each client holds the socket open (default 200)\n");
        return 1;
    }

    printf("firing %d concurrent connection(s) at localhost:%d (hold %dms)\n",
           count, DEFAULT_PORT, hold_ms);

    pthread_t *threads = calloc((size_t)count, sizeof(pthread_t));
    job *jobs = calloc((size_t)count, sizeof(job));
    if (!threads || !jobs) {
        printf("calloc failed\n");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        jobs[i].id = i;
        jobs[i].hold_ms = hold_ms;
        if (pthread_create(&threads[i], NULL, connect_job, &jobs[i]) != 0) {
            printf("[%d] pthread_create failed: %s\n", i, strerror(errno));
            jobs[i].ok = 0;
            threads[i] = 0;
        }
    }

    for (int i = 0; i < count; i++) {
        if (threads[i]) {
            pthread_join(threads[i], NULL);
        }
        if (jobs[i].ok) {
            ok++;
        } else {
            fail++;
        }
    }

    free(threads);
    free(jobs);

    printf("done: %d ok, %d failed\n", ok, fail);
    printf("watch server for: queue fill (no free worker) then later drain\n");
    return fail ? 1 : 0;
}
