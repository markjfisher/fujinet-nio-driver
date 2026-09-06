#ifndef FUJINET_DISK_DEVICE_H
#define FUJINET_DISK_DEVICE_H

#include <exec/io.h>
#include "fujinet-nio.h"
#include "../../common/fujinet_disk_retry.h"

/* Keep private commands beyond the complete trackdisk.device command range.
 * CMD_NONSTD itself is TD_MOTOR and must never be repurposed. */
#define FUJINET_DISK_CMD_MOUNT (CMD_NONSTD + 0x100)
#define FUJINET_DISK_CMD_TRACE (CMD_NONSTD + 0x101)
#define FUJINET_DISK_CMD_MOUNT_WRITABLE (CMD_NONSTD + 0x102)
#define FUJINET_DISK_CMD_MOUNT_CATALOG (CMD_NONSTD + 0x103)
#define FUJINET_DISK_CMD_INSPECT_CATALOG (CMD_NONSTD + 0x104)
#define FUJINET_DISK_CMD_TRACE_CLEAR (CMD_NONSTD + 0x105)
#define FUJINET_DISK_TRACE_CAPACITY 32
#define FUJINET_DISK_TRACE_ATTEMPTS FUJINET_DISK_RETRY_ATTEMPTS

struct fujinet_disk_trace {
    UWORD count;
    UWORD commands[FUJINET_DISK_TRACE_CAPACITY];
    ULONG offsets[FUJINET_DISK_TRACE_CAPACITY];
    ULONG lengths[FUJINET_DISK_TRACE_CAPACITY];
    ULONG actuals[FUJINET_DISK_TRACE_CAPACITY];
    BYTE errors[FUJINET_DISK_TRACE_CAPACITY];
    UBYTE exchange_attempts[FUJINET_DISK_TRACE_CAPACITY];
    UBYTE exchange_results[FUJINET_DISK_TRACE_CAPACITY][FUJINET_DISK_TRACE_ATTEMPTS];
    UBYTE exchange_causes[FUJINET_DISK_TRACE_CAPACITY][FUJINET_DISK_TRACE_ATTEMPTS];
    UBYTE exchange_native_errors[FUJINET_DISK_TRACE_CAPACITY][FUJINET_DISK_TRACE_ATTEMPTS];
    UWORD exchange_statuses[FUJINET_DISK_TRACE_CAPACITY][FUJINET_DISK_TRACE_ATTEMPTS];
    UWORD exchange_response_lengths[FUJINET_DISK_TRACE_CAPACITY][FUJINET_DISK_TRACE_ATTEMPTS];
};

struct fujinet_disk_catalog_mount {
    UBYTE catalog_slot;
    UBYTE writable;
};

struct fujinet_disk_catalog_inspection {
    UBYTE catalog_slot;
    fn_disk_inspection_t inspection;
};

#define FUJINET_DISK_DEVICE_NAME "fujinet-disk.device"

#endif
