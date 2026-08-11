#ifndef FUJINET_DISK_DRIVER_H
#define FUJINET_DISK_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "fujinet-nio.h"

#define FUJINET_DISK_UNIT_COUNT 8U
#define FUJINET_DISK_UNIT_ZERO 0U
#define FUJINET_DISK_FIRST_SLOT 1U
#define FUJINET_DISK_BLOCK_SIZE 512U
#define FUJINET_ADF_BLOCK_COUNT 1760UL
#define FUJINET_ADF_BYTE_SIZE \
    (FUJINET_DISK_BLOCK_SIZE * FUJINET_ADF_BLOCK_COUNT)

typedef struct fujinet_disk_client {
    uint8_t (*init)(void *context);
    uint8_t (*mount)(void *context, uint8_t slot, const char *uri,
                     uint8_t readonly, uint8_t type,
                     uint16_t sector_size_hint, fn_disk_info_t *info);
    uint8_t (*info)(void *context, uint8_t slot, fn_disk_info_t *info);
    uint8_t (*read_sector)(void *context, uint8_t slot, uint32_t lba,
                           uint8_t *data, uint16_t capacity,
                           uint16_t *length);
    uint8_t (*write_sector)(void *context, uint8_t slot, uint32_t lba,
                            const uint8_t *data, uint16_t length);
    uint8_t (*flush)(void *context, uint8_t slot);
    uint8_t (*unmount)(void *context, uint8_t slot);
    uint8_t (*clear_changed)(void *context, uint8_t slot);
} fujinet_disk_client_t;

typedef struct fujinet_disk_driver {
    const fujinet_disk_client_t *client;
    void *client_context;
    fn_disk_info_t media;
    uint8_t client_initialized;
    uint8_t mounted;
    uint8_t writable;
    uint8_t change_ack_pending;
    uint32_t change_count;
    uint8_t unit;
} fujinet_disk_driver_t;

typedef struct fujinet_nio_disk_context {
    fn_disk_client_context_t client;
} fujinet_nio_disk_context_t;

void fujinet_disk_driver_init(fujinet_disk_driver_t *driver,
                              const fujinet_disk_client_t *client,
                              void *client_context, uint8_t unit);
uint8_t fujinet_disk_unit_to_slot(uint32_t unit, uint8_t *slot);
uint8_t fujinet_disk_mount(fujinet_disk_driver_t *driver, uint32_t unit,
                           const char *uri);
uint8_t fujinet_disk_mount_mode(fujinet_disk_driver_t *driver, uint32_t unit,
                                const char *uri, uint8_t writable);
uint8_t fujinet_disk_eject(fujinet_disk_driver_t *driver, uint32_t unit);
uint8_t fujinet_disk_flush(fujinet_disk_driver_t *driver, uint32_t unit);
uint8_t fujinet_disk_write(fujinet_disk_driver_t *driver, uint32_t unit,
                           uint32_t byte_offset, const uint8_t *data,
                           uint32_t byte_length, uint32_t *actual);
uint8_t fujinet_disk_info(fujinet_disk_driver_t *driver, uint32_t unit,
                          fn_disk_info_t *info);
uint8_t fujinet_disk_read(fujinet_disk_driver_t *driver, uint32_t unit,
                          uint32_t byte_offset, uint8_t *data,
                          uint32_t byte_length, uint32_t *actual);

extern const fujinet_disk_client_t fujinet_nio_disk_client;
uint8_t fujinet_nio_disk_context_init(fujinet_nio_disk_context_t *context);

#endif
