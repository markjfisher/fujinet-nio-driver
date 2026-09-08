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
#include <proto/misc.h>
#include <resources/misc.h>

#include <stddef.h>
#include <string.h>

#include "fujinet_paula_uart.h"
#include "fujinet_serial_device.h"

#define FN_REGISTER(name) __asm(name)

#define DEVICE_NAME FUJINET_SERIAL_DEVICE_NAME
#define DEVICE_VERSION 0
#define DEVICE_BAUD_DEFAULT 19200UL
#define TBE_SPIN_MAX 2000000UL

#define READ_IDLE 0
#define READ_PENDING 1
#define READ_COMPLETING 2
#define READ_ABORTING 3
#define READ_REPLIED 4

#define paula (*(volatile struct Custom *)0xdff000)

struct fujinet_serial_rbf_data {
    fujinet_paula_rx_t rx;
    uint16_t pending_need;
    uint16_t pad;
    struct Interrupt *soft_int;
    struct ExecBase *sys_base;
    uint16_t rbf_fire;
    uint16_t ingest;
};

struct fujinet_serial_base {
    struct Device device;
    BPTR segment_list;
    struct Unit exec_unit;
    struct Interrupt rbf_int;
    struct Interrupt soft_int;
    uint8_t rx_buf[FUJINET_PAULA_RX_DEFAULT_SIZE];
    struct fujinet_serial_rbf_data rbf_data;
    uint32_t baud;
    struct Interrupt *old_rbf;
    struct IOExtSer *pending_read;
    UBYTE read_state;
    UBYTE rbf_installed;
    UBYTE rbf_was_enabled;
    UBYTE tbe_was_enabled;
    UBYTE port_claimed;
    UBYTE bits_claimed;
    UBYTE receive_armed;
    UBYTE closing;
    uint16_t write_discard;
    uint16_t setparams_discard;
};

struct ExecBase *SysBase;
struct Library *MiscBase;

extern void fujinet_serial_rbf_server(void);
extern void fujinet_serial_softint(void);
void fujinet_serial_complete_read(
    register struct fujinet_serial_base *base FN_REGISTER("a1"));

typedef char fn_rbf_off_buf_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.buf) == 0 ? 1 : -1];
typedef char fn_rbf_off_mask_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.mask) == 4 ? 1 : -1];
typedef char fn_rbf_off_head_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.head) == 6 ? 1 : -1];
typedef char fn_rbf_off_tail_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.tail) == 8 ? 1 : -1];
typedef char fn_rbf_off_hw_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.hardware_overrun_latched) == 10 ? 1 : -1];
typedef char fn_rbf_off_sw_ok[
    offsetof(struct fujinet_serial_rbf_data, rx.software_ring_overflow_latched) == 11 ? 1 : -1];
typedef char fn_rbf_off_need_ok[
    offsetof(struct fujinet_serial_rbf_data, pending_need) == 12 ? 1 : -1];
typedef char fn_rbf_off_soft_ok[
    offsetof(struct fujinet_serial_rbf_data, soft_int) == 16 ? 1 : -1];
typedef char fn_rbf_off_sys_ok[
    offsetof(struct fujinet_serial_rbf_data, sys_base) == 20 ? 1 : -1];
typedef char fn_rbf_off_fire_ok[
    offsetof(struct fujinet_serial_rbf_data, rbf_fire) == 24 ? 1 : -1];
typedef char fn_rbf_off_ingest_ok[
    offsetof(struct fujinet_serial_rbf_data, ingest) == 26 ? 1 : -1];

static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    "$VER: " DEVICE_NAME " 0.7 (7.9.2026) \xa9 2026 Mark Fisher\r\n";

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
    while ((paula.intreqr & (UWORD)INTF_RBF) != 0) {
        UWORD serdatr = paula.serdatr;

        if (base->rbf_data.rbf_fire != 0xFFFFU)
            base->rbf_data.rbf_fire += 1;
        if ((serdatr & FUJINET_PAULA_SERDATR_RBF) != 0 &&
            base->rbf_data.ingest != 0xFFFFU)
            base->rbf_data.ingest += 1;
        fujinet_paula_rx_ingest(&base->rbf_data.rx, serdatr);
        paula.intreq = (UWORD)INTF_RBF;
    }
}

static uint32_t baud_from_open(const struct IOExtSer *req)
{
    uint32_t baud = req->io_Baud;

    if (baud < FUJINET_SERIAL_BAUD_MIN || baud > FUJINET_SERIAL_BAUD_MAX)
        return DEVICE_BAUD_DEFAULT;
    return baud;
}

/* Read SERDATR for TSRE only while RBF is masked. A live RBF handler and a
 * task-level SERDATR poll would race over the same receive register. */
