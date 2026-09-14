#ifndef FUJINET_NIO_PACKET_BACKEND_H
#define FUJINET_NIO_PACKET_BACKEND_H

#include <stdint.h>

/* Whole raw FujiBus packets, never SLIP. Callbacks borrow buffers only until
 * return and must obey capacity even when reporting an oversized packet.
 * REJECTED proves nothing was sent. COMPLETE proves this exchange finished
 * and cannot execute again or deliver another response. Everything else is
 * UNKNOWN, including partial/local acceptance. No automatic recovery. */
typedef enum {
    FN_PACKET_REJECTED,
    FN_PACKET_COMPLETE,
    FN_PACKET_UNKNOWN
} fn_packet_outcome_t;

typedef struct {
    uint8_t (*open)(void *context);
    void (*close)(void *context);
    fn_packet_outcome_t (*transfer)(void *context,
        const uint8_t *request, uint16_t request_length,
        uint8_t *response, uint16_t capacity, uint16_t *length);
    uint8_t (*local_reset)(void *context);
    /* FN_OK is proof: no prior work can execute and no old reply can arrive.
     * This must be an independent completion barrier, not a local clear. */
    uint8_t (*quiesce)(void *context);
} fn_packet_io_t;

typedef struct {
    fn_packet_io_t io;
    void *context;
    uint8_t *scratch;
    uint16_t capacity;
    uint8_t opened;
    uint8_t active;
    uint8_t quarantined;
} fn_packet_backend_t;

/* Initialize ONCE per remote endpoint lifetime, with exclusive scratch storage
 * of 6..65535 bytes, disjoint from requests and caller responses. Starts in
 * quarantine: even first use needs explicit proof. Never reinitialize to clear
 * uncertainty. Serialize all operations in the broker worker's ownership domain;
 * active also rejects callback reentry, it is not a thread synchronization lock. */
uint8_t fn_packet_backend_init(fn_packet_backend_t *backend,
    const fn_packet_io_t *io, void *context, uint8_t *scratch, uint16_t capacity);
uint8_t fn_packet_backend_open(fn_packet_backend_t *backend);
void fn_packet_backend_close(fn_packet_backend_t *backend);
/* Every local reset attempt quarantines, including success and failure from a
 * healthy state. Only explicit recover with proof can enable transmission.
 * Reset and recover are allowed both open and closed. The adapter must fail
 * when it cannot perform the operation/provide proof in that lifecycle state. */
uint8_t fn_packet_backend_reset(fn_packet_backend_t *backend);
uint8_t fn_packet_backend_recover(fn_packet_backend_t *backend);
uint8_t fn_packet_backend_exchange(fn_packet_backend_t *backend,
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity, uint16_t *response_length);

#endif
