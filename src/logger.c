#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_FLUSH_EVERY 32

typedef struct {
    enum log_level level;
    uint8_t ready;
    uint8_t len;
    uint8_t message[LOG_MESSAGE_SIZE];
} log_entry;

typedef struct {
    log_entry data[LOG_QUEUE_SIZE];
    uint32_t head_id __attribute__((aligned(64)));
    uint32_t tail_id __attribute__((aligned(64)));
} log_queue;

static log_queue logs = { 0 };
static pthread_t logger_thread;
static uint8_t logger_run = 1;

static uint8_t enqueue_log(log_entry l);
static uint8_t dequeue_log(log_entry *l);
static void *logger(void *p);

void log_init(void) {
    logger_run = 1;
    pthread_create(&logger_thread, NULL, logger, NULL);
}

void log_shutdown(void) {
    __atomic_store_n(&logger_run, 0, __ATOMIC_RELEASE);
    pthread_join(logger_thread, NULL);
}

void tiny_log(enum log_level level, const char *fmt, ...) {
    log_entry l = { 0 };
    va_list ap;
    int32_t n;

    l.level = level;
    va_start(ap, fmt);
    n = vsnprintf((char *)l.message, LOG_MESSAGE_SIZE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((uint32_t)n >= LOG_MESSAGE_SIZE) {
        l.len = (uint8_t)(LOG_MESSAGE_SIZE - 1);
    } else {
        l.len = (uint8_t)n;
    }
    enqueue_log(l);
}

static void *logger(void *p) {
    (void)p;
    log_entry l = { 0 };
    uint32_t since_flush = 0;

    while (__atomic_load_n(&logger_run, __ATOMIC_ACQUIRE)) {
        if (dequeue_log(&l)) {
            fwrite(l.message, 1, l.len, stdout);
            since_flush++;
            if (since_flush >= LOG_FLUSH_EVERY) {
                fflush(stdout);
                since_flush = 0;
            }
        } else {
            if (since_flush > 0) {
                fflush(stdout);
                since_flush = 0;
            }
            usleep(100);
        }
    }
    while (dequeue_log(&l)) {
        fwrite(l.message, 1, l.len, stdout);
    }
    fflush(stdout);
    return NULL;
}

static uint8_t enqueue_log(log_entry l) {
    uint32_t current_tail_id;

    do {
        current_tail_id = __atomic_load_n(&logs.tail_id, __ATOMIC_RELAXED);
        uint32_t current_head_id = __atomic_load_n(&logs.head_id, __ATOMIC_ACQUIRE);
        if ((current_tail_id - current_head_id) >= LOG_QUEUE_SIZE) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&logs.tail_id, &current_tail_id,
                                          current_tail_id + 1, 0,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

    log_entry *slot = &logs.data[current_tail_id & (LOG_QUEUE_SIZE - 1)];
    slot->level = l.level;
    slot->len = l.len;
    memcpy(slot->message, l.message, l.len);
    slot->message[l.len] = '\0';
    __atomic_store_n(&slot->ready, 1, __ATOMIC_RELEASE);
    return 1;
}

static uint8_t dequeue_log(log_entry *l) {
    uint32_t current_head_id = __atomic_load_n(&logs.head_id, __ATOMIC_RELAXED);
    uint32_t current_tail_id = __atomic_load_n(&logs.tail_id, __ATOMIC_ACQUIRE);

    if (current_head_id == current_tail_id) {
        return 0;
    }
    log_entry *slot = &logs.data[current_head_id & (LOG_QUEUE_SIZE - 1)];
    while (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {}

    l->level = slot->level;
    l->len = slot->len;
    memcpy(l->message, slot->message, slot->len);
    l->message[slot->len] = '\0';
    __atomic_store_n(&slot->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&logs.head_id, current_head_id + 1, __ATOMIC_RELEASE);
    return 1;
}
