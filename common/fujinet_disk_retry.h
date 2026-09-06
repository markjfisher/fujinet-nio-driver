#ifndef FUJINET_DISK_RETRY_H
#define FUJINET_DISK_RETRY_H

#include <stdint.h>

#define FUJINET_DISK_RETRY_ATTEMPTS 3

typedef uint8_t (*fujinet_disk_retry_transport_fn)(
    void *context, const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity,
    uint16_t *response_length);

typedef void (*fujinet_disk_retry_attempt_fn)(
    void *context, uint8_t attempt, uint8_t result,
    uint16_t response_length);

typedef struct fujinet_disk_retry_diagnostics {
    uint8_t attempts;
    uint8_t results[FUJINET_DISK_RETRY_ATTEMPTS];
    uint16_t response_lengths[FUJINET_DISK_RETRY_ATTEMPTS];
} fujinet_disk_retry_diagnostics_t;

uint8_t fujinet_disk_retry_exchange(
    fujinet_disk_retry_transport_fn transport, void *transport_context,
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity,
    uint16_t *response_length, fujinet_disk_retry_diagnostics_t *diagnostics,
    fujinet_disk_retry_attempt_fn attempt_observer, void *observer_context);

#endif
