#ifndef FUJINET_DISK_FILESYSTEM_H
#define FUJINET_DISK_FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "fujinet-nio.h"
#include <fujinet-amiga-disk/support.h>

/* The first longword of an Amiga boot block is the big-endian DosType. */
#define FUJINET_AMIGA_BOOT_DOSTYPE_BYTES 4U

/* Returns a recognized supported DosType from the boot-block identifier. This does not by itself establish that the complete filesystem is valid. */
uint8_t fujinet_disk_classify_filesystem(const uint8_t *boot_block,
                                          size_t boot_block_length,
                                          uint32_t *dos_type);

#endif
