#include "fujinet_disk_driver.h"
#include "fujinet_nio_endian.h"
#include "fn_platform.h"
#include "fn_protocol.h"

enum {
    NIO_DISK_READ_SECTOR = 0x03,
    NIO_DISK_WRITE_SECTOR = 0x04,
    NIO_DISK_READ_REQUEST_SIZE = FN_HEADER_SIZE + 8,
    NIO_DISK_WRITE_REQUEST_SIZE = FN_HEADER_SIZE + 8 + FUJINET_DISK_BLOCK_SIZE,
    NIO_DISK_EXCHANGE_ATTEMPTS = 3
};

static uint8_t retry_request_checksum(const uint8_t *request,
                                      uint16_t request_length)
{
    uint16_t checksum = 0;
    uint16_t i;

    for (i = 0; i < request_length; ++i) {
        if (i != 4) {
            checksum += request[i];
            checksum = (checksum >> 8) + (checksum & 0xFFu);
        }
    }
    return (uint8_t)checksum;
}

static uint8_t is_retryable_sector_request(const uint8_t *request,
                                            uint16_t request_length)
{
    uint16_t encoded_length;
    uint16_t sector_length;
    uint8_t command;
    uint8_t slot;

    if (request == NULL || request_length < FN_HEADER_SIZE)
        return 0;

    command = request[1];
    if ((command == NIO_DISK_READ_SECTOR &&
         request_length != NIO_DISK_READ_REQUEST_SIZE) ||
        (command == NIO_DISK_WRITE_SECTOR &&
         request_length != NIO_DISK_WRITE_REQUEST_SIZE) ||
        (command != NIO_DISK_READ_SECTOR &&
         command != NIO_DISK_WRITE_SECTOR))
        return 0;

    encoded_length = fujinet_nio_get_le16(request + 2);
    sector_length = fujinet_nio_get_le16(request + 12);
    slot = request[7];

    return request[0] == FN_DEVICE_DISK &&
           encoded_length == request_length &&
           request[4] == retry_request_checksum(request, request_length) &&
           request[5] == 0 &&
           request[6] == FN_DISK_PROTOCOL_VERSION &&
           slot >= FUJINET_DISK_FIRST_SLOT &&
           slot < FUJINET_DISK_FIRST_SLOT + FUJINET_DISK_UNIT_COUNT &&
           sector_length == FUJINET_DISK_BLOCK_SIZE;
}

static uint8_t nio_exchange(void *exchange_context,
                            const uint8_t *request, uint16_t request_length,
                            uint8_t *response, uint16_t response_capacity,
                            uint16_t *response_length)
{
    uint16_t attempt_response_length;
    uint8_t attempt;
    uint8_t attempts;
    uint8_t result = FN_ERR_INVALID;

    (void)exchange_context;
    if (response_length == NULL) return FN_ERR_INVALID;

    attempts = is_retryable_sector_request(request, request_length)
                   ? NIO_DISK_EXCHANGE_ATTEMPTS
                   : 1;

    for (attempt = 0; attempt < attempts; ++attempt) {
        *response_length = 0;
        attempt_response_length = 0;
        result = fn_transport_exchange_buffers(
            request, request_length, response, response_capacity,
            &attempt_response_length);
        if (result == FN_OK) {
            *response_length = attempt_response_length;
            return FN_OK;
        }
        if (result != FN_ERR_TRANSPORT && result != FN_ERR_TIMEOUT)
            return result;
    }

    return result;
}

uint8_t fujinet_nio_disk_context_init(fujinet_nio_disk_context_t *context)
{
    if (context == NULL) return FN_ERR_INVALID;
    return fn_disk_context_init(&context->client, nio_exchange, NULL);
}

static uint8_t nio_init(void *context)
{
    (void)context;
    return fn_init();
}

static uint8_t nio_mount(void *context, uint8_t slot, const char *uri,
                         uint8_t readonly, uint8_t type,
                         uint16_t sector_size_hint, fn_disk_info_t *info)
{
    fujinet_nio_disk_context_t *nio = context;
    if (nio == NULL) return FN_ERR_INVALID;
    return fn_disk_mount_context(&nio->client, slot, uri, readonly, type,
                                 sector_size_hint, info);
}

static uint8_t nio_read_sector(void *context, uint8_t slot, uint32_t lba,
                               uint8_t *data, uint16_t capacity,
                               uint16_t *length)
{
    fujinet_nio_disk_context_t *nio = context;
    if (nio == NULL) return FN_ERR_INVALID;
    return fn_disk_read_sector_context(&nio->client, slot, lba, data,
                                       capacity, length);
}

static uint8_t nio_info(void *context, uint8_t slot, fn_disk_info_t *info)
{
    fujinet_nio_disk_context_t *nio = context;
    if (nio == NULL) return FN_ERR_INVALID;
    return fn_disk_info_context(&nio->client, slot, info);
}

static uint8_t nio_write(void *context, uint8_t slot, uint32_t lba,
                         const uint8_t *data, uint16_t length)
{ fujinet_nio_disk_context_t *nio = context; return nio ? fn_disk_write_sector_context(&nio->client, slot, lba, data, length) : FN_ERR_INVALID; }
static uint8_t nio_flush(void *context, uint8_t slot)
{ fujinet_nio_disk_context_t *nio = context; return nio ? fn_disk_flush_context(&nio->client, slot) : FN_ERR_INVALID; }
static uint8_t nio_unmount(void *context, uint8_t slot)
{ fujinet_nio_disk_context_t *nio = context; return nio ? fn_disk_unmount_context(&nio->client, slot) : FN_ERR_INVALID; }
static uint8_t nio_clear_changed(void *context, uint8_t slot)
{ fujinet_nio_disk_context_t *nio = context; return nio ? fn_disk_clear_changed_context(&nio->client, slot) : FN_ERR_INVALID; }
static uint8_t nio_inspect(void *context, const char *uri, fn_disk_inspection_t *inspection)
{ fujinet_nio_disk_context_t *nio = context; return nio ? fn_disk_inspect_context(&nio->client, uri, FN_DISK_TYPE_AUTO, 0, inspection) : FN_ERR_INVALID; }

const fujinet_disk_client_t fujinet_nio_disk_client = {
    nio_init,
    nio_mount,
    nio_info,
    nio_read_sector,
    nio_write,
    nio_flush,
    nio_unmount,
    nio_clear_changed,
    nio_inspect
};
