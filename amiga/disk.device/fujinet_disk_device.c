#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/newstyle.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_disk_driver.h"
#include "fujinet_disk_device.h"

#define DEVICE_NAME FUJINET_DISK_DEVICE_NAME
#define DEVICE_VERSION 0
#define DEVICE_REVISION 1

static const UWORD supported_commands[] = {
    CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_START, CMD_STOP, CMD_FLUSH,
    ETD_READ, ETD_WRITE, ETD_UPDATE, ETD_CLEAR,
    TD_MOTOR, TD_SEEK, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_REMOVE, TD_ADDCHANGEINT, TD_REMCHANGEINT,
    TD_GETDRIVETYPE, TD_GETNUMTRACKS, TD_GETGEOMETRY,
    NSCMD_DEVICEQUERY, FUJINET_DISK_CMD_MOUNT,
    FUJINET_DISK_CMD_MOUNT_WRITABLE, 0
};

struct fujinet_disk_device_base {
    struct Device device;
    struct Unit unit;
    BPTR segment_list;
    fujinet_disk_driver_t driver;
    fujinet_nio_disk_context_t nio_context;
    struct SignalSemaphore io_lock;
    struct List io_queue;
    UBYTE io_processing;
    UBYTE io_stopped;
    struct IORequest *change_request;
    struct fujinet_disk_trace trace;
};

struct ExecBase *SysBase;

static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    DEVICE_NAME " 0.1 (10.8.2026)\r\n";

static BYTE result_to_io_error(uint8_t result)
{
    switch (result) {
    case FN_OK:
        return 0;
    case FN_ERR_INVALID:
        return IOERR_BADLENGTH;
    case FN_ERR_NOT_FOUND:
    case FN_ERR_NOT_READY:
        return TDERR_DiskChanged;
    default:
        return TDERR_NotSpecified;
    }
}

static void signal_media_change(struct fujinet_disk_device_base *base)
{
    struct IORequest *change = base->change_request;
    if (change == NULL) return;
    if (change->io_Command == TD_ADDCHANGEINT) {
        struct IOStdReq *io = (struct IOStdReq *)change;
        if (io->io_Data != NULL) Cause((struct Interrupt *)io->io_Data);
    } else {
        base->change_request = NULL;
        change->io_Error = 0;
        ReplyMsg(&change->io_Message);
    }
}

static struct fujinet_disk_device_base *device_init(
    register struct fujinet_disk_device_base *base __asm("d0"),
    register BPTR segment_list __asm("a0"),
    register struct ExecBase *sys_base __asm("a6"))
{
    SysBase = sys_base;
    base->segment_list = segment_list;
    InitSemaphore(&base->io_lock);
    base->io_queue.lh_Head = (struct Node *)&base->io_queue.lh_Tail;
    base->io_queue.lh_Tail = NULL;
    base->io_queue.lh_TailPred =
        (struct Node *)&base->io_queue.lh_Head;
    fujinet_nio_disk_context_init(&base->nio_context);
    fujinet_disk_driver_init(&base->driver, &fujinet_nio_disk_client,
                             &base->nio_context);
    return base;
}

