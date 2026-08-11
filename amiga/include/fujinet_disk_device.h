#ifndef FUJINET_DISK_DEVICE_H
#define FUJINET_DISK_DEVICE_H

#include <exec/io.h>

/* Keep private commands beyond the complete trackdisk.device command range.
 * CMD_NONSTD itself is TD_MOTOR and must never be repurposed. */
#define FUJINET_DISK_CMD_MOUNT (CMD_NONSTD + 0x100)
#define FUJINET_DISK_CMD_TRACE (CMD_NONSTD + 0x101)
#define FUJINET_DISK_CMD_MOUNT_WRITABLE (CMD_NONSTD + 0x102)
#define FUJINET_DISK_TRACE_CAPACITY 32

struct fujinet_disk_trace {
    UWORD count;
    UWORD commands[FUJINET_DISK_TRACE_CAPACITY];
    ULONG offsets[FUJINET_DISK_TRACE_CAPACITY];
    ULONG lengths[FUJINET_DISK_TRACE_CAPACITY];
    ULONG actuals[FUJINET_DISK_TRACE_CAPACITY];
    BYTE errors[FUJINET_DISK_TRACE_CAPACITY];
};

#define FUJINET_DISK_DEVICE_NAME "fujinet-disk.device"

#endif
