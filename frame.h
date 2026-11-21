#ifndef FRAME_H
#define FRAME_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- the fixed wire format (see the project brief) ------------------- */
/*  length   2 bytes   payload length, 0..1500                            */
/*  src      6 bytes   source MAC                                         */
/*  dst      6 bytes   destination MAC, ff:ff:ff:ff:ff:ff = broadcast     */
/*  vlan     2 bytes   1..4094                                            */
/*  priority 1 byte    0 = high, 1 = normal, 2 = low                      */
/*  payload  length bytes                                                 */

#define FRAME_HEADER_LEN  17
#define FRAME_MAX_PAYLOAD 1500

#define VLAN_MIN 1
#define VLAN_MAX 4094

#define PRIO_HIGH   0
#define PRIO_NORMAL 1
#define PRIO_LOW    2
#define NUM_PRIO    3

#define MAC_LEN     6
#define MAC_STR_LEN 18 /* "aa:bb:cc:dd:ee:ff" + NUL */

struct mera_frame_header
{
    uint16_t length;
    uint8_t  src[MAC_LEN];
    uint8_t  dst[MAC_LEN];
    uint16_t vlan;
    uint8_t  priority;
};

/*
 * A frame on the heap.  One broadcast frame is put into N port queues
 * without being copied N times, so the frame carries a reference count.
 * Whoever drops the last reference frees it.
 *
 * refs is a C11 atomic: no lock is needed and ThreadSanitizer is happy
 * with it, because the atomic read-modify-write is itself the
 * synchronisation point.
 */
struct frame
{
    struct mera_frame_header hdr;
    atomic_int               refs;
    int                      in_port; /* port the frame arrived on, -1 if injected */
    uint8_t                  payload[]; /* hdr.length bytes */
};

/* ---- header pack / unpack (network byte order) ----------------------- */
void frame_serialize(const struct mera_frame_header *h,
                     uint8_t                         buf[FRAME_HEADER_LEN]);
void frame_deserialize(const uint8_t             buf[FRAME_HEADER_LEN],
                       struct mera_frame_header *h);

/* ---- reference counted frames ---------------------------------------- */
struct frame *frame_new(const struct mera_frame_header *h,
                        const uint8_t                  *payload);
struct frame *frame_ref(struct frame *f);
void          frame_unref(struct frame *f);

/* ---- MAC helpers ------------------------------------------------------ */
int  mac_is_broadcast(const uint8_t mac[MAC_LEN]);
int  mac_parse(const char *s, uint8_t mac[MAC_LEN]);
void mac_format(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN]);

/* ---- byte stream I/O that survives short reads and writes ------------- */
/*
 * A read() can return half a frame and a write() can write part of one.
 * Everything that touches a socket goes through these two.
 *
 * io_read_full : returns n on success, 0..n-1 if the peer closed part
 *                way through, -1 on error.
 * io_write_full: returns n on success, -1 on error (errno == EPIPE when
 *                the peer is gone).  EINTR is retried, not reported.
 */
ssize_t io_read_full(int fd, void *buf, size_t n);
ssize_t io_write_full(int fd, const void *buf, size_t n);

/* 1 = got a frame, 0 = peer closed cleanly, -1 = error or bad frame */
int frame_recv(int fd, struct frame **out);
/* 0 = sent, -1 = peer gone / error */
int frame_send(int fd, const struct frame *f);

#endif /* FRAME_H */