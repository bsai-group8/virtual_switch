#include "frame.h"
#include <arpa/inet.h>
#include <string.h>

void frame_serialize(const struct mera_frame_header *h, uint8_t buf[FRAME_HEADER_KI_LENGTH])
{
    uint16_t n;
    n = htons(h->length);
    memcpy(buf, &n, 2);
    memcpy(buf + 2, h->src, 6);
    memcpy(buf + 8, h->dst, 6);
    n = htons(h->vlan);
    memcpy(buf + 14, &n, 2);
    buf[16] = h->priority;
}
void frame_deserialize(const uint8_t buf[FRAME_HEADER_KI_LENGTH], struct mera_frame_header *h)
{
    uint16_t n;
    memcpy(&n, buf, 2);
    h->length = ntohs(n);
    memcpy(h->src, buf + 2, 6);
    memcpy(h->dst, buf + 8, 6);
    memcpy(&n, buf + 14, 2);
    h->vlan = ntohs(n);
    h->priority = buf[16];
}