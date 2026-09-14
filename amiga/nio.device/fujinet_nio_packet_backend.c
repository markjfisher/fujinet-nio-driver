#include "fujinet_nio_packet_backend.h"
#include "fn_protocol.h"
#include <string.h>

/* Structural raw validation without process-global library parser state.
 * Descriptor codes encode 0,1,2,3,4 u8s, one/two u16s, or one u32.
 * Reserved descriptor bits retain the existing permissive codec behavior. */
static uint8_t valid_packet(const uint8_t *packet, uint16_t length)
{
    static const uint8_t sizes[8] = {0, 1, 2, 3, 4, 2, 4, 4};
    uint16_t offset = FN_HEADER_SIZE;
    uint16_t fields = 0;
    uint8_t descriptor;
    if (length < FN_HEADER_SIZE ||
        ((uint16_t)packet[2] | ((uint16_t)packet[3] << 8)) != length ||
        fn_calc_packet_checksum(packet, length) != packet[4]) return 0;
    descriptor = packet[5];
    for (;;) {
        uint8_t size = sizes[descriptor & 7];
        if (size > length - offset || fields > length - offset - size)
            return 0;
        fields = (uint16_t)(fields + size);
        if (!(descriptor & 0x80)) break;
        if (offset == length) return 0;
        descriptor = packet[offset++];
    }
    return fields <= length - offset;
}

uint8_t fn_packet_backend_init(fn_packet_backend_t *b,
    const fn_packet_io_t *io, void *context, uint8_t *scratch, uint16_t capacity)
{
    fn_packet_io_t saved_io;
    if (b == NULL || io == NULL || io->transfer == NULL ||
        scratch == NULL || capacity < FN_HEADER_SIZE) return FN_ERR_INVALID;
    saved_io = *io;
    memset(b, 0, sizeof(*b));
    b->io = saved_io;
    b->context = context;
    b->scratch = scratch;
    b->capacity = capacity;
    b->quarantined = 1;
    return FN_OK;
}

uint8_t fn_packet_backend_open(fn_packet_backend_t *b)
{
    uint8_t result;
    if (b == NULL) return FN_ERR_INVALID;
    if (b->active) return FN_ERR_BUSY;
    if (b->opened) return FN_OK;
    b->active = 1;
    result = b->io.open != NULL ? b->io.open(b->context) : FN_OK;
    b->active = 0;
    if (result == FN_OK) b->opened = 1;
    return result;
}

void fn_packet_backend_close(fn_packet_backend_t *b)
{
    if (b == NULL || b->active || !b->opened) return;
    b->active = 1;
    if (b->io.close != NULL) b->io.close(b->context);
    b->opened = 0;
    b->active = 0;
}

uint8_t fn_packet_backend_reset(fn_packet_backend_t *b)
{
    uint8_t result;
    if (b == NULL) return FN_ERR_INVALID;
    if (b->active) return FN_ERR_BUSY;
    b->active = 1;
    b->quarantined = 1;
    result = b->io.local_reset != NULL ? b->io.local_reset(b->context)
                                     : FN_ERR_TRANSPORT;
    b->active = 0;
    return result;
}

uint8_t fn_packet_backend_recover(fn_packet_backend_t *b)
{
    uint8_t result;
    if (b == NULL) return FN_ERR_INVALID;
    if (b->active) return FN_ERR_BUSY;
    b->active = 1;
    result = b->io.quiesce != NULL ? b->io.quiesce(b->context)
                                 : FN_ERR_TRANSPORT;
    b->active = 0;
    if (result == FN_OK) b->quarantined = 0;
    else b->quarantined = 1;
    return result;
}

uint8_t fn_packet_backend_exchange(fn_packet_backend_t *b,
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity, uint16_t *response_length)
{
    fn_packet_outcome_t outcome;
    uint16_t length = 0;
    if (response_length == NULL) return FN_ERR_INVALID;
    *response_length = 0;
    if (b == NULL) return FN_ERR_INVALID;
    if (b->active) return FN_ERR_BUSY;
    if (request == NULL || response == NULL || request_length > b->capacity ||
        !valid_packet(request, request_length)) return FN_ERR_INVALID;
    if (!b->opened || b->quarantined) return FN_ERR_TRANSPORT;
    b->active = 1;
    /* Mark uncertainty BEFORE giving the adapter any opportunity to send. */
    b->quarantined = 1;
    outcome = b->io.transfer(b->context, request, request_length,
                            b->scratch, b->capacity, &length);
    b->active = 0;
    if (outcome == FN_PACKET_REJECTED) {
        b->quarantined = 0;
        return FN_ERR_TRANSPORT;
    }
    if (outcome != FN_PACKET_COMPLETE || length > b->capacity ||
        length > response_capacity || !valid_packet(b->scratch, length) ||
        b->scratch[0] != request[0] || b->scratch[1] != request[1])
        return FN_ERR_TRANSPORT;
    memcpy(response, b->scratch, length);
    *response_length = length;
    b->quarantined = 0;
    return FN_OK;
}
