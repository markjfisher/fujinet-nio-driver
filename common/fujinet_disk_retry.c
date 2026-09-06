#include "fujinet_disk_retry.h"

#include "fujinet-nio.h"
#include "fn_protocol.h"

enum {
    NIO_DISK_READ_SECTOR = 0x03,
    NIO_DISK_WRITE_SECTOR = 0x04,
    NIO_DISK_READ_REQUEST_SIZE = FN_HEADER_SIZE + 8,
    NIO_DISK_WRITE_REQUEST_SIZE = FN_HEADER_SIZE + 8 + 512
};

static uint8_t is_retryable_sector_request(const uint8_t *request,
                                            uint16_t request_length)
{
    uint16_t encoded_length;
    uint16_t sector_length;
    uint8_t command;
    uint8_t slot;

    if (request == NULL || request_length < FN_HEADER_SIZE) return 0;
    command = request[1];
    if ((command == NIO_DISK_READ_SECTOR &&
         request_length != NIO_DISK_READ_REQUEST_SIZE) ||
        (command == NIO_DISK_WRITE_SECTOR &&
         request_length != NIO_DISK_WRITE_REQUEST_SIZE) ||
        (command != NIO_DISK_READ_SECTOR && command != NIO_DISK_WRITE_SECTOR))
        return 0;

    encoded_length = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
    sector_length = (uint16_t)request[12] | ((uint16_t)request[13] << 8);
    slot = request[7];
    return request[0] == FN_DEVICE_DISK && encoded_length == request_length &&
           request[FN_CHECKSUM_OFFSET] ==
               fn_calc_packet_checksum(request, request_length) &&
           request[5] == 0 && request[6] == FN_DISK_PROTOCOL_VERSION &&
           slot >= 1 && slot <= 8 && sector_length == 512;
}

uint8_t fujinet_disk_retry_exchange(
    fujinet_disk_retry_transport_fn transport, void *transport_context,
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity,
    uint16_t *response_length, fujinet_disk_retry_diagnostics_t *diagnostics,
    fujinet_disk_retry_attempt_fn attempt_observer, void *observer_context)
{
    uint16_t attempt_response_length;
    uint8_t attempt;
    uint8_t attempts;
    uint8_t result = FN_ERR_INVALID;

    if (response_length == NULL || transport == NULL) return FN_ERR_INVALID;
    attempts = is_retryable_sector_request(request, request_length)
                   ? FUJINET_DISK_RETRY_ATTEMPTS : 1;
    if (diagnostics != NULL) diagnostics->attempts = 0;

    for (attempt = 0; attempt < attempts; ++attempt) {
        if (diagnostics != NULL) {
            diagnostics->attempts = (uint8_t)(attempt + 1);
            diagnostics->results[attempt] = FN_ERR_INVALID;
            diagnostics->response_lengths[attempt] = 0;
        }
        *response_length = 0;
        attempt_response_length = 0;
        result = transport(transport_context, request, request_length,
                            response, response_capacity,
                            &attempt_response_length);
        if (diagnostics != NULL) {
            diagnostics->results[attempt] = result;
            diagnostics->response_lengths[attempt] = attempt_response_length;
        }
        if (attempt_observer != NULL)
            attempt_observer(observer_context, (uint8_t)(attempt + 1), result,
                             attempt_response_length);
        if (result == FN_OK) {
            *response_length = attempt_response_length;
            return FN_OK;
        }
        if (result != FN_ERR_TRANSPORT && result != FN_ERR_TIMEOUT)
            return result;
    }
    return result;
}
