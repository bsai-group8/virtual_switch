#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logger.h"

#define LOG_SLOTS 256
#define LOG_LEN   128

struct slot
{
    time_t t;
    char   text[LOG_LEN];
};

static struct slot     ring[LOG_SLOTS];
static int             head, count, done, running;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static pthread_t       thread;
static FILE           *fp;

static void *logger_main(void *arg)
{
    struct slot s;
    struct tm tm;
    char ts[32];

    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;)
    {
        while (count == 0 && !done)
        {
            pthread_cond_wait(&cond, &lock);
        }
        if (count == 0)
        {
            break;
        }
        s = ring[head];
        head = (head + 1) % LOG_SLOTS;
        count--;
        pthread_mutex_unlock(&lock);

        localtime_r(&s.t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(fp, "%s %s\n", ts, s.text);
        fflush(fp);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int log_start(const char *path)
{
    fp = fopen(path, "a");
    if (fp == NULL)
    {
        return -1;
    }
    if (pthread_create(&thread, NULL, logger_main, NULL) != 0)
    {
        fclose(fp);
        return -1;
    }
    running = 1;
    return 0;
}

void log_msg(const char *fmt, ...)
{
    char text[LOG_LEN];
    va_list ap;
    int slot;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&lock);
    if (running && count < LOG_SLOTS)
    {
        slot = (head + count) % LOG_SLOTS;
        ring[slot].t = time(NULL);
        memcpy(ring[slot].text, text, sizeof text);
        count++;
        pthread_cond_signal(&cond);
    }
    pthread_mutex_unlock(&lock);
}

void log_stop(void)
{
    pthread_mutex_lock(&lock);
    done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_mutex_unlock(&lock);
    fclose(fp);
}