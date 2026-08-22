#ifndef FUJINET_NIO_BACKEND_H
#define FUJINET_NIO_BACKEND_H

#include <stdint.h>

uint8_t backend_open(void);
void backend_close(void);
uint8_t backend_exchange(
    const uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_len);

#endif