static struct Device *device_open(
    register struct IORequest *request __asm("a1"),
    register ULONG unit_number __asm("d0"),
    register ULONG flags __asm("d1"),
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    uint8_t slot;

    (void)flags;
    if (fujinet_disk_unit_to_slot(unit_number, &slot) != FN_OK) {
        request->io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    request->io_Error = 0;
    request->io_Device = &base->device;
    request->io_Unit = &base->unit;
    ++base->device.dd_Library.lib_OpenCnt;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return &base->device;
}

static BPTR device_close(
    register struct IORequest *request __asm("a1"),
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    request->io_Device = NULL;
    request->io_Unit = NULL;
    if (base->device.dd_Library.lib_OpenCnt != 0) {
        --base->device.dd_Library.lib_OpenCnt;
    }
    return 0;
}

static BPTR device_expunge(
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    /* The serial-backed session is process-global today, so unloading is
     * deliberately deferred until Stage 7 defines explicit lifecycle. */
    base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
    return 0;
}

static ULONG device_reserved(void)
{
    return 0;
}

static void device_begin_io(
    register struct IORequest *request __asm("a1"),
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    struct IOStdReq *io = (struct IOStdReq *)request;
    uint8_t result;
    UWORD trace_index = FUJINET_DISK_TRACE_CAPACITY;

    Disable();
    if (base->io_processing) {
        if (request->io_Command == CMD_FLUSH) {
            while (!IsListEmpty(&base->io_queue)) {
                struct IORequest *queued =
                    (struct IORequest *)RemHead(&base->io_queue);
                queued->io_Error = IOERR_ABORTED;
                ReplyMsg(&queued->io_Message);
            }
            request->io_Error = 0;
            ((struct IOStdReq *)request)->io_Actual = 0;
            Enable();
            if ((request->io_Flags & IOF_QUICK) == 0)
                ReplyMsg(&request->io_Message);
            return;
        }
        request->io_Flags &= (UBYTE)~IOF_QUICK;
        AddTail(&base->io_queue, &request->io_Message.mn_Node);
        Enable();
        return;
    }
    base->io_processing = 1;
    Enable();

process_request:
    request->io_Error = 0;
    io->io_Actual = 0;

    if (request->io_Command != FUJINET_DISK_CMD_TRACE &&
        base->trace.count < FUJINET_DISK_TRACE_CAPACITY) {
        trace_index = base->trace.count++;
        base->trace.commands[trace_index] = request->io_Command;
        base->trace.offsets[trace_index] = io->io_Offset;
        base->trace.lengths[trace_index] = io->io_Length;
    }

    switch (request->io_Command) {
    case FUJINET_DISK_CMD_MOUNT:
    case FUJINET_DISK_CMD_MOUNT_WRITABLE:
        {
        uint32_t old_count = base->driver.change_count;
        if (request->io_Command == FUJINET_DISK_CMD_MOUNT_WRITABLE)
            result = fujinet_disk_mount_mode(&base->driver,
                                             FUJINET_DISK_UNIT_ZERO,
                                             (const char *)io->io_Data, 1);
        else
            result = fujinet_disk_mount(&base->driver,
                                        FUJINET_DISK_UNIT_ZERO,
                                        (const char *)io->io_Data);
        request->io_Error = result_to_io_error(result);
        if (base->driver.change_count != old_count) signal_media_change(base);
        }
        break;
    case FUJINET_DISK_CMD_TRACE:
        if (io->io_Data == NULL || io->io_Length < sizeof(base->trace)) {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            memcpy(io->io_Data, &base->trace, sizeof(base->trace));
            io->io_Actual = sizeof(base->trace);
        }
        break;
    case CMD_READ:
    case ETD_READ:
        if (!base->driver.mounted) {
            request->io_Error = TDERR_DiskChanged;
        } else if (io->io_Data == NULL || io->io_Offset >= FUJINET_ADF_BYTE_SIZE ||
                   io->io_Length > FUJINET_ADF_BYTE_SIZE - io->io_Offset) {
            request->io_Error = IOERR_BADADDRESS;
        } else if (io->io_Length == 0 ||
                   (io->io_Offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
                   (io->io_Length % FUJINET_DISK_BLOCK_SIZE) != 0) {
            request->io_Error = IOERR_BADLENGTH;
        } else if (request->io_Command == ETD_READ &&
                   ((struct IOExtTD *)request)->iotd_Count < base->driver.change_count) {
            request->io_Error = TDERR_DiskChanged;
        } else if (request->io_Command == ETD_READ &&
                   ((struct IOExtTD *)request)->iotd_SecLabel != NULL) {
            request->io_Error = IOERR_NOCMD;
        } else {
            result = fujinet_disk_read(&base->driver, FUJINET_DISK_UNIT_ZERO,
                                       io->io_Offset, (uint8_t *)io->io_Data,
                                       io->io_Length, &io->io_Actual);
            request->io_Error = result_to_io_error(result);
        }
        break;
    case CMD_WRITE:
    case ETD_WRITE:
        if (!base->driver.mounted) {
            request->io_Error = TDERR_DiskChanged;
        } else if (!base->driver.writable) {
            request->io_Error = TDERR_WriteProt;
        } else if (io->io_Data == NULL || io->io_Offset >= FUJINET_ADF_BYTE_SIZE ||
                   io->io_Length > FUJINET_ADF_BYTE_SIZE - io->io_Offset) {
            request->io_Error = IOERR_BADADDRESS;
        } else if (io->io_Length == 0 ||
                   (io->io_Offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
                   (io->io_Length % FUJINET_DISK_BLOCK_SIZE) != 0) {
            request->io_Error = IOERR_BADLENGTH;
        } else if (request->io_Command == ETD_WRITE &&
                   (((struct IOExtTD *)request)->iotd_Count <
                    base->driver.change_count)) {
            request->io_Error = TDERR_DiskChanged;
        } else if (request->io_Command == ETD_WRITE &&
                   ((struct IOExtTD *)request)->iotd_SecLabel != NULL) {
            request->io_Error = IOERR_NOCMD;
        } else {
            result = fujinet_disk_write(&base->driver,
                                        FUJINET_DISK_UNIT_ZERO,
                                        io->io_Offset,
                                        (const uint8_t *)io->io_Data,
                                        io->io_Length, &io->io_Actual);
            request->io_Error = result_to_io_error(result);
        }
        break;
    case CMD_RESET:
    case CMD_UPDATE:
    case ETD_UPDATE:
        if (request->io_Command == ETD_UPDATE &&
            ((struct IOExtTD *)request)->iotd_Count < base->driver.change_count)
            request->io_Error = TDERR_DiskChanged;
        else {
            result = fujinet_disk_flush(&base->driver, FUJINET_DISK_UNIT_ZERO);
            request->io_Error = result_to_io_error(result);
        }
        break;
    case CMD_CLEAR:
    case ETD_CLEAR:
        if (request->io_Command == ETD_CLEAR &&
            ((struct IOExtTD *)request)->iotd_Count < base->driver.change_count)
            request->io_Error = TDERR_DiskChanged;
        break;
    case CMD_START:
        base->io_stopped = 0;
        break;
    case CMD_STOP:
        base->io_stopped = 1;
        break;
    case CMD_FLUSH:
        while (!IsListEmpty(&base->io_queue)) {
            struct IORequest *queued =
                (struct IORequest *)RemHead(&base->io_queue);
            queued->io_Error = IOERR_ABORTED;
            ReplyMsg(&queued->io_Message);
        }
    case TD_MOTOR:
    case TD_SEEK:
        break;
    case TD_CHANGENUM:
        io->io_Actual = base->driver.change_count;
        break;
    case TD_CHANGESTATE:
        io->io_Actual = base->driver.mounted ? 0 : 1;
        break;
    case TD_PROTSTATUS:
        io->io_Actual = (!base->driver.mounted || !base->driver.writable) ? 1 : 0;
        break;
    case TD_EJECT:
        {
        uint32_t old_count = base->driver.change_count;
        result = fujinet_disk_eject(&base->driver, FUJINET_DISK_UNIT_ZERO);
        request->io_Error = result_to_io_error(result);
        if (base->driver.change_count != old_count) signal_media_change(base);
        }
        break;
    case TD_GETDRIVETYPE:
        io->io_Actual = DRIVE3_5;
        break;
    case TD_GETNUMTRACKS:
        io->io_Actual = 80;
        break;
    case TD_GETGEOMETRY:
        if (io->io_Data == NULL || io->io_Length < sizeof(struct DriveGeometry)) {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            struct DriveGeometry *geometry = (struct DriveGeometry *)io->io_Data;
            memset(geometry, 0, sizeof(*geometry));
            geometry->dg_SectorSize = FUJINET_DISK_BLOCK_SIZE;
            geometry->dg_TotalSectors = FUJINET_ADF_BLOCK_COUNT;
            geometry->dg_Cylinders = 80;
            geometry->dg_CylSectors = 22;
            geometry->dg_Heads = 2;
            geometry->dg_TrackSectors = 11;
            geometry->dg_BufMemType = MEMF_PUBLIC;
            geometry->dg_DeviceType = DG_DIRECT_ACCESS;
            geometry->dg_Flags = DGF_REMOVABLE;
            io->io_Actual = sizeof(*geometry);
        }
        break;
    case TD_ADDCHANGEINT:
    case TD_REMOVE:
        if (base->change_request != NULL) {
            request->io_Error = IOERR_UNITBUSY;
            break;
        }
        base->change_request = request;
        base->trace.actuals[trace_index] = io->io_Actual;
        base->trace.errors[trace_index] = request->io_Error;
        request->io_Flags &= (UBYTE)~IOF_QUICK;
        goto next_request;
    case TD_REMCHANGEINT:
        if (base->change_request != NULL) {
            base->change_request->io_Error = IOERR_ABORTED;
            ReplyMsg(&base->change_request->io_Message);
            base->change_request = NULL;
        }
        break;
    case NSCMD_DEVICEQUERY:
        if (io->io_Data == NULL ||
            io->io_Length < sizeof(struct NSDeviceQueryResult)) {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            struct NSDeviceQueryResult *query =
                (struct NSDeviceQueryResult *)io->io_Data;
            query->nsdqr_DevQueryFormat = 0;
            query->nsdqr_SizeAvailable = sizeof(*query);
            query->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
            query->nsdqr_DeviceSubType = 0;
            query->nsdqr_SupportedCommands = (APTR)supported_commands;
            io->io_Actual = sizeof(*query);
        }
        break;
    default:
        request->io_Error = IOERR_NOCMD;
        break;
    }

    if (trace_index < FUJINET_DISK_TRACE_CAPACITY) {
        base->trace.actuals[trace_index] = io->io_Actual;
        base->trace.errors[trace_index] = request->io_Error;
    }

    if ((request->io_Flags & IOF_QUICK) == 0) {
        ReplyMsg(&request->io_Message);
    }

next_request:
    Disable();
    if (base->io_stopped || IsListEmpty(&base->io_queue)) {
        base->io_processing = 0;
        Enable();
        return;
    }
    request = (struct IORequest *)RemHead(&base->io_queue);
    io = (struct IOStdReq *)request;
    trace_index = FUJINET_DISK_TRACE_CAPACITY;
    Enable();
    goto process_request;
}

static LONG device_abort_io(
    register struct IORequest *request __asm("a1"),
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    struct Node *node;
    Disable();
    if (request == base->change_request) {
        base->change_request = NULL;
        request->io_Error = IOERR_ABORTED;
        ReplyMsg(&request->io_Message);
        Enable();
        return 0;
    }
    for (node = base->io_queue.lh_Head; node->ln_Succ != NULL;
         node = node->ln_Succ) {
        if (node == &request->io_Message.mn_Node) {
            Remove(node);
            request->io_Error = IOERR_ABORTED;
            ReplyMsg(&request->io_Message);
            Enable();
            return 0;
        }
    }
    Enable();
    return IOERR_NOCMD;
}

static const APTR device_vectors[] = {
    (APTR)device_open,
    (APTR)device_close,
    (APTR)device_expunge,
    (APTR)device_reserved,
    (APTR)device_begin_io,
    (APTR)device_abort_io,
    (APTR)-1
};

static const ULONG device_init_table[] = {
    sizeof(struct fujinet_disk_device_base),
    (ULONG)device_vectors,
    0,
    (ULONG)device_init
};

static const char device_end;

const struct Resident device_resident __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&device_resident,
    (APTR)&device_end,
    RTF_AUTOINIT,
    DEVICE_VERSION,
    NT_DEVICE,
    0,
    (char *)device_name,
    (char *)device_id,
    (APTR)device_init_table
};

static const char device_end = 0;