static void wait_tx_idle(void)
{
    ULONG spin = 0;

    while ((paula.serdatr & FUJINET_PAULA_SERDATR_TSRE) == 0) {
        if (++spin >= TBE_SPIN_MAX) return;
    }
}

/* SERDATR TBE is the transmit-empty flag. The same read can also show a
 * received byte (RBF). Ingest that byte here; do not poll INTREQ TBE —
 * INTENA TBE is masked, and on PiStorm INTREQ TBE never latched, so TX
 * spun under Forbid and the ESP saw nothing. Call with Disable held so
 * the RBF handler does not also sample SERDATR. */
static int wait_tbe(struct fujinet_serial_base *base)
{
    ULONG spin = 0;

    for (;;) {
        UWORD serdatr = paula.serdatr;

        /* Drain every stacked RBF before looking at TBE. One ingest per
         * loop lost leading response bytes when ESP replied during TX
         * (PiStorm first-after-idle: resp_len 40..43 vs 44). */
        while ((serdatr & FUJINET_PAULA_SERDATR_RBF) != 0) {
            if (base->rbf_data.rbf_fire != 0xFFFFU)
                base->rbf_data.rbf_fire += 1;
            if (base->rbf_data.ingest != 0xFFFFU)
                base->rbf_data.ingest += 1;
            fujinet_paula_rx_ingest(&base->rbf_data.rx, serdatr);
            paula.intreq = (UWORD)INTF_RBF;
            serdatr = paula.serdatr;
        }
        if ((serdatr & FUJINET_PAULA_SERDATR_TBE) != 0)
            return 0;
        if (++spin >= TBE_SPIN_MAX) return -1;
    }
}

static void settle_serper(struct fujinet_serial_base *base)
{
    Disable();
    paula.intena = (UWORD)INTF_RBF;
    base->receive_armed = 0;
    drain_rbf_locked(base);
    Enable();
    wait_tx_idle();
    apply_serper(base);
    Disable();
    drain_rbf_locked(base);
    {
        uint16_t queued = fujinet_paula_rx_count(&base->rbf_data.rx);

        if ((uint32_t)base->setparams_discard + queued > 0xFFFFUL)
            base->setparams_discard = 0xFFFFU;
        else
            base->setparams_discard += queued;
    }
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
    base->receive_armed = 1;
    Enable();
}

static int rbf_vector_is_ours(struct fujinet_serial_base *base)
{
    return SysBase->IntVects[INTB_RBF].iv_Code ==
               (void (*)())fujinet_serial_rbf_server &&
           SysBase->IntVects[INTB_RBF].iv_Data == &base->rbf_data;
}

static void rearm_receive(struct fujinet_serial_base *base)
{
    if (base->receive_armed) return;
    Disable();
    drain_rbf_locked(base);
    paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
    base->receive_armed = 1;
    Enable();
}

static struct IOExtSer *take_pending(struct fujinet_serial_base *base, UBYTE next)
{
    struct IOExtSer *req;

    if (base->read_state != READ_PENDING || base->pending_read == NULL)
        return NULL;
    req = base->pending_read;
    base->read_state = next;
    base->pending_read = NULL;
    base->rbf_data.pending_need = 0;
    return req;
}

static void reply_request(struct IOExtSer *req, BYTE err)
{
    req->IOSer.io_Error = err;
    ReplyMsg(&req->IOSer.io_Message);
}

static void finish(struct IOExtSer *req, BYTE err)
{
    req->IOSer.io_Error = err;
    if ((req->IOSer.io_Flags & IOF_QUICK) != 0) return;
    ReplyMsg(&req->IOSer.io_Message);
}

void fujinet_serial_complete_read(
    register struct fujinet_serial_base *base FN_REGISTER("a1"))
{
    struct IOExtSer *req;
    ULONG length;
    uint8_t *data;
    uint16_t copied;

    Disable();
    req = take_pending(base, READ_COMPLETING);
    if (req == NULL) {
        Enable();
        return;
    }
    length = req->IOSer.io_Length;
    data = (uint8_t *)req->IOSer.io_Data;
    if (length > 0xFFFFUL) length = 0xFFFFUL;
    copied = fujinet_paula_rx_read(&base->rbf_data.rx, data, (uint16_t)length);
    req->IOSer.io_Actual = copied;
    req->io_Status = fujinet_paula_rx_public_overrun(&base->rbf_data.rx)
                         ? (UWORD)IO_STATF_OVERRUN
                         : 0;
    base->read_state = READ_REPLIED;
    Enable();
    reply_request(req, 0);
}

