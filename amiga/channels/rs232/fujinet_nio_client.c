#include "fujinet_disk_driver.h"
#include "fn_platform.h"

static uint8_t nio_exchange(void *exchange_context,
                            const uint8_t *request, uint16_t request_length,
                            uint8_t *response, uint16_t response_capacity,
                            uint16_t *response_length)
{
    (void)exchange_context;
    return fn_transport_exchange_buffers(request, request_length, response,
                                         response_capacity, response_length);
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

const fujinet_disk_client_t fujinet_nio_disk_client = {
    nio_init,
    nio_mount,
    nio_info,
    nio_read_sector
};
