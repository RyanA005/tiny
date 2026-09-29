#ifndef STATS_H
#define STATS_H

#include <stdint.h>

enum {
    STAT_ACCEPT = 0,
    STAT_QUEUE_DROP,
    STAT_CONN_OPEN,
    STAT_CONN_CLOSE,

    STAT_RESP_TOTAL,
    STAT_RESP_2XX,
    STAT_RESP_3XX,
    STAT_RESP_4XX,
    STAT_RESP_5XX,

    STAT_TIMEOUT,
    STAT_EAGAIN_READ,
    STAT_EAGAIN_WRITE,

    STAT_NS_READ,
    STAT_NS_READ_N,
    STAT_NS_BEGIN,
    STAT_NS_BEGIN_N,
    STAT_NS_WRITE_HDR,
    STAT_NS_WRITE_HDR_N,
    STAT_NS_SENDFILE,
    STAT_NS_SENDFILE_N,
    STAT_NS_TOTAL,
    STAT_NS_TOTAL_N,

    STAT_COUNT
};

#ifdef TINY_STATS

void stats_init(void);
void stats_add(uint32_t id, uint64_t n);
void stats_dump(void);
void stats_note_status(uint16_t code);
uint64_t stats_now_ns(void);

#define STAT_INC(id)              stats_add((id), 1)
#define STAT_ADD(id, n)           stats_add((id), (n))
#define STAT_TIME_BEGIN(name)     uint64_t _st_##name = stats_now_ns()
#define STAT_TIME_END(id, n_id, name) do { \
        stats_add((id), stats_now_ns() - _st_##name); \
        stats_add((n_id), 1); \
    } while (0)
#define STAT_NOTE_STATUS(code)    stats_note_status((code))

#else

static inline void stats_init(void) {}
static inline void stats_dump(void) {}
static inline void stats_note_status(uint16_t code) { (void)code; }

#define STAT_INC(id)              ((void)0)
#define STAT_ADD(id, n)           ((void)0)
#define STAT_TIME_BEGIN(name)     ((void)0)
#define STAT_TIME_END(id, n_id, name) ((void)0)
#define STAT_NOTE_STATUS(code)    ((void)(code))

#endif

#endif