static BYTE claim_misc(struct fujinet_serial_base *base)
{
    if (MiscBase == NULL) {
        MiscBase = (struct Library *)OpenResource((CONST_STRPTR)MISCNAME);
        if (MiscBase == NULL) return IOERR_OPENFAIL;
    }
    if (AllocMiscResource(MR_SERIALPORT, (UBYTE *)device_name) != NULL)
        return IOERR_UNITBUSY;
    base->port_claimed = 1;
    if (AllocMiscResource(MR_SERIALBITS, (UBYTE *)device_name) != NULL) {
        FreeMiscResource(MR_SERIALPORT);
        base->port_claimed = 0;
        return IOERR_UNITBUSY;
    }
    base->bits_claimed = 1;
    return 0;
}

static void release_misc(struct fujinet_serial_base *base)
{
    if (base->bits_claimed) {
        FreeMiscResource(MR_SERIALBITS);
        base->bits_claimed = 0;
    }
    if (base->port_claimed) {
        FreeMiscResource(MR_SERIALPORT);
        base->port_claimed = 0;
    }
}

static void release_paula(struct fujinet_serial_base *base)
{
    struct IOExtSer *pending;

    if (!base->port_claimed && !base->bits_claimed && !base->rbf_installed)
        return;
    Disable();
    base->closing = 1;
    pending = take_pending(base, READ_ABORTING);
    paula.intena = (UWORD)INTF_RBF;
    base->receive_armed = 0;
    drain_rbf_locked(base);
    if (base->rbf_installed && rbf_vector_is_ours(base)) {
        SetIntVector(INTB_RBF, base->old_rbf);
        if (base->rbf_was_enabled)
            paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
        if (base->tbe_was_enabled)
            paula.intena = (UWORD)(INTF_SETCLR | INTF_TBE);
    }
    base->rbf_installed = 0;
    base->old_rbf = NULL;
    base->pending_read = NULL;
    base->rbf_data.pending_need = 0;
    if (pending != NULL) {
        pending->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
        Enable();
        reply_request(pending, IOERR_ABORTED);
    } else {
        Enable();
    }
    release_misc(base);
}

static BYTE claim_paula(struct fujinet_serial_base *base, uint32_t baud)
{
    BYTE err;

    if (fujinet_paula_rx_init(&base->rbf_data.rx, base->rx_buf,
                              FUJINET_PAULA_RX_DEFAULT_SIZE) != 0)
        return IOERR_OPENFAIL;
    base->baud = baud;
    base->closing = 0;
    base->pending_read = NULL;
    base->read_state = READ_IDLE;
    base->rbf_data.pending_need = 0;
    base->rbf_data.rbf_fire = 0;
    base->rbf_data.ingest = 0;
    base->write_discard = 0;
    base->setparams_discard = 0;
    err = claim_misc(base);
    if (err != 0) return err;

    Disable();
    base->rbf_was_enabled =
        (UBYTE)((paula.intenar & (UWORD)INTF_RBF) != 0);
    base->tbe_was_enabled =
        (UBYTE)((paula.intenar & (UWORD)INTF_TBE) != 0);
    paula.intena = (UWORD)(INTF_RBF | INTF_TBE);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rbf_data.rx);

    memset(&base->rbf_int, 0, sizeof(base->rbf_int));
    base->rbf_int.is_Node.ln_Type = NT_INTERRUPT;
    base->rbf_int.is_Node.ln_Pri = 0;
    base->rbf_int.is_Node.ln_Name = (char *)device_name;
    base->rbf_int.is_Data = &base->rbf_data;
    base->rbf_int.is_Code = (void (*)())fujinet_serial_rbf_server;

    memset(&base->soft_int, 0, sizeof(base->soft_int));
    base->soft_int.is_Node.ln_Type = NT_INTERRUPT;
    base->soft_int.is_Node.ln_Pri = 0;
    base->soft_int.is_Node.ln_Name = (char *)device_name;
    base->soft_int.is_Data = base;
    base->soft_int.is_Code = (void (*)())fujinet_serial_softint;

    base->rbf_data.soft_int = &base->soft_int;
    base->rbf_data.sys_base = SysBase;
    base->old_rbf = SetIntVector(INTB_RBF, &base->rbf_int);
    base->rbf_installed = 1;
    base->receive_armed = 0;
    Enable();
    settle_serper(base);
    return 0;
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

static void cmd_query(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    rearm_receive(base);
    Disable();
    req->IOSer.io_Actual = fujinet_paula_rx_count(&base->rbf_data.rx);
    req->io_Status = fujinet_paula_rx_public_overrun(&base->rbf_data.rx)
                         ? (UWORD)IO_STATF_OVERRUN
                         : 0;
    req->io_ExtFlags = ((ULONG)base->rbf_data.rbf_fire << 16) |
                       (ULONG)base->rbf_data.ingest;
    req->io_CtlChar = ((ULONG)base->write_discard << 16) |
                      (ULONG)base->setparams_discard;
    Enable();
    finish(req, 0);
}

