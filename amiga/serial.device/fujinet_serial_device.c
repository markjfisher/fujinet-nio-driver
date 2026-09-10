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
/* SETPARAMS only: RBF is masked while waiting TSRE. Not used for CMD_WRITE. */
#define TSRE_WAIT_SPINS 2000000UL

#define READ_IDLE 0
#define READ_PENDING 1
#define READ_COMPLETING 2
#define READ_ABORTING 3
#define READ_REPLIED 4

#define WRITE_IDLE 0
#define WRITE_PENDING 1
#define WRITE_COMPLETING 2
#define WRITE_ABORTING 3
#define WRITE_REPLIED 4

#define WRITE_OWNER_NONE 0
#define WRITE_OWNER_TBE 1
#define WRITE_OWNER_ABORT 2
#define WRITE_OWNER_FLUSH 3
#define WRITE_OWNER_CLOSE 4

#define paula (*(volatile struct Custom *)0xdff000)

struct fujinet_serial_rbf_data {
    fujinet_paula_rx_t rx;
    uint16_t pending_need;
    uint16_t pad;
    struct Interrupt *soft_int;
    uint16_t rbf_fire;
    uint16_t ingest;
    uint8_t capture_first;
    uint8_t first_byte;
    uint16_t first_serdatr;
};

struct fujinet_serial_tbe_data {
    const uint8_t *buf;
    ULONG remaining;
    ULONG committed;
    ULONG length;
    struct Interrupt *soft_int;
    uint16_t tbe_fire;
    UBYTE accepted;
    UBYTE pad;
};

struct fujinet_serial_base {
    struct Device device;
    BPTR segment_list;
    struct Unit exec_unit;
    struct Interrupt rbf_int;
    struct Interrupt tbe_int;
    struct Interrupt soft_int;
    struct Interrupt write_soft_int;
    uint8_t rx_buf[FUJINET_PAULA_RX_DEFAULT_SIZE];
    struct fujinet_serial_rbf_data rbf_data;
    struct fujinet_serial_tbe_data tbe_data;
    uint32_t baud;
    struct Interrupt *old_rbf;
    struct Interrupt *old_tbe;
    struct IOExtSer *pending_read;
    struct IOExtSer *pending_write;
    UBYTE read_state;
    UBYTE write_state;
    UBYTE last_write_owner;
    BYTE last_write_error;
    ULONG last_write_length;
    ULONG last_write_committed;
    ULONG last_write_actual;
    uint16_t last_tbe_fire;
    UBYTE rbf_installed;
    UBYTE rbf_was_enabled;
    UBYTE tbe_was_enabled;
    UBYTE port_claimed;
    UBYTE bits_claimed;
    UBYTE receive_armed;
    UBYTE closing;
    uint16_t write_discard;
    uint16_t setparams_discard;
    uint16_t fire_at_write_start;
    uint16_t fire_at_tx_queued;
};

struct ExecBase *SysBase;
struct Library *MiscBase;

extern void fujinet_serial_rbf_server(void);
extern void fujinet_serial_tbe_server(void);
extern void fujinet_serial_softint(void);
extern void fujinet_serial_write_softint(void);
void fujinet_serial_complete_read(
    register struct fujinet_serial_base *base FN_REGISTER("a1"));
void fujinet_serial_complete_write(
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
typedef char fn_rbf_off_fire_ok[
    offsetof(struct fujinet_serial_rbf_data, rbf_fire) == 20 ? 1 : -1];
typedef char fn_rbf_off_ingest_ok[
    offsetof(struct fujinet_serial_rbf_data, ingest) == 22 ? 1 : -1];
typedef char fn_rbf_off_cap_ok[
    offsetof(struct fujinet_serial_rbf_data, capture_first) == 24 ? 1 : -1];
typedef char fn_rbf_off_first_ok[
    offsetof(struct fujinet_serial_rbf_data, first_byte) == 25 ? 1 : -1];
typedef char fn_rbf_off_first_ser_ok[
    offsetof(struct fujinet_serial_rbf_data, first_serdatr) == 26 ? 1 : -1];
typedef char fn_tbe_off_buf_ok[
    offsetof(struct fujinet_serial_tbe_data, buf) == 0 ? 1 : -1];
typedef char fn_tbe_off_rem_ok[
    offsetof(struct fujinet_serial_tbe_data, remaining) == 4 ? 1 : -1];
typedef char fn_tbe_off_committed_ok[
    offsetof(struct fujinet_serial_tbe_data, committed) == 8 ? 1 : -1];
typedef char fn_tbe_off_len_ok[
    offsetof(struct fujinet_serial_tbe_data, length) == 12 ? 1 : -1];
typedef char fn_tbe_off_soft_ok[
    offsetof(struct fujinet_serial_tbe_data, soft_int) == 16 ? 1 : -1];
typedef char fn_tbe_off_fire_ok[
    offsetof(struct fujinet_serial_tbe_data, tbe_fire) == 20 ? 1 : -1];
typedef char fn_tbe_off_accepted_ok[
    offsetof(struct fujinet_serial_tbe_data, accepted) == 22 ? 1 : -1];

static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    "$VER: " DEVICE_NAME " 0.11 (10.9.2026) \xa9 2026 Mark Fisher\r\n";

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
    uint16_t head = base->rbf_data.rx.head;
    uint16_t mask = base->rbf_data.rx.mask;
    uint16_t tail = base->rbf_data.rx.tail;
    uint8_t *buf = base->rbf_data.rx.buf;

    while ((paula.intreqr & (UWORD)INTF_RBF) != 0) {
        UWORD serdatr = paula.serdatr;

        paula.intreq = (UWORD)INTF_RBF;
        if (base->rbf_data.rbf_fire != 0xFFFFU)
            base->rbf_data.rbf_fire += 1;
        if (base->rbf_data.capture_first) {
            base->rbf_data.capture_first = 0;
            base->rbf_data.first_byte = (uint8_t)serdatr;
            base->rbf_data.first_serdatr = serdatr;
        }
        if ((serdatr & FUJINET_PAULA_SERDATR_OVRUN) != 0)
            base->rbf_data.rx.hardware_overrun_latched = 1;
        {
            uint16_t next = (uint16_t)((head + 1U) & mask);

            if (next == tail) {
                base->rbf_data.rx.software_ring_overflow_latched = 1;
            } else {
                buf[head] = (uint8_t)serdatr;
                head = next;
                if (base->rbf_data.ingest != 0xFFFFU)
                    base->rbf_data.ingest += 1;
            }
        }
    }
    base->rbf_data.rx.head = head;
}

static uint32_t baud_from_open(const struct IOExtSer *req)
{
    uint32_t baud = req->io_Baud;

    if (baud < FUJINET_SERIAL_BAUD_MIN || baud > FUJINET_SERIAL_BAUD_MAX)
        return DEVICE_BAUD_DEFAULT;
    return baud;
}

/* Read SERDATR for TSRE only while RBF is masked (SETPARAMS settle).
 * CMD_WRITE uses the TBE interrupt and must not poll SERDATR: a live RBF
 * handler and a task-level SERDATR read race over the same receive byte. */
