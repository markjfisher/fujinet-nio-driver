#ifndef FUJINET_AMIGA_DISK_DEVICE_H
#define FUJINET_AMIGA_DISK_DEVICE_H

#include <exec/io.h>
#include "fujinet-nio.h"

#define FUJINET_DISK_DEVICE_NAME "fujinet-disk.device"
#define FUJINET_DISK_MAX_UNIT 7
#define FUJINET_DISK_CMD_MOUNT (CMD_NONSTD + 0x100)
#define FUJINET_DISK_CMD_TRACE (CMD_NONSTD + 0x101)
#define FUJINET_DISK_CMD_MOUNT_WRITABLE (CMD_NONSTD + 0x102)
#define FUJINET_DISK_CMD_MOUNT_CATALOG (CMD_NONSTD + 0x103)
#define FUJINET_DISK_CMD_INSPECT_CATALOG (CMD_NONSTD + 0x104)

struct fujinet_disk_catalog_mount { UBYTE catalog_slot; UBYTE writable; };
struct fujinet_disk_catalog_inspection { UBYTE catalog_slot; fn_disk_inspection_t inspection; };

#endif
