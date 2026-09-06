#include <devices/serial.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/types.h>
#include <hardware/custom.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_paula_uart.h"
#include "fujinet_serial_device.h"

#define FN_REGISTER(name) __asm(name)

#define DEVICE_NAME FUJINET_SERIAL_DEVICE_NAME
#define DEVICE_VERSION 0
#define DEVICE_BAUD_DEFAULT 19200UL
#define DEVICE_BAUD_MIN 300UL
#define DEVICE_BAUD_MAX 230400UL
#define TBE_SPIN_MAX 2000000UL

#define paula (*(volatile struct Custom *)0xdff000)

struct fujinet_serial_base {
    struct Device device;
    BPTR segment_list;
    struct Unit exec_unit;
    struct Interrupt rbf_int;
    uint8_t rx_buf[FUJINET_PAULA_RX_DEFAULT_SIZE];
    fujinet_paula_rx_t rx;
    uint32_t baud;
    struct Interrupt *old_rbf;
    UBYTE rbf_added;
    UBYTE rbf_was_enabled;
    UBYTE tbe_was_enabled;
    UBYTE paula_claimed;
};

struct ExecBase *SysBase;

static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    "$VER: " DEVICE_NAME " 0.1 (7.9.2026) \xa9 2026 Mark Fisher\r\n";

static int pal_display(void)
{
    return SysBase != NULL && SysBase->VBlankFrequency == 50U;
}

static void apply_serper(struct fujinet_serial_base *base)
{
    paula.serper = fujinet_paula_serper(base->baud, pal_display());
}

static void drain_rbf_locked(struct fujinet_serial_base *base)
{
    UWORD serdatr = paula.serdatr;

    if ((serdatr & (FUJINET_PAULA_SERDATR_RBF | FUJINET_PAULA_SERDATR_OVRUN)) == 0)
        return;
    paula.intreq = (UWORD)INTF_RBF;
    fujinet_paula_rx_ingest(&base->rx, serdatr);
}

static void drain_rbf(struct fujinet_serial_base *base)
{
    Disable();
    drain_rbf_locked(base);
    Enable();
}

static void rbf_server(
    register struct fujinet_serial_base *base FN_REGISTER("a1"))
{
    UWORD serdatr;
    uint16_t next;
    fujinet_paula_rx_t *rx;

    /* Exec servers may scratch D0-D1/A0-A1 only. No C helper calls. */
    __asm volatile("movem.l d2-d7/a2-a6,-(%%sp)" ::: "memory");
    if (base == NULL) goto out;
    serdatr = paula.serdatr;
    if ((serdatr & (FUJINET_PAULA_SERDATR_RBF | FUJINET_PAULA_SERDATR_OVRUN)) == 0)
        goto out;
    paula.intreq = (UWORD)INTF_RBF;
    rx = &base->rx;
    if (serdatr & FUJINET_PAULA_SERDATR_OVRUN) rx->overrun = 1;
    if ((serdatr & FUJINET_PAULA_SERDATR_RBF) == 0) goto out;
    next = (uint16_t)((rx->head + 1U) & rx->mask);
    if (next == rx->tail) {
        rx->overrun = 1;
        goto out;
    }
    rx->buf[rx->head] = (uint8_t)serdatr;
    rx->head = next;
out:
    __asm volatile("movem.l (%%sp)+,d2-d7/a2-a6" ::: "memory");
}

static void fill_open_defaults(struct IOExtSer *req, struct fujinet_serial_base *base)
{
    req->io_CtlChar = SER_DEFAULT_CTLCHAR;
    req->io_RBufLen = FUJINET_PAULA_RX_DEFAULT_SIZE;
    req->io_ExtFlags = 0;
    req->io_Baud = base->baud;
    req->io_BrkTime = 250000UL;
    req->io_TermArray.TermArray0 = 0;
    req->io_TermArray.TermArray1 = 0;
    req->io_ReadLen = 8;
    req->io_WriteLen = 8;
    req->io_StopBits = 1;
    req->io_SerFlags = SERF_XDISABLED;
    req->io_Status = 0;
}

static void finish(struct IOExtSer *req, BYTE err)
{
    req->IOSer.io_Error = err;
    if ((req->IOSer.io_Flags & IOF_QUICK) != 0) return;
    ReplyMsg(&req->IOSer.io_Message);
}