static void wait_tx_idle(void)
{
    ULONG spin = 0;

    while ((paula.serdatr & FUJINET_PAULA_SERDATR_TSRE) == 0) {
        if (++spin >= TSRE_WAIT_SPINS) return;
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

static int tbe_vector_is_ours(struct fujinet_serial_base *base)
{
    return SysBase->IntVects[INTB_TBE].iv_Code ==
               (void (*)())fujinet_serial_tbe_server &&
           SysBase->IntVects[INTB_TBE].iv_Data == &base->tbe_data;
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

static struct IOExtSer *take_pending_write(struct fujinet_serial_base *base,
                                           UBYTE next, UBYTE owner)
{
    struct IOExtSer *req;

    if (base->write_state != WRITE_PENDING || base->pending_write == NULL)
        return NULL;
    req = base->pending_write;
    base->write_state = next;
    base->pending_write = NULL;
    base->last_write_owner = owner;
    base->last_write_length = base->tbe_data.length;
    base->last_write_committed = base->tbe_data.committed;
    base->last_tbe_fire = base->tbe_data.tbe_fire;
    paula.intena = (UWORD)INTF_TBE;
    base->tbe_data.remaining = 0;
    return req;
}

static void record_write_reply(struct fujinet_serial_base *base, ULONG actual,
                               BYTE err)
{
    base->last_write_actual = actual;
    base->last_write_error = err;
    base->write_state = WRITE_REPLIED;
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

static UWORD status_with_first(struct fujinet_serial_base *base)
{
    UWORD st = fujinet_paula_rx_public_overrun(&base->rbf_data.rx)
                   ? (UWORD)IO_STATF_OVERRUN
                   : 0;
    return (UWORD)(st | (UWORD)base->rbf_data.first_byte);
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
    /* Copy with RBF live. Holding Disable() across the ring copy blocked
     * the handler for several 57600 character times and latched OVRUN
     * (io_Error=0, io_Status bit 8) mid-frame. take_pending already
     * cleared pending_need so the handler will not Cause() this request
     * again. */
    Enable();
    copied = fujinet_paula_rx_read(&base->rbf_data.rx, data, (uint16_t)length);
    Disable();
    req->IOSer.io_Actual = copied;
    req->io_Status = status_with_first(base);
    base->read_state = READ_REPLIED;
    Enable();
    reply_request(req, 0);
}

void fujinet_serial_complete_write(
    register struct fujinet_serial_base *base FN_REGISTER("a1"))
{
    struct IOExtSer *req;
    ULONG actual;

    Disable();
    req = take_pending_write(base, WRITE_COMPLETING, WRITE_OWNER_TBE);
    if (req == NULL) {
        Enable();
        return;
    }
    actual = base->last_write_committed;
    req->IOSer.io_Actual = actual;
    base->fire_at_tx_queued = base->rbf_data.rbf_fire;
    record_write_reply(base, actual, 0);
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
    struct IOExtSer *pending_read;
    struct IOExtSer *pending_write;

    if (!base->port_claimed && !base->bits_claimed && !base->rbf_installed)
        return;
    Disable();
    base->closing = 1;
    pending_write = take_pending_write(base, WRITE_ABORTING, WRITE_OWNER_CLOSE);
    pending_read = take_pending(base, READ_ABORTING);
    paula.intena = (UWORD)(INTF_RBF | INTF_TBE);
    base->receive_armed = 0;
    drain_rbf_locked(base);
    if (base->rbf_installed) {
        if (rbf_vector_is_ours(base))
            SetIntVector(INTB_RBF, base->old_rbf);
        if (tbe_vector_is_ours(base))
            SetIntVector(INTB_TBE, base->old_tbe);
        if (base->rbf_was_enabled)
            paula.intena = (UWORD)(INTF_SETCLR | INTF_RBF);
        if (base->tbe_was_enabled)
            paula.intena = (UWORD)(INTF_SETCLR | INTF_TBE);
    }
    base->rbf_installed = 0;
    base->old_rbf = NULL;
    base->old_tbe = NULL;
    base->pending_read = NULL;
    base->pending_write = NULL;
    base->rbf_data.pending_need = 0;
    if (pending_write != NULL) {
        ULONG actual = base->last_write_committed;

        pending_write->IOSer.io_Actual = actual;
        record_write_reply(base, actual, IOERR_ABORTED);
    }
    if (pending_read != NULL) {
        pending_read->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
    }
    Enable();
    if (pending_write != NULL)
        reply_request(pending_write, IOERR_ABORTED);
    if (pending_read != NULL)
        reply_request(pending_read, IOERR_ABORTED);
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
    base->pending_write = NULL;
    base->read_state = READ_IDLE;
    base->write_state = WRITE_IDLE;
    base->last_write_owner = WRITE_OWNER_NONE;
    base->last_write_error = 0;
    base->last_write_length = 0;
    base->last_write_committed = 0;
    base->last_write_actual = 0;
    base->last_tbe_fire = 0;
    base->rbf_data.pending_need = 0;
    base->rbf_data.rbf_fire = 0;
    base->rbf_data.ingest = 0;
    base->rbf_data.capture_first = 0;
    base->rbf_data.first_byte = 0;
    base->rbf_data.first_serdatr = 0;
    base->write_discard = 0;
    base->setparams_discard = 0;
    base->fire_at_write_start = 0;
    base->fire_at_tx_queued = 0;
    base->tbe_data.buf = NULL;
    base->tbe_data.remaining = 0;
    base->tbe_data.committed = 0;
    base->tbe_data.length = 0;
    base->tbe_data.tbe_fire = 0;
    base->tbe_data.accepted = 0;
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

    memset(&base->tbe_int, 0, sizeof(base->tbe_int));
    base->tbe_int.is_Node.ln_Type = NT_INTERRUPT;
    base->tbe_int.is_Node.ln_Pri = 0;
    base->tbe_int.is_Node.ln_Name = (char *)device_name;
    base->tbe_int.is_Data = &base->tbe_data;
    base->tbe_int.is_Code = (void (*)())fujinet_serial_tbe_server;

    memset(&base->soft_int, 0, sizeof(base->soft_int));
    base->soft_int.is_Node.ln_Type = NT_INTERRUPT;
    base->soft_int.is_Node.ln_Pri = 0;
    base->soft_int.is_Node.ln_Name = (char *)device_name;
    base->soft_int.is_Data = base;
    base->soft_int.is_Code = (void (*)())fujinet_serial_softint;

    memset(&base->write_soft_int, 0, sizeof(base->write_soft_int));
    base->write_soft_int.is_Node.ln_Type = NT_INTERRUPT;
    base->write_soft_int.is_Node.ln_Pri = 0;
    base->write_soft_int.is_Node.ln_Name = (char *)device_name;
    base->write_soft_int.is_Data = base;
    base->write_soft_int.is_Code = (void (*)())fujinet_serial_write_softint;

    base->rbf_data.soft_int = &base->soft_int;
    base->tbe_data.soft_int = &base->write_soft_int;
    base->old_rbf = SetIntVector(INTB_RBF, &base->rbf_int);
    base->old_tbe = SetIntVector(INTB_TBE, &base->tbe_int);
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
    req->io_Status = status_with_first(base);
    req->io_ExtFlags = ((ULONG)base->rbf_data.rbf_fire << 16) |
                       (ULONG)base->rbf_data.ingest;
    req->io_CtlChar = ((ULONG)base->fire_at_write_start << 16) |
                      (ULONG)base->fire_at_tx_queued;
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

    if (base->closing) {
        finish(req, IOERR_OPENFAIL);
        return;
    }
    if (length != 0 && data == NULL) {
        req->IOSer.io_Actual = 0;
        finish(req, IOERR_BADADDRESS);
        return;
    }
    rearm_receive(base);
    Disable();
    if (base->write_state == WRITE_PENDING) {
        Enable();
        finish(req, IOERR_UNITBUSY);
        return;
    }
    /* Leftover drain must not be recorded as the response's first byte. */
    base->rbf_data.capture_first = 0;
    drain_rbf_locked(base);
    base->write_discard = fujinet_paula_rx_count(&base->rbf_data.rx);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    base->fire_at_write_start = base->rbf_data.rbf_fire;
    base->fire_at_tx_queued = 0;
    base->rbf_data.rbf_fire = 0;
    base->rbf_data.ingest = 0;
    base->rbf_data.first_byte = 0;
    base->rbf_data.first_serdatr = 0;
    base->rbf_data.capture_first = 1;
    if (length == 0) {
        Enable();
        req->IOSer.io_Actual = 0;
        finish(req, 0);
        return;
    }
    req->IOSer.io_Flags &= (UBYTE)~IOF_QUICK;
    base->pending_write = req;
    base->write_state = WRITE_PENDING;
    base->last_write_owner = WRITE_OWNER_NONE;
    base->last_write_length = length;
    base->last_write_committed = 0;
    base->last_write_actual = 0;
    base->last_write_error = 0;
    base->last_tbe_fire = 0;
    base->tbe_data.buf = data;
    base->tbe_data.remaining = length;
    base->tbe_data.committed = 0;
    base->tbe_data.length = length;
    base->tbe_data.tbe_fire = 0;
    base->tbe_data.accepted = 0;
    /* RBF is armed. Enable CPU interrupts before TBE writes SERDAT so a
     * response byte can preempt transmit. Do not wait here: TBE Causes a
     * software interrupt when the extra TBE after the last SERDAT write
     * shows the final byte has been accepted by Paula's transmit shift
     * register. That is CMD_WRITE complete; it is not TSRE / wire-idle. */
    Enable();
    paula.intena = (UWORD)(INTF_SETCLR | INTF_TBE);
    paula.intreq = (UWORD)(INTF_SETCLR | INTF_TBE);
}

static void cmd_setparams(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    uint32_t baud = req->io_Baud;

    if (base->read_state == READ_PENDING ||
        base->write_state == WRITE_PENDING) {
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
    struct IOExtSer *pending_read;
    struct IOExtSer *pending_write;
    ULONG write_actual = 0;

    Disable();
    pending_write = take_pending_write(base, WRITE_ABORTING, WRITE_OWNER_FLUSH);
    if (pending_write != NULL) write_actual = base->last_write_committed;
    pending_read = take_pending(base, READ_ABORTING);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    if (pending_write != NULL) {
        pending_write->IOSer.io_Actual = write_actual;
        record_write_reply(base, write_actual, IOERR_ABORTED);
    }
    if (pending_read != NULL) {
        pending_read->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
    }
    Enable();
    if (pending_write != NULL)
        reply_request(pending_write, IOERR_ABORTED);
    if (pending_read != NULL)
        reply_request(pending_read, IOERR_ABORTED);
    req->IOSer.io_Actual = 0;
    req->io_Status = 0;
    finish(req, 0);
}

static void cmd_flush(struct fujinet_serial_base *base, struct IOExtSer *req)
{
    struct IOExtSer *pending_read;
    struct IOExtSer *pending_write;
    ULONG write_actual = 0;

    Disable();
    pending_write = take_pending_write(base, WRITE_ABORTING, WRITE_OWNER_FLUSH);
    if (pending_write != NULL) write_actual = base->last_write_committed;
    pending_read = take_pending(base, READ_ABORTING);
    drain_rbf_locked(base);
    fujinet_paula_rx_clear(&base->rbf_data.rx);
    /* Stay armed. PiStorm 38400: first request after FLUSH-quiesce timed
     * out with no RX (cause=4) even with 16/2000 pacing; later trials in
     * the same command succeeded after timeout closed and reopened. */
    if (pending_write != NULL) {
        pending_write->IOSer.io_Actual = write_actual;
        record_write_reply(base, write_actual, IOERR_ABORTED);
    }
    if (pending_read != NULL) {
        pending_read->IOSer.io_Actual = 0;
        base->read_state = READ_REPLIED;
    }
    Enable();
    if (pending_write != NULL)
        reply_request(pending_write, IOERR_ABORTED);
    if (pending_read != NULL)
        reply_request(pending_read, IOERR_ABORTED);
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
    if (base->pending_write == req) {
        ULONG actual = base->tbe_data.committed;

        taken = take_pending_write(base, WRITE_ABORTING, WRITE_OWNER_ABORT);
        if (taken != NULL) {
            taken->IOSer.io_Actual = actual;
            record_write_reply(base, actual, IOERR_ABORTED);
            Enable();
            reply_request(taken, IOERR_ABORTED);
            return 0;
        }
        Enable();
        return 0;
    }
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
