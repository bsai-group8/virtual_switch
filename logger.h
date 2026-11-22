#ifndef LOGGER_H
#define LOGGER_H

int  log_start(const char *path);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_stop(void);

#endif