#include "stats.h"

#ifndef TINY_STATS

void tiny_stats_stub(void) {}

#else

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logger.h"

static uint64_t g_counts[STAT_COUNT];
static const char *g_names[STAT_COUNT] = {
    [STAT_ACCEPT]        = "accept",
    [STAT_QUEUE_DROP]    = "queue_drop",
    [STAT_CONN_OPEN]     = "conn_open",
    [STAT_CONN_CLOSE]    = "conn_close",
    [STAT_REQ_OK]        = "req_ok",
    [STAT_REQ_ERR]       = "req_err",
    [STAT_TIMEOUT]       = "timeout",
    [STAT_EAGAIN_READ]   = "eagain_read",
    [STAT_EAGAIN_WRITE]  = "eagain_write",
    [STAT_NS_READ]       = "ns_read",
    [STAT_NS_BEGIN]      = "ns_begin",
    [STAT_NS_WRITE_HDR]  = "ns_write_hdr",
    [STAT_NS_SENDFILE]   = "ns_sendfile",
    [STAT_NS_TOTAL]      = "ns_total",
};

void stats_init(void) {
    memset(g_counts, 0, sizeof(g_counts));
}

void stats_add(uint32_t id, uint64_t n) {
    if (id < STAT_COUNT) {
        __atomic_fetch_add(&g_counts[id], n, __ATOMIC_RELAXED);
    }
}

uint64_t stats_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double avg_ns(uint64_t total_ns, uint64_t n) {
    if (n == 0) {
        return 0.0;
    }
    return (double)total_ns / (double)n;
}

void stats_dump(void) {
    uint64_t snap[STAT_COUNT];
    for (uint32_t i = 0; i < STAT_COUNT; i++) {
        snap[i] = __atomic_load_n(&g_counts[i], __ATOMIC_RELAXED);
    }

    uint64_t ok = snap[STAT_REQ_OK];
    tiny_log(INFO, "[STATS] ---- begin ----\n");
    tiny_log(INFO, "[STATS] accept=%llu drop=%llu open=%llu close=%llu\n",
             (unsigned long long)snap[STAT_ACCEPT],
             (unsigned long long)snap[STAT_QUEUE_DROP],
             (unsigned long long)snap[STAT_CONN_OPEN],
             (unsigned long long)snap[STAT_CONN_CLOSE]);
    tiny_log(INFO, "[STATS] req_ok=%llu req_err=%llu timeout=%llu eagain_r=%llu eagain_w=%llu\n",
             (unsigned long long)snap[STAT_REQ_OK],
             (unsigned long long)snap[STAT_REQ_ERR],
             (unsigned long long)snap[STAT_TIMEOUT],
             (unsigned long long)snap[STAT_EAGAIN_READ],
             (unsigned long long)snap[STAT_EAGAIN_WRITE]);
    tiny_log(INFO, "[STATS] avg_us read=%.1f begin=%.1f write_hdr=%.1f sendfile=%.1f total=%.1f\n",
             avg_ns(snap[STAT_NS_READ], ok) / 1000.0,
             avg_ns(snap[STAT_NS_BEGIN], ok) / 1000.0,
             avg_ns(snap[STAT_NS_WRITE_HDR], ok) / 1000.0,
             avg_ns(snap[STAT_NS_SENDFILE], ok) / 1000.0,
             avg_ns(snap[STAT_NS_TOTAL], ok) / 1000.0);
    tiny_log(INFO, "[STATS] ---- end ----\n");
    (void)g_names;
}

#endif
