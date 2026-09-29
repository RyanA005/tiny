#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>

#define LOG_QUEUE_SIZE 128
#define LOG_MESSAGE_SIZE 160

_Static_assert((LOG_QUEUE_SIZE & (LOG_QUEUE_SIZE - 1)) == 0,
               "LOG_QUEUE_SIZE must be a power of two");

enum log_level { INFO, WARNING, ERROR };

/* Returns 0 on success, -1 if the logger thread failed to start. */
int32_t log_init(void);
void log_shutdown(void);
void tiny_log(enum log_level level, const char *fmt, ...);

/* INFO is off unless built with -DTINY_VERBOSE. */
#ifdef TINY_VERBOSE
#define TINY_LOG_INFO(...) tiny_log(INFO, __VA_ARGS__)
#else
#define TINY_LOG_INFO(...) ((void)0)
#endif

#endif