static void release_paula(struct fujinet_serial_base *base)
{
    if (!base->paula_claimed) return;
    paula.intena = (UWORD)(INTF_RBF | INTF_TBE);
    if (base->rbf_added) {
        SetIntVector(INTB_RBF, base->old_rbf);
        base->old_rbf = NULL;
        base->rbf_added = 0;
    }
    drain_rbf_locked(base);
    if (base->rbf_was_enabled)
        paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
    if (base->tbe_was_enabled)
        paula.intena = (UWORD)(INTF_SETCLR | INTF_TBE);
    base->rbf_was_enabled = 0;
    base->tbe_was_enabled = 0;
    base->paula_claimed = 0;
}

static BYTE claim_paula(struct fujinet_serial_base *base)
{
    if (fujinet_paula_rx_init(&base->rx, base->rx_buf,
                              FUJINET_PAULA_RX_DEFAULT_SIZE) != 0)
        return IOERR_OPENFAIL;
    base->baud = DEVICE_BAUD_DEFAULT;
    base->rbf_was_enabled =
        (UBYTE)((paula.intenar & (UWORD)INTF_RBF) != 0);
    base->tbe_was_enabled =
        (UBYTE)((paula.intenar & (UWORD)INTF_TBE) != 0);
    paula.intena = (UWORD)(INTF_RBF | INTF_TBE);
    paula.intreq = (UWORD)(INTF_RBF | INTF_TBE);
    apply_serper(base);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rx);

    memset(&base->rbf_int, 0, sizeof(base->rbf_int));
    base->rbf_int.is_Node.ln_Type = NT_INTERRUPT;
    base->rbf_int.is_Node.ln_Pri = 127;
    base->rbf_int.is_Node.ln_Name = (char *)device_name;
    base->rbf_int.is_Data = base;
    base->rbf_int.is_Code = (void (*)())rbf_server;
    base->old_rbf = SetIntVector(INTB_RBF, &base->rbf_int);
    base->rbf_added = 1;
    paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
    base->paula_claimed = 1;
    return 0;
}

static void cmd_query(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    drain_rbf(base);
    Disable();
    req->IOSer.io_Actual = fujinet_paula_rx_count(&base->rx);
    req->io_Status = base->rx.overrun ? (UWORD)IO_STATF_OVERRUN : 0;
    Enable();
    finish(req, 0);
}

static void cmd_read(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    ULONG length = req->IOSer.io_Length;
    uint8_t *data = (uint8_t *)req->IOSer.io_Data;
    uint16_t copied;

    drain_rbf(base);
    if (length != 0 && data == NULL) {
        req->IOSer.io_Actual = 0;
        finish(req, IOERR_BADADDRESS);
        return;
    }
    if (length > 0xFFFFUL) length = 0xFFFFUL;
    Disable();
    copied = fujinet_paula_rx_read(&base->rx, data, (uint16_t)length);
    if (copied != 0) base->rx.overrun = 0;
    req->io_Status = base->rx.overrun ? (UWORD)IO_STATF_OVERRUN : 0;
    Enable();
    req->IOSer.io_Actual = copied;
    finish(req, 0);
}

static void cmd_write(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    ULONG length = req->IOSer.io_Length;
    const uint8_t *data = (const uint8_t *)req->IOSer.io_Data;
    ULONG i;

    if (length != 0 && data == NULL) {
        req->IOSer.io_Actual = 0;
        finish(req, IOERR_BADADDRESS);
        return;
    }
    Forbid();
    for (i = 0; i < length; ++i) {
        ULONG spin = 0;

        while ((paula.serdatr & FUJINET_PAULA_SERDATR_TBE) == 0) {
            drain_rbf(base);
            if (++spin >= TBE_SPIN_MAX) {
                Permit();
                req->IOSer.io_Actual = i;
                finish(req, SerErr_LineErr);
                return;
            }
        }
        drain_rbf(base);
        paula.serdat = fujinet_paula_serdat_word(data[i]);
    }
    Permit();
    req->IOSer.io_Actual = length;
    finish(req, 0);
}

static void cmd_setparams(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    uint32_t baud = req->io_Baud;

    if (baud < DEVICE_BAUD_MIN || baud > DEVICE_BAUD_MAX) {
        finish(req, SerErr_BaudMismatch);
        return;
    }
    if (req->io_ReadLen != 8 || req->io_WriteLen != 8 || req->io_StopBits != 1) {
        finish(req, SerErr_InvParam);
        return;
    }
    if ((req->io_SerFlags & SERF_PARTY_ON) != 0) {
        finish(req, SerErr_InvParam);
        return;
    }
    base->baud = baud;
    apply_serper(base);
    req->io_RBufLen = FUJINET_PAULA_RX_DEFAULT_SIZE;
    req->io_SerFlags |= SERF_XDISABLED;
    finish(req, 0);
}

