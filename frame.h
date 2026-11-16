#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define FRAME_HEADER_KI_LENGTH 17

struct mera_frame_header
{
    uint16_t length;
    uint8_t  src[6];
    uint8_t  dst[6];
    uint16_t vlan;
    uint8_t  priority;
};

void frame_serialize(const struct mera_frame_header *h, uint8_t buf[FRAME_HEADER_KI_LENGTH]);
void frame_deserialize(const uint8_t buf[FRAME_HEADER_KI_LENGTH], struct mera_frame_header *h);

#endif