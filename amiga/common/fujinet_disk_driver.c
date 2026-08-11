#include "fujinet_disk_driver.h"

#include <string.h>

void fujinet_disk_driver_init(fujinet_disk_driver_t *driver,
                              const fujinet_disk_client_t *client,
                              void *client_context, uint8_t unit)
{
    if (driver == NULL) {
        return;
    }

    memset(driver, 0, sizeof(*driver));
    driver->client = client;
    driver->client_context = client_context;
    driver->unit = unit;
}

uint8_t fujinet_disk_unit_to_slot(uint32_t unit, uint8_t *slot)
{
    if (slot == NULL) {
        return FN_ERR_INVALID;
    }
    if (unit >= FUJINET_DISK_UNIT_COUNT) {
        return FN_ERR_NOT_FOUND;
    }
    *slot = (uint8_t)(FUJINET_DISK_FIRST_SLOT + unit);
    return FN_OK;
}

static uint8_t ensure_client(fujinet_disk_driver_t *driver)
{
    uint8_t result;

    if (driver == NULL || driver->client == NULL ||
        driver->client->init == NULL || driver->client->mount == NULL ||
        driver->client->info == NULL ||
        driver->client->read_sector == NULL ||
        driver->client->write_sector == NULL ||
        driver->client->flush == NULL || driver->client->unmount == NULL ||
        driver->client->clear_changed == NULL) {
        return FN_ERR_INVALID;
    }
    if (driver->client_initialized) {
        return FN_OK;
    }

    result = driver->client->init(driver->client_context);
    if (result == FN_OK) {
        driver->client_initialized = 1;
    }
    return result;
}

static uint8_t validate_standard_adf(const fn_disk_info_t *info,
                                     uint8_t writable, uint8_t slot)
{
    if (info == NULL ||
        (info->flags & FN_DISK_FLAG_MOUNTED) == 0 ||
        (writable && (info->flags & FN_DISK_FLAG_READONLY) != 0) ||
        info->slot != slot ||
        info->type != FN_DISK_TYPE_RAW ||
        info->sector_size != FUJINET_DISK_BLOCK_SIZE ||
        info->sector_count != FUJINET_ADF_BLOCK_COUNT) {
        return FN_ERR_INVALID;
    }
    return FN_OK;
}

uint8_t fujinet_disk_info(fujinet_disk_driver_t *driver, uint32_t unit,
                          fn_disk_info_t *info)
{
    uint8_t slot;
    uint8_t result;

    if (driver == NULL || info == NULL) {
        return FN_ERR_INVALID;
    }
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) {
        return result;
    }
    result = ensure_client(driver);
    if (result != FN_OK) {
        return result;
    }
    result = driver->client->info(driver->client_context, slot, info);
    if (result != FN_OK) {
        return result;
    }
    return validate_standard_adf(info, driver->writable, slot);
}

uint8_t fujinet_disk_mount(fujinet_disk_driver_t *driver, uint32_t unit,
                           const char *uri)
{ return fujinet_disk_mount_mode(driver, unit, uri, 0); }

uint8_t fujinet_disk_mount_mode(fujinet_disk_driver_t *driver, uint32_t unit,
                                const char *uri, uint8_t writable)
{
    uint8_t slot;
    uint8_t result;
    uint8_t replacing;

    if (uri == NULL || uri[0] == '\0') {
        return FN_ERR_INVALID;
    }

    replacing = driver != NULL && driver->mounted;
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) {
        return result;
    }
    result = ensure_client(driver);
    if (result != FN_OK) {
        return result;
    }

    result = driver->client->mount(
        driver->client_context, slot, uri, writable ? 0 : 1, FN_DISK_TYPE_AUTO,
        FUJINET_DISK_BLOCK_SIZE, &driver->media);
    if (result != FN_OK) {
        return result;
    }
    result = validate_standard_adf(&driver->media, writable, slot);
    if (result != FN_OK) {
        (void)driver->client->unmount(driver->client_context, slot);
        return result;
    }
    if (writable) {
        result = driver->client->flush(driver->client_context, slot);
        if (result != FN_OK) {
            (void)driver->client->unmount(driver->client_context, slot);
            return result;
        }
    }
    driver->writable = writable ? 1 : 0;
    result = fujinet_disk_info(driver, unit, &driver->media);
    if (result == FN_OK) {
        driver->mounted = 1;
        driver->change_count += replacing ? 2 : 1;
        if (driver->client->clear_changed(driver->client_context, slot) != FN_OK)
            driver->change_ack_pending = 1;
        else
            driver->change_ack_pending = 0;
    }
    return result;
}