static void cmd_read(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    ULONG length = req->IOSer.io_Length;
    uint8_t *data = (uint8_t *)req->IOSer.io_Data;

    if (base->closing) {
        finish(req, IOERR_OPENFAIL);
        return;
    }
    rearm_receive(base);
    if (length != 0 && data == NULL) {
        req->IOSer.io_Actual = 0;
        finish(req, IOERR_BADADDRESS);
        return;
    }
    if (length == 0) {
        req->IOSer.io_Actual = 0;
        finish(req, 0);
        return;
    }
    if (length > 0xFFFFUL) length = 0xFFFFUL;
    if (length > base->rbf_data.rx.mask) {
        req->IOSer.io_Actual = 0;
        finish(req, SerErr_InvParam);
        return;
    }
    req->IOSer.io_Flags &= (UBYTE)~IOF_QUICK;
    Disable();
    if (base->read_state == READ_PENDING) {
        Enable();
        finish(req, IOERR_UNITBUSY);
        return;
    }
    base->pending_read = req;
    base->read_state = READ_PENDING;
    base->rbf_data.pending_need = (uint16_t)length;
    if (fujinet_paula_rx_count(&base->rbf_data.rx) >= (uint16_t)length)
        Cause(&base->soft_int);
    Enable();
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
    rearm_receive(base);
    Disable();
    drain_rbf_locked(base);
    base->write_discard = fujinet_paula_rx_count(&base->rbf_data.rx);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    base->rbf_data.rbf_fire = 0;
    base->rbf_data.ingest = 0;
    for (i = 0; i < length; ++i) {
        if (wait_tbe(base) != 0) {
            Enable();
            req->IOSer.io_Actual = i;
            finish(req, SerErr_LineErr);
            return;
        }
        paula.serdat = fujinet_paula_serdat_word(data[i]);
    }
    drain_rbf_locked(base);
    Enable();
    req->IOSer.io_Actual = length;
    finish(req, 0);
}

static void cmd_setparams(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    uint32_t baud = req->io_Baud;

    if (base->read_state == READ_PENDING) {
        finish(req, SerErr_InvParam);
        return;
    }
    if (baud < FUJINET_SERIAL_BAUD_MIN || baud > FUJINET_SERIAL_BAUD_MAX) {
        finish(req, SerErr_BaudMismatch);
        return;
    }
    if (!fujinet_serial_params_valid(baud, req->io_ReadLen, req->io_WriteLen,
                                     req->io_StopBits,
                                     (req->io_SerFlags & SERF_PARTY_ON) != 0)) {
        finish(req, SerErr_InvParam);
        return;
    }
    base->baud = baud;
    settle_serper(base);
    req->io_RBufLen = FUJINET_PAULA_RX_DEFAULT_SIZE;
    req->io_SerFlags |= SERF_XDISABLED;
    finish(req, 0);
}

static void cmd_clear(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    struct IOExtSer *pending;

    Disable();
    pending = take_pending(base, READ_ABORTING);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    if (pending != NULL) {
        pending->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
        Enable();
        reply_request(pending, IOERR_ABORTED);
    } else {
        Enable();
    }
    req->IOSer.io_Actual = 0;
    req->io_Status = 0;
    finish(req, 0);
}

static void cmd_flush(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    struct IOExtSer *pending;

    Disable();
    pending = take_pending(base, READ_ABORTING);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    /* Stay armed. PiStorm 38400: first request after FLUSH-quiesce timed
     * out with no RX (cause=4) even with 16/2000 pacing; later trials in
     * the same command succeeded after timeout closed and reopened. */
    if (pending != NULL) {
        pending->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
        Enable();
        reply_request(pending, IOERR_ABORTED);
    } else {
        Enable();
    }
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
    BYTE err;

    (void)flags;
    if (unit_number != FUJINET_SERIAL_DEVICE_UNIT) {
        request->io_Error = IOERR_OPENFAIL;
        return NULL;
    }
    if (base->device.dd_Library.lib_OpenCnt != 0) {
        request->io_Error = IOERR_UNITBUSY;
        return NULL;
    }
    err = claim_paula(base, baud_from_open(req));
    if (err != 0) {
        request->io_Error = err;
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

    if (!base->port_claimed || !base->bits_claimed || base->closing) {
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
        cmd_clear(base, req);
        break;
    case CMD_FLUSH:
    case CMD_RESET:
        cmd_flush(base, req);
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
    struct IOExtSer *req = (struct IOExtSer *)request;
    struct IOExtSer *taken;

    Disable();
    if (base->pending_read != req) {
        Enable();
        return 0;
    }
    taken = take_pending(base, READ_ABORTING);
    if (taken != NULL) {
        taken->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
        Enable();
        reply_request(taken, IOERR_ABORTED);
    } else {
        Enable();
    }
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
