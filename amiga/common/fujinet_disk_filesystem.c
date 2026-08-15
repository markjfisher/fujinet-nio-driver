#include "fujinet_disk_filesystem.h"

static uint32_t get_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

uint8_t fujinet_disk_classify_filesystem(const uint8_t *boot_block,
                                          size_t boot_block_length,
                                          uint32_t *dos_type)
{
    uint32_t value;

    if (boot_block == NULL || dos_type == NULL ||
        boot_block_length < FUJINET_AMIGA_BOOT_DOSTYPE_BYTES) {
        return FN_ERR_INVALID;
    }

    value = get_be32(boot_block);
    if (value != FUJINET_AMIGA_DOS_OFS && value != FUJINET_AMIGA_DOS_FFS) {
        return FN_ERR_INVALID;
    }

    *dos_type = value;
    return FN_OK;
}