uint8_t fujinet_disk_flush(fujinet_disk_driver_t *driver, uint32_t unit)
{
    uint8_t slot, result;
    if (driver == NULL || !driver->mounted) return FN_ERR_NOT_READY;
    result = ensure_client(driver);
    if (result != FN_OK) return result;
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) return result;
    return driver->client->flush(driver->client_context, slot);
}

uint8_t fujinet_disk_acknowledge_change(fujinet_disk_driver_t *driver,
                                        uint32_t unit)
{
    uint8_t slot, result;
    if (driver == NULL) return FN_ERR_INVALID;
    if (!driver->change_ack_pending) return FN_OK;
    result = ensure_client(driver);
    if (result != FN_OK) return result;
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) return result;
    result = driver->client->clear_changed(driver->client_context, slot);
    if (result == FN_OK) driver->change_ack_pending = 0;
    return result;
}

uint8_t fujinet_disk_eject(fujinet_disk_driver_t *driver, uint32_t unit)
{
    uint8_t slot, result;
    if (driver == NULL || !driver->mounted) return FN_ERR_NOT_READY;
    result = ensure_client(driver);
    if (result != FN_OK) return result;
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) return result;
    result = driver->client->unmount(driver->client_context, slot);
    if (result == FN_OK) {
        driver->mounted = driver->writable = 0;
        memset(&driver->media, 0, sizeof(driver->media));
        ++driver->change_count;
    }
    return result;
}

uint8_t fujinet_disk_write(fujinet_disk_driver_t *driver, uint32_t unit,
                           uint32_t byte_offset, const uint8_t *data,
                           uint32_t byte_length, uint32_t *actual)
{
    uint32_t lba, remaining;
    uint8_t slot, result;
    if (actual != NULL) *actual = 0;
    if (driver == NULL || data == NULL || actual == NULL || byte_length == 0 ||
        (byte_offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
        (byte_length % FUJINET_DISK_BLOCK_SIZE) != 0)
        return FN_ERR_INVALID;
    if (!driver->mounted) return FN_ERR_NOT_READY;
    if (!driver->writable) return FN_ERR_INVALID;
    if (byte_offset >= FUJINET_ADF_BYTE_SIZE ||
        byte_length > FUJINET_ADF_BYTE_SIZE - byte_offset) return FN_ERR_INVALID;
    result = ensure_client(driver);
    if (result != FN_OK) return result;
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) return result;
    lba = byte_offset / FUJINET_DISK_BLOCK_SIZE;
    remaining = byte_length;
    while (remaining != 0) {
        result = driver->client->write_sector(driver->client_context, slot,
                                               lba, data,
                                               FUJINET_DISK_BLOCK_SIZE);
        if (result != FN_OK) return result;
        data += FUJINET_DISK_BLOCK_SIZE;
        remaining -= FUJINET_DISK_BLOCK_SIZE;
        *actual += FUJINET_DISK_BLOCK_SIZE;
        ++lba;
    }
    return FN_OK;
}

uint8_t fujinet_disk_read(fujinet_disk_driver_t *driver, uint32_t unit,
                          uint32_t byte_offset, uint8_t *data,
                          uint32_t byte_length, uint32_t *actual)
{
    uint32_t remaining;
    uint32_t lba;
    uint8_t slot;
    uint8_t result;

    if (actual != NULL) {
        *actual = 0;
    }
    if (driver == NULL || data == NULL || actual == NULL ||
        byte_length == 0 ||
        (byte_offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
        (byte_length % FUJINET_DISK_BLOCK_SIZE) != 0 ||
        byte_offset >= FUJINET_ADF_BYTE_SIZE ||
        byte_length > FUJINET_ADF_BYTE_SIZE - byte_offset) {
        return FN_ERR_INVALID;
    }
    result = fujinet_disk_unit_to_slot(unit, &slot);
    if (result != FN_OK) {
        return result;
    }
    if (!driver->mounted) {
        return FN_ERR_NOT_READY;
    }
    result = ensure_client(driver);
    if (result != FN_OK) return result;

    lba = byte_offset / FUJINET_DISK_BLOCK_SIZE;
    remaining = byte_length;
    while (remaining != 0) {
        uint16_t received = 0;

        result = driver->client->read_sector(
            driver->client_context, slot, lba, data,
            FUJINET_DISK_BLOCK_SIZE, &received);
        if (result != FN_OK) {
            return result;
        }
        if (received != FUJINET_DISK_BLOCK_SIZE) {
            return FN_ERR_IO;
        }

        data += FUJINET_DISK_BLOCK_SIZE;
        remaining -= FUJINET_DISK_BLOCK_SIZE;
        *actual += FUJINET_DISK_BLOCK_SIZE;
        ++lba;
    }
    return FN_OK;
}
