#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* header pack / unpack                                             */
/* ---------------------------------------------------------------- */

void frame_serialize(const struct mera_frame_header *h,
                     uint8_t                         buf[FRAME_HEADER_LEN])
{
    uint16_t n;

    n = htons(h->length);
    memcpy(buf + 0, &n, 2);
    memcpy(buf + 2, h->src, MAC_LEN);
    memcpy(buf + 8, h->dst, MAC_LEN);
    n = htons(h->vlan);
    memcpy(buf + 14, &n, 2);
    buf[16] = h->priority;
}

void frame_deserialize(const uint8_t             buf[FRAME_HEADER_LEN],
                       struct mera_frame_header *h)
{
    uint16_t n;

    memcpy(&n, buf + 0, 2);
    h->length = ntohs(n);
    memcpy(h->src, buf + 2, MAC_LEN);
    memcpy(h->dst, buf + 8, MAC_LEN);
    memcpy(&n, buf + 14, 2);
    h->vlan     = ntohs(n);
    h->priority = buf[16];
}

/* ---------------------------------------------------------------- */
/* reference counted frames                                         */
/* ---------------------------------------------------------------- */

struct frame *frame_new(const struct mera_frame_header *h,
                        const uint8_t                  *payload)
{
    struct frame *f;

    if (h->length > FRAME_MAX_PAYLOAD)
    {
        return NULL;
    }
    f = malloc(sizeof *f + h->length);
    if (f == NULL)
    {
        return NULL;
    }
    f->hdr     = *h;
    f->in_port = -1;
    atomic_init(&f->refs, 1);
    if (h->length > 0 && payload != NULL)
    {
        memcpy(f->payload, payload, h->length);
    }
    return f;
}

struct frame *frame_ref(struct frame *f)
{
    /* relaxed is enough: we already hold a reference, so the object
     * cannot disappear underneath us while we bump the count. */
    atomic_fetch_add_explicit(&f->refs, 1, memory_order_relaxed);
    return f;
}

void frame_unref(struct frame *f)
{
    int old;

    if (f == NULL)
    {
        return;
    }
    old = atomic_fetch_sub_explicit(&f->refs, 1, memory_order_release);
    if (old == 1)
    {
        /* Acquire fence pairs with every release above, so all the
         * writes other threads made to this frame happen-before the
         * free().  Without it ThreadSanitizer reports a race between
         * the last writer and the freeing thread. */
        atomic_thread_fence(memory_order_acquire);
        free(f);
    }
}

/* ---------------------------------------------------------------- */
/* MAC helpers                                                      */
/* ---------------------------------------------------------------- */

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

int mac_parse(const char *s, uint8_t mac[MAC_LEN])
{
    unsigned v[MAC_LEN];
    int      i;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5])
        != MAC_LEN)
    {
        return -1;
    }
    for (i = 0; i < MAC_LEN; i++)
    {
        if (v[i] > 0xff)
        {
            return -1;
        }
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

void mac_format(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN])
{
    snprintf(out, MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

/* ---------------------------------------------------------------- */
/* partial read / write                                             */
/* ---------------------------------------------------------------- */

ssize_t io_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p    = buf;
    size_t   done = 0;
    ssize_t  r;

    while (done < n)
    {
        r = read(fd, p + done, n - done);
        if (r > 0)
        {
            done += (size_t)r;
            continue;
        }
        if (r == 0)
        {
            return (ssize_t)done; /* peer closed, maybe mid frame */
        }
        if (errno == EINTR)
        {
            continue; /* a signal hit us, not an error */
        }
        return -1;
    }
    return (ssize_t)done;
}

ssize_t io_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p    = buf;
    size_t         done = 0;
    ssize_t        w;

    while (done < n)
    {
        w = write(fd, p + done, n - done);
        if (w > 0)
        {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
        {
            continue;
        }
        /* SIGPIPE is ignored process wide, so a dead peer shows up here
         * as errno == EPIPE instead of killing the switch. */
        return -1;
    }
    return (ssize_t)done;
}

int frame_recv(int fd, struct frame **out)
{
    uint8_t                  hb[FRAME_HEADER_LEN];
    struct mera_frame_header h;
    struct frame            *f;
    ssize_t                  r;

    r = io_read_full(fd, hb, sizeof hb);
    if (r < 0)
    {
        return -1;
    }
    if (r == 0)
    {
        return 0; /* clean disconnect between frames */
    }
    if ((size_t)r < sizeof hb)
    {
        return 0; /* host died in the middle of a header */
    }

    frame_deserialize(hb, &h);
    if (h.length > FRAME_MAX_PAYLOAD)
    {
        return -1; /* lying length, drop the host */
    }

    f = frame_new(&h, NULL);
    if (f == NULL)
    {
        return -1;
    }
    if (h.length > 0)
    {
        r = io_read_full(fd, f->payload, h.length);
        if (r < 0)
        {
            frame_unref(f);
            return -1;
        }
        if ((size_t)r < h.length)
        {
            frame_unref(f); /* died in the middle of a payload */
            return 0;
        }
    }
    *out = f;
    return 1;
}

int frame_send(int fd, const struct frame *f)
{
    uint8_t buf[FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD];
    size_t  total = FRAME_HEADER_LEN + f->hdr.length;

    /* One write() for the whole frame keeps frames from interleaving on
     * the wire, which matters because several threads can target the
     * same socket only through its single sender thread. */
    frame_serialize(&f->hdr, buf);
    if (f->hdr.length > 0)
    {
        memcpy(buf + FRAME_HEADER_LEN, f->payload, f->hdr.length);
    }
    if (io_write_full(fd, buf, total) != (ssize_t)total)
    {
        return -1;
    }
    return 0;
}