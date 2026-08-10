#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "fujinet_disk_driver.h"

#define DEVICE_NAME "fujinet-disk.device"
#define DEVICE_VERSION 0
#define DEVICE_REVISION 1

/* io_Data points to a NUL-terminated image URI and io_Length is ignored. */
#define FUJINET_DISK_CMD_MOUNT CMD_NONSTD

struct fujinet_disk_device_base {
    struct Device device;
    struct Unit unit;
    BPTR segment_list;
    fujinet_disk_driver_t driver;
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
        return IOERR_OPENFAIL;
    default:
        return IOERR_ABORTED;
    }
}

static struct fujinet_disk_device_base *device_init(
    register struct fujinet_disk_device_base *base __asm("d0"),
    register BPTR segment_list __asm("a0"),
    register struct ExecBase *sys_base __asm("a6"))
{
    SysBase = sys_base;
    base->segment_list = segment_list;
    fujinet_disk_driver_init(&base->driver, &fujinet_nio_disk_client, NULL);
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

    request->io_Error = 0;
    io->io_Actual = 0;

    switch (request->io_Command) {
    case FUJINET_DISK_CMD_MOUNT:
        result = fujinet_disk_mount(&base->driver, FUJINET_DISK_UNIT_ZERO,
                                    (const char *)io->io_Data);
        request->io_Error = result_to_io_error(result);
        break;
    case CMD_READ:
        result = fujinet_disk_read(&base->driver, FUJINET_DISK_UNIT_ZERO,
                                   io->io_Offset, (uint8_t *)io->io_Data,
                                   io->io_Length, &io->io_Actual);
        request->io_Error = result_to_io_error(result);
        break;
    case CMD_WRITE:
        request->io_Error = IOERR_NOCMD;
        break;
    default:
        request->io_Error = IOERR_NOCMD;
        break;
    }

    if ((request->io_Flags & IOF_QUICK) == 0) {
        ReplyMsg(&request->io_Message);
    }
}

static LONG device_abort_io(
    register struct IORequest *request __asm("a1"),
    register struct fujinet_disk_device_base *base __asm("a6"))
{
    (void)request;
    (void)base;
    /* BeginIO is synchronous in this first single-request implementation. */
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
