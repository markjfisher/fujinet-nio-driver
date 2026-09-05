#ifndef FUJINET_NIO_CHECKSUM_H
#define FUJINET_NIO_CHECKSUM_H

#include <stdbool.h>
#include <stdint.h>

static inline uint8_t packet_checksum(const uint8_t *data, uint16_t length,
                                      bool skip_checksum_byte)
{
    uint16_t checksum = 0;
    uint16_t i;

    for (i = 0; i < length; ++i) {
        if (skip_checksum_byte && i == 4) continue;
        checksum = (uint16_t)(checksum + data[i]);
        checksum = (uint16_t)((checksum >> 8) + (checksum & 0xFFu));
    }
    return (uint8_t)checksum;
}

#endif
