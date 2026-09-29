#include "stats.h"

#ifndef TINY_STATS

void tiny_stats_stub(void) {}

#else

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logger.h"

static uint64_t g_counts[STAT_COUNT];

void stats_init(void) {
    memset(g_counts, 0, sizeof(g_counts));
}

void stats_add(uint32_t id, uint64_t n) {
    if (id < STAT_COUNT) {
        __atomic_fetch_add(&g_counts[id], n, __ATOMIC_RELAXED);
    }
}

void stats_note_status(uint16_t code) {
    STAT_INC(STAT_RESP_TOTAL);
    if (code >= 200 && code < 300) {
        STAT_INC(STAT_RESP_2XX);
    } else if (code >= 300 && code < 400) {
        STAT_INC(STAT_RESP_3XX);
    } else if (code >= 400 && code < 500) {
        STAT_INC(STAT_RESP_4XX);
    } else if (code >= 500 && code < 600) {
        STAT_INC(STAT_RESP_5XX);
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

    tiny_log(INFO, "[STATS] ---- begin ----\n");
    tiny_log(INFO, "[STATS] accept=%llu drop=%llu open=%llu close=%llu\n",
             (unsigned long long)snap[STAT_ACCEPT],
             (unsigned long long)snap[STAT_QUEUE_DROP],
             (unsigned long long)snap[STAT_CONN_OPEN],
             (unsigned long long)snap[STAT_CONN_CLOSE]);
    tiny_log(INFO, "[STATS] resp total=%llu 2xx=%llu 3xx=%llu 4xx=%llu 5xx=%llu\n",
             (unsigned long long)snap[STAT_RESP_TOTAL],
             (unsigned long long)snap[STAT_RESP_2XX],
             (unsigned long long)snap[STAT_RESP_3XX],
             (unsigned long long)snap[STAT_RESP_4XX],
             (unsigned long long)snap[STAT_RESP_5XX]);
    tiny_log(INFO, "[STATS] timeout=%llu eagain_r=%llu eagain_w=%llu\n",
             (unsigned long long)snap[STAT_TIMEOUT],
             (unsigned long long)snap[STAT_EAGAIN_READ],
             (unsigned long long)snap[STAT_EAGAIN_WRITE]);
    tiny_log(INFO, "[STATS] avg_us read=%.1f begin=%.1f write_hdr=%.1f sendfile=%.1f total=%.1f\n",
             avg_ns(snap[STAT_NS_READ], snap[STAT_NS_READ_N]) / 1000.0,
             avg_ns(snap[STAT_NS_BEGIN], snap[STAT_NS_BEGIN_N]) / 1000.0,
             avg_ns(snap[STAT_NS_WRITE_HDR], snap[STAT_NS_WRITE_HDR_N]) / 1000.0,
             avg_ns(snap[STAT_NS_SENDFILE], snap[STAT_NS_SENDFILE_N]) / 1000.0,
             avg_ns(snap[STAT_NS_TOTAL], snap[STAT_NS_TOTAL_N]) / 1000.0);
    tiny_log(INFO, "[STATS] ---- end ----\n");
}

#endif
