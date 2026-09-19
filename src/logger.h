#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>

#define LOG_QUEUE_SIZE 128
#define LOG_MESSAGE_SIZE 160

enum log_level { INFO, WARNING, ERROR };

void log_init(void);
void log_shutdown(void);
void tiny_log(enum log_level level, const char *fmt, ...);

#endif
