#ifndef FRAME_H
#define FRAME_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define HDR_LEN     17
#define MAX_PAYLOAD 1500
#define MAC_LEN     6
#define MAC_STR_LEN 18
#define NUM_PRIO    3

struct frame
{
    uint16_t   len;
    uint8_t    src[MAC_LEN];
    uint8_t    dst[MAC_LEN];
    uint16_t   vlan;
    uint8_t    prio;
    atomic_int refs;
    uint8_t    payload[];
};

struct frame *frame_new(uint16_t len);
void          frame_ref(struct frame *f);
void          frame_unref(struct frame *f);

int frame_recv(int fd, struct frame **out);
int frame_send(int fd, const struct frame *f);

int read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

int  mac_parse(const char *s, uint8_t mac[MAC_LEN]);
void mac_format(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN]);
int  mac_is_broadcast(const uint8_t mac[MAC_LEN]);

long long now_ms(void);
long      mono_sec(void);
void      ts_in_ms(struct timespec *ts, long ms);

#endif