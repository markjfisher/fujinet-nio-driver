#include <devices/serial.h>
#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_nio_backend.h"
#include "fujinet_nio_serial_channel.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"
#include "fn_session.h"

extern struct ExecBase *SysBase;

#ifndef FN_SERIAL_BACKEND_BAUD
#define FN_SERIAL_BACKEND_BAUD 19200UL
#endif
#define FN_SERIAL_BACKEND_UNIT 0
#define FN_SERIAL_BACKEND_TIMER_UNIT UNIT_MICROHZ
/* The exchange worker waits for a complete FujiBus frame. A 10 ms empty-RX
 * poll adds up to a full video frame of avoidable response latency for each
 * short request; 1 ms keeps the worker yielding without quantising a read
 * response at the display rate. */
#define FN_SERIAL_BACKEND_POLL_MS 1
#define FN_SERIAL_BACKEND_TIMEOUT_MS 5000
#define FN_SERIAL_BACKEND_WIRE_BUF_SIZE ((FN_MAX_PACKET_SIZE * 2) + 2)
/* serial.device requires io_RBufLen to be a multiple of 64. Keep its
 * receive buffer large enough for the full SLIP frame, including delimiters,
 * then round upward rather than handing it the 2050-byte wire buffer size. */
#define FN_SERIAL_BACKEND_RBUF_SIZE \
    (((FN_SERIAL_BACKEND_WIRE_BUF_SIZE + 63) / 64) * 64)

static const UBYTE serial_device_name[] = "serial.device";

static struct MsgPort *serial_port;
static struct IOExtSer *serial_req;
static BYTE serial_open;
static struct MsgPort *timer_port;
static struct timerequest *timer_req;
static BYTE timer_open;
static uint8_t wire_buf[FN_SERIAL_BACKEND_WIRE_BUF_SIZE];
static uint8_t read_buf[128];
static uint16_t read_pos;
static uint16_t read_len;
static fn_stream_session_t session;
static uint8_t session_initialized;
static uint8_t channel_error;
static uint8_t serial_failure_detail;
static uint8_t serial_failure_io_error;
static uint16_t serial_failure_status;
static uint8_t serial_flush_drained_overrun; /* set when session_flush drained IO_STATF_OVERRUN */
static uint32_t serial_baud = FN_SERIAL_BACKEND_BAUD;

/*
 * Writes use synchronous DoIO(CMD_WRITE). timeout_ms is unused on purpose:
 * aborting an outstanding serial CMD_WRITE is not reliable on Amiga
 * serial.device (including Amiberry), the same reason the read path polls
 * SDCMD_QUERY instead of parking a CMD_READ. Each write is one SLIP byte
 * into a finite TX buffer; completion is bounded by the serial driver
 * accepting that byte, not by an application timer. A hung serial.device
 * is a host/emulator defect; treating it as FN_ERR_TIMEOUT would require
 * AbortIO that this backend must not rely on.
 */
static uint8_t serial_write(const uint8_t *buf, uint16_t len)
{
    serial_req->IOSer.io_Command = CMD_WRITE;
    serial_req->IOSer.io_Data = (APTR)buf;
    serial_req->IOSer.io_Length = len;
    if (DoIO((struct IORequest *)serial_req) != 0) {
        serial_failure_detail = FUJINET_NIO_DETAIL_SERIAL_WRITE;
        serial_failure_io_error = (uint8_t)serial_req->IOSer.io_Error;
        serial_failure_status = serial_req->io_Status;
        return FN_ERR_IO;
    }
    return FN_OK;
}

static uint8_t serial_read_byte(uint8_t *byte_out, uint16_t timeout_ms)
{
    ULONG available;

    if (read_pos < read_len) {
        *byte_out = read_buf[read_pos++];
        return FN_OK;
    }

    serial_req->IOSer.io_Command = SDCMD_QUERY;
    serial_req->IOSer.io_Data = NULL;
    serial_req->IOSer.io_Length = 0;
    serial_req->IOSer.io_Actual = 0;
    if (DoIO((struct IORequest *)serial_req) != 0) {
        serial_failure_detail = FUJINET_NIO_DETAIL_SERIAL_QUERY;
        serial_failure_io_error = (uint8_t)serial_req->IOSer.io_Error;
        serial_failure_status = serial_req->io_Status;
        return FN_ERR_IO;
    }
    available = serial_req->IOSer.io_Actual;
    if (available == 0) {
        uint16_t remaining = timeout_ms;
        while (remaining != 0) {
            uint16_t slice = remaining > FN_SERIAL_BACKEND_POLL_MS
                                 ? FN_SERIAL_BACKEND_POLL_MS
                                 : remaining;
            timer_req->tr_node.io_Command = TR_ADDREQUEST;
            timer_req->tr_time.tv_secs = 0;
            timer_req->tr_time.tv_micro = slice * 1000;
            if (DoIO((struct IORequest *)timer_req) != 0) {
                serial_failure_detail = FUJINET_NIO_DETAIL_TIMER_WAIT;
                serial_failure_io_error = (uint8_t)timer_req->tr_node.io_Error;
                serial_failure_status = 0;
                return FN_ERR_IO;
            }

            serial_req->IOSer.io_Command = SDCMD_QUERY;
            serial_req->IOSer.io_Data = NULL;
            serial_req->IOSer.io_Length = 0;
            serial_req->IOSer.io_Actual = 0;
            if (DoIO((struct IORequest *)serial_req) != 0) {
                serial_failure_detail = FUJINET_NIO_DETAIL_SERIAL_QUERY;
                serial_failure_io_error = (uint8_t)serial_req->IOSer.io_Error;
                serial_failure_status = serial_req->io_Status;
                return FN_ERR_IO;
            }
            if (serial_req->IOSer.io_Actual != 0) {
                available = serial_req->IOSer.io_Actual;
                break;
            }
            remaining = (uint16_t)(remaining - slice);
        }
        if (available == 0) return FN_ERR_TIMEOUT;
    }
    if (available > sizeof(read_buf)) available = sizeof(read_buf);

    serial_req->IOSer.io_Command = CMD_READ;
    serial_req->IOSer.io_Data = (APTR)read_buf;
    serial_req->IOSer.io_Length = available;
    serial_req->IOSer.io_Actual = 0;
    if (DoIO((struct IORequest *)serial_req) != 0) {
        /* cause=9 if session_flush already drained IO_STATF_OVERRUN but
         * CMD_READ still failed; cause=7 if flush drain never triggered.
         * These two cases tell us whether the flag was visible before
         * CMD_WRITE (flush path) or only appeared during response receipt. */
        serial_failure_detail = serial_flush_drained_overrun
            ? FUJINET_NIO_DETAIL_FLUSH_DRAINED_THEN_READ_FAILED
            : FUJINET_NIO_DETAIL_SERIAL_READ;
        serial_failure_io_error = (uint8_t)serial_req->IOSer.io_Error;
        /* io_Status high byte reveals which SerErr_LineErr sub-flag fired:
         * IO_STATF_OVERRUN (bit8=0x0100), IO_STATF_FRAMEERROR (bit9=0x0200),
         * IO_STATF_PARITYERR (bit10=0x0400).  status-hi in the exchange tool
         * is this high byte (1=overrun, 2=framing, 4=parity). */
        serial_failure_status = serial_req->io_Status;
        return FN_ERR_IO;
    }
    read_pos = 1;
    read_len = (uint16_t)serial_req->IOSer.io_Actual;
    if (read_len == 0) return FN_ERR_NOT_READY;
    *byte_out = read_buf[0];
    return FN_OK;
}

static uint8_t session_open(void *context)
{
    (void)context;
    return FN_OK;
}

static void session_close(void *context)
{
    (void)context;
}

static uint8_t session_write_byte(void *context, uint8_t value,
                                  uint16_t timeout_ms)
{
    uint8_t result;

    (void)context;
    (void)timeout_ms;
    result = serial_write(&value, 1);
    if (result != FN_OK && channel_error == 0) channel_error = result;
    return result;
}

static uint8_t session_write_bytes(void *context, const uint8_t *data,
                                   uint16_t length, uint16_t timeout_ms)
{
    uint8_t result;

    (void)context;
    (void)timeout_ms;
    result = serial_write(data, length);
    if (result != FN_OK && channel_error == 0) channel_error = result;
    return result;
}

static uint8_t session_read_byte(void *context, uint8_t *value,
                                 uint16_t timeout_ms)
{
    (void)context;
    if (channel_error != 0) return 0;
    return fn_serial_channel_got_byte(
        serial_read_byte(value, timeout_ms), &channel_error);
}

static void session_flush(void *context)
{
    ULONG available;
    UBYTE drain_byte;

    (void)context;
    read_pos = 0;
    read_len = 0;
    if (!serial_open) return;

    for (;;) {
        serial_req->IOSer.io_Command = SDCMD_QUERY;
        serial_req->IOSer.io_Data = NULL;
        serial_req->IOSer.io_Length = 0;
        serial_req->IOSer.io_Actual = 0;
        if (DoIO((struct IORequest *)serial_req) != 0) {
            if (channel_error == 0) channel_error = FN_ERR_IO;
            return;
        }
        available = serial_req->IOSer.io_Actual;

        /* IO_STATF_OVERRUN can be latched in CIA hardware status even when
         * the ring buffer is empty (available == 0).  CMD_READ returns
         * immediately when the flag is set, consuming it.  Drain it here
         * before CMD_WRITE so the first real CMD_READ sees clean state.
         * This catches the flag whether it was left by SDCMD_SETPARAMS or
         * by a previous failed exchange. */
        if (serial_req->io_Status & IO_STATF_OVERRUN) {
            serial_req->IOSer.io_Command = CMD_READ;
            serial_req->IOSer.io_Data = (APTR)&drain_byte;
            serial_req->IOSer.io_Length = 1;
            serial_req->IOSer.io_Actual = 0;
            DoIO((struct IORequest *)serial_req);
            /* Ignore result — flag consumed; loop to verify clean state. */
            serial_flush_drained_overrun = 1;
            continue;
        }

        if (available == 0) return;
        if (available > sizeof(read_buf)) available = sizeof(read_buf);
        serial_req->IOSer.io_Command = CMD_READ;
        serial_req->IOSer.io_Data = (APTR)read_buf;
        serial_req->IOSer.io_Length = available;
        serial_req->IOSer.io_Actual = 0;
        if (DoIO((struct IORequest *)serial_req) != 0) {
            if (channel_error == 0) channel_error = FN_ERR_IO;
            return;
        }
    }
}

static const fn_stream_channel_ops_t session_ops = {
    session_open,
    session_close,
    session_write_byte,
    session_read_byte,
    session_flush,
    session_write_bytes
};

static void release_timer(void)
{
    if (timer_open) {
        CloseDevice((struct IORequest *)timer_req);
        timer_open = 0;
    }
    if (timer_req != NULL) {
        DeleteExtIO((struct IORequest *)timer_req);
        timer_req = NULL;
    }
    if (timer_port != NULL) {
        DeletePort(timer_port);
        timer_port = NULL;
    }
}

static void release_serial(void)
{
    if (serial_open) {
        CloseDevice((struct IORequest *)serial_req);
        serial_open = 0;
    }
    if (serial_req != NULL) {
        DeleteExtIO((struct IORequest *)serial_req);
        serial_req = NULL;
    }
    if (serial_port != NULL) {
        DeletePort(serial_port);
        serial_port = NULL;
    }
}

void backend_close(void)
{
    if (session_initialized) {
        fn_stream_session_close(&session);
        session_initialized = 0;
    }
    memset(&session, 0, sizeof(session));
    read_pos = 0;
    read_len = 0;
    channel_error = 0;
    serial_failure_detail = FUJINET_NIO_DETAIL_NONE;
    serial_failure_io_error = 0;
    serial_failure_status = 0;
    serial_flush_drained_overrun = 0;
    release_timer();
    release_serial();
}

uint8_t backend_set_baud(uint32_t baud)
{
    /* serial.device accepts a ULONG baud rate. Keep the public setting within
     * the practical range shared by the Amiga UART and FujiNet ESP32 UART
     * configurations; reopening applies it atomically between exchanges. */
    if (baud < 300UL || baud > 230400UL) return FN_ERR_INVALID;
    serial_baud = baud;
    return FN_OK;
}

uint32_t backend_get_baud(void)
{
    return serial_baud;
}

uint8_t backend_open(void)
{
    if (serial_open && timer_open && session_initialized) return FN_OK;

    backend_close();

    serial_port = CreatePort(NULL, 0);
    if (serial_port == NULL) return FN_ERR_IO;
    serial_req = (struct IOExtSer *)CreateExtIO(serial_port,
                                                sizeof(struct IOExtSer));
    if (serial_req == NULL) {
        backend_close();
        return FN_ERR_IO;
    }
    if (OpenDevice(serial_device_name, FN_SERIAL_BACKEND_UNIT,
                   (struct IORequest *)serial_req, 0) != 0) {
        backend_close();
        return FN_ERR_NOT_FOUND;
    }
    serial_open = 1;

    serial_req->io_Baud = serial_baud;
    serial_req->io_ReadLen = 8;
    serial_req->io_WriteLen = 8;
    serial_req->io_StopBits = 1;
    serial_req->io_RBufLen = FN_SERIAL_BACKEND_RBUF_SIZE;
    /* SERF_RAD_BOOGIE raises the serial receive ISR to Level 6 (the highest
     * maskable priority on the Amiga). Without it, at 57,600 baud a new byte
     * can arrive at the CIA UART hardware shift register before the standard
     * Level-5 ISR has moved the previous byte to the software buffer, producing
     * the IO_STATF_OVERRUN seen on the first CMD_READ. SERF_RAD_BOOGIE skips
     * break/parity checking, which is acceptable: FujiBus uses SLIP framing
     * and its own packet integrity; we do not rely on RS-232 line-status bits.
     * Receive overrun detection (IO_STATF_OVERRUN via SerErr_LineErr / CMD_READ
     * io_Error = IOERR_BADLENGTH mapping) is preserved regardless of this flag. */
    serial_req->io_SerFlags = SERF_XDISABLED | SERF_RAD_BOOGIE | SERF_7WIRE;
    serial_req->IOSer.io_Command = SDCMD_SETPARAMS;
    if (DoIO((struct IORequest *)serial_req) != 0) {
        backend_close();
        return FN_ERR_IO;
    }

    /* A baud-rate transition in SDCMD_SETPARAMS can latch IO_STATF_OVERRUN
     * in the CIA 8520 hardware status register. There is no Amiga API to
     * clear this flag directly (CMD_CLEAR only discards buffered data).
     * The only way to consume it is CMD_READ: when the flag is present,
     * serial.device returns immediately with SerErr_LineErr / io_Actual=0,
     * and the flag is cleared as a side-effect. Draining it here ensures
     * the first real CMD_READ of every exchange session sees clean state,
     * regardless of how many times the backend is closed and reopened. */
    serial_req->IOSer.io_Command = SDCMD_QUERY;
    serial_req->IOSer.io_Data = NULL;
    serial_req->IOSer.io_Length = 0;
    serial_req->IOSer.io_Actual = 0;
    DoIO((struct IORequest *)serial_req);
    if (serial_req->io_Status & IO_STATF_OVERRUN) {
        UBYTE drain_byte;
        serial_req->IOSer.io_Command = CMD_READ;
        serial_req->IOSer.io_Data = (APTR)&drain_byte;
        serial_req->IOSer.io_Length = 1;
        serial_req->IOSer.io_Actual = 0;
        DoIO((struct IORequest *)serial_req);
        /* Ignore the error result — the overrun flag is now consumed. */
    }

    timer_port = CreatePort(NULL, 0);
    if (timer_port == NULL) {
        backend_close();
        return FN_ERR_IO;
    }
    timer_req = (struct timerequest *)CreateExtIO(
        timer_port, sizeof(struct timerequest));
    if (timer_req == NULL) {
        backend_close();
        return FN_ERR_IO;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, FN_SERIAL_BACKEND_TIMER_UNIT,
                   (struct IORequest *)timer_req, 0) != 0) {
        backend_close();
        return FN_ERR_NOT_FOUND;
    }
    timer_open = 1;

    if (fn_stream_session_init(&session, &session_ops, 0, wire_buf,
                               sizeof(wire_buf)) != FN_OK ||
        fn_stream_session_open(&session) != FN_OK) {
        backend_close();
        return FN_ERR_IO;
    }
    session_initialized = 1;
    read_pos = 0;
    read_len = 0;
    channel_error = 0;
    serial_failure_detail = FUJINET_NIO_DETAIL_NONE;
    return FN_OK;
}

