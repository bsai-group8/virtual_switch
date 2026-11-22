#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frame.h"

struct frame *frame_new(uint16_t len)
{
    struct frame *f = calloc(1, sizeof *f + len);

    if (f != NULL)
    {
        f->len = len;
        atomic_init(&f->refs, 1);
    }
    return f;
}

void frame_ref(struct frame *f)
{
    atomic_fetch_add(&f->refs, 1);
}

void frame_unref(struct frame *f)
{
    if (atomic_fetch_sub(&f->refs, 1) == 1)
    {
        free(f);
    }
}

int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    ssize_t r;

    while (got < n)
    {
        r = read(fd, (char *)buf + got, n - got);
        if (r == 0)
        {
            return 0;
        }
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        got += (size_t)r;
    }
    return 1;
}

int write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    ssize_t r;

    while (done < n)
    {
        r = write(fd, (const char *)buf + done, n - done);
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

int frame_recv(int fd, struct frame **out)
{
    uint8_t h[HDR_LEN];
    uint16_t t;
    struct frame *f;
    int rc;

    rc = read_full(fd, h, HDR_LEN);
    if (rc <= 0)
    {
        return rc;
    }
    memcpy(&t, h, 2);
    if (ntohs(t) > MAX_PAYLOAD)
    {
        return -1;
    }
    f = frame_new(ntohs(t));
    if (f == NULL)
    {
        return -1;
    }
    memcpy(f->src, h + 2, MAC_LEN);
    memcpy(f->dst, h + 8, MAC_LEN);
    memcpy(&t, h + 14, 2);
    f->vlan = ntohs(t);
    f->prio = h[16] < NUM_PRIO ? h[16] : 1;
    rc = read_full(fd, f->payload, f->len);
    if (rc <= 0)
    {
        frame_unref(f);
        return rc;
    }
    *out = f;
    return 1;
}

int frame_send(int fd, const struct frame *f)
{
    uint8_t buf[HDR_LEN + MAX_PAYLOAD];
    uint16_t t;

    t = htons(f->len);
    memcpy(buf, &t, 2);
    memcpy(buf + 2, f->src, MAC_LEN);
    memcpy(buf + 8, f->dst, MAC_LEN);
    t = htons(f->vlan);
    memcpy(buf + 14, &t, 2);
    buf[16] = f->prio;
    memcpy(buf + HDR_LEN, f->payload, f->len);
    return write_full(fd, buf, (size_t)HDR_LEN + f->len);
}

int mac_parse(const char *s, uint8_t mac[MAC_LEN])
{
    unsigned a[MAC_LEN];
    int i;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
    {
        return -1;
    }
    for (i = 0; i < MAC_LEN; i++)
    {
        if (a[i] > 255)
        {
            return -1;
        }
        mac[i] = (uint8_t)a[i];
    }
    return 0;
}

void mac_format(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN])
{
    snprintf(out, MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int mac_is_broadcast(const uint8_t mac[MAC_LEN])
{
    int i;

    for (i = 0; i < MAC_LEN; i++)
    {
        if (mac[i] != 0xff)
        {
            return 0;
        }
    }
    return 1;
}

long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long mono_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

void ts_in_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}