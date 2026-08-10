#include "fujinet_disk_driver.h"

static uint8_t nio_init(void *context)
{
    (void)context;
    return fn_init();
}

static uint8_t nio_mount(void *context, uint8_t slot, const char *uri,
                         uint8_t readonly, uint8_t type,
                         uint16_t sector_size_hint, fn_disk_info_t *info)
{
    (void)context;
    return fn_disk_mount(slot, uri, readonly, type, sector_size_hint, info);
}

static uint8_t nio_read_sector(void *context, uint8_t slot, uint32_t lba,
                               uint8_t *data, uint16_t capacity,
                               uint16_t *length)
{
    (void)context;
    return fn_disk_read_sector(slot, lba, data, capacity, length);
}

static uint8_t nio_info(void *context, uint8_t slot, fn_disk_info_t *info)
{
    (void)context;
    return fn_disk_info(slot, info);
}

const fujinet_disk_client_t fujinet_nio_disk_client = {
    nio_init,
    nio_mount,
    nio_info,
    nio_read_sector
};