uint8_t backend_recover_from_overrun(void)
{
    /* If the last transport error was specifically IO_STATF_OVERRUN, keep
     * serial.device and timer.device open (preserving CIA Level-6 ISR warm
     * state) and reset only the SLIP session layer.  The next exchange will
     * skip SDCMD_SETPARAMS entirely, matching the REUSE behaviour that
     * reliably works.  Returns 1 if recovery was applied (caller must NOT
     * call backend_close), 0 if it was a different error (caller must close
     * normally). */
    if (serial_failure_io_error != SerErr_LineErr ||
        !(serial_failure_status & IO_STATF_OVERRUN) ||
        !serial_open || !timer_open)
        return 0;

    if (session_initialized) {
        fn_stream_session_close(&session);
        session_initialized = 0;
    }
    memset(&session, 0, sizeof(session));
    read_pos = 0;
    read_len = 0;
    channel_error = 0;
    serial_failure_detail = FUJINET_NIO_DETAIL_NONE;
    serial_failure_io_error = 0;
    serial_failure_status = 0;
    serial_flush_drained_overrun = 0;

    if (fn_stream_session_init(&session, &session_ops, 0, wire_buf,
                               sizeof(wire_buf)) != FN_OK ||
        fn_stream_session_open(&session) != FN_OK) {
        backend_close();
        return 0;
    }
    session_initialized = 1;
    return 1;
}

uint8_t backend_exchange(
    const uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_len,
    uint8_t *detail,
    uint8_t *native_io_error,
    uint16_t *native_status)
{
    uint8_t session_result;
    uint8_t result;

    if (response_len != NULL) *response_len = 0;
    if (detail != NULL) *detail = FUJINET_NIO_DETAIL_NONE;
    if (native_io_error != NULL) *native_io_error = 0;
    if (native_status != NULL) *native_status = 0;
    if (!serial_open || !timer_open || !session_initialized) {
        if (detail != NULL) *detail = FUJINET_NIO_DETAIL_BACKEND_OPEN;
        return FN_ERR_IO;
    }
    if (response_len == NULL) return FN_ERR_INVALID;

    channel_error = 0;
    serial_failure_detail = FUJINET_NIO_DETAIL_NONE;
    serial_failure_io_error = 0;
    serial_failure_status = 0;
    session_result = fn_stream_session_request(
        &session, request, request_len, response, response_capacity,
        response_len, FN_SERIAL_BACKEND_TIMEOUT_MS);
    if (detail != NULL) {
        if (channel_error != 0)
            *detail = serial_failure_detail != FUJINET_NIO_DETAIL_NONE
                          ? serial_failure_detail
                          : FUJINET_NIO_DETAIL_SERIAL_IO;
        else if (session_result == FN_ERR_IO)
            *detail = FUJINET_NIO_DETAIL_SESSION_IO;
        else if (session_result == FN_ERR_TIMEOUT)
            *detail = FUJINET_NIO_DETAIL_TIMEOUT;
    }
    if (native_io_error != NULL)
        *native_io_error = serial_failure_io_error;
    if (native_status != NULL)
        *native_status = serial_failure_status;
    result = fn_serial_channel_map_session_result(session_result, &channel_error);
    /*
     * Timeout is not FN_ERR_TRANSPORT, but leftover RX / a late SLIP frame
     * would desynchronize the next exchange. Drain now; the worker then
     * backend_close()s so the next exchange lazy-reopens a clean session.
     */
    if (result == FN_ERR_TIMEOUT) session_flush(NULL);
    return result;
}