static void cmd_clear(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    Disable();
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rx);
    Enable();
    req->IOSer.io_Actual = 0;
    req->io_Status = 0;
    finish(req, 0);
}

static struct fujinet_serial_base *device_init(
    register struct fujinet_serial_base *base FN_REGISTER("d0"),
    register BPTR segment_list FN_REGISTER("a0"),
    register struct ExecBase *sys_base FN_REGISTER("a6"))
{
    SysBase = sys_base;
    base->segment_list = segment_list;
    base->baud = DEVICE_BAUD_DEFAULT;
    return base;
}

static struct Device *device_open(
    register struct IORequest *request FN_REGISTER("a1"),
    register ULONG unit_number FN_REGISTER("d0"),
    register ULONG flags FN_REGISTER("d1"),
    register struct fujinet_serial_base *base FN_REGISTER("a6"))
{
    struct IOExtSer *req = (struct IOExtSer *)request;

    (void)flags;
    if (unit_number != FUJINET_SERIAL_DEVICE_UNIT) {
        request->io_Error = IOERR_OPENFAIL;
        return NULL;
    }
    if (base->device.dd_Library.lib_OpenCnt != 0) {
        request->io_Error = IOERR_UNITBUSY;
        return NULL;
    }
    if (claim_paula(base) != 0) {
        request->io_Error = IOERR_OPENFAIL;
        return NULL;
    }
    fill_open_defaults(req, base);
    request->io_Error = 0;
    request->io_Device = &base->device;
    request->io_Unit = &base->exec_unit;
    ++base->device.dd_Library.lib_OpenCnt;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return &base->device;
}

static BPTR device_expunge(
    register struct fujinet_serial_base *base FN_REGISTER("a6"));

static BPTR device_close(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_serial_base *base FN_REGISTER("a6"))
{
    uint8_t delayed_expunge;

    request->io_Device = NULL;
    request->io_Unit = NULL;
    if (base->device.dd_Library.lib_OpenCnt != 0)
        --base->device.dd_Library.lib_OpenCnt;
    if (base->device.dd_Library.lib_OpenCnt == 0) release_paula(base);
    delayed_expunge =
        (uint8_t)(base->device.dd_Library.lib_OpenCnt == 0 &&
                  (base->device.dd_Library.lib_Flags & LIBF_DELEXP) != 0);
    if (delayed_expunge) return device_expunge(base);
    return 0;
}

static BPTR device_expunge(
    register struct fujinet_serial_base *base FN_REGISTER("a6"))
{
    BPTR segment_list;

    if (base->device.dd_Library.lib_OpenCnt != 0) {
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    release_paula(base);
    segment_list = base->segment_list;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    Forbid();
    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - base->device.dd_Library.lib_NegSize,
            (ULONG)base->device.dd_Library.lib_NegSize +
                (ULONG)base->device.dd_Library.lib_PosSize);
    Permit();
    return segment_list;
}

static ULONG device_reserved(void)
{
    return 0;
}

static void device_begin_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_serial_base *base FN_REGISTER("a6"))
{
    struct IOExtSer *req = (struct IOExtSer *)request;

    if (!base->paula_claimed) {
        finish(req, IOERR_OPENFAIL);
        return;
    }
    switch (request->io_Command) {
    case CMD_READ:
        cmd_read(base, req);
        break;
    case CMD_WRITE:
        cmd_write(base, req);
        break;
    case SDCMD_QUERY:
        cmd_query(base, req);
        break;
    case SDCMD_SETPARAMS:
        cmd_setparams(base, req);
        break;
    case CMD_CLEAR:
    case CMD_FLUSH:
    case CMD_RESET:
        cmd_clear(base, req);
        break;
    default:
        finish(req, IOERR_NOCMD);
        break;
    }
}

static LONG device_abort_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_serial_base *base FN_REGISTER("a6"))
{
    (void)request;
    (void)base;
    /* READ/WRITE/QUERY complete inside BeginIO, so there is nothing to abort. */
    return 0;
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
    sizeof(struct fujinet_serial_base),
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
