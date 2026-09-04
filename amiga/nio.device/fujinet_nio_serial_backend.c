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
 * serial.device (including Amiberry). Each write is one SLIP byte into a
 * finite TX buffer; completion is bounded by the serial driver accepting
 * that byte. Reads poll SDCMD_QUERY until bytes are advertised, then
 * CMD_READ that length. QUERY can over-report after an overrun; unbounded
 * DoIO(CMD_READ) then waits forever. Those reads use SendIO plus a timer
 * AbortIO so the worker can return FN_ERR_TIMEOUT instead of hanging.
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

static uint8_t serial_cmd_read(APTR buf, ULONG length, uint16_t timeout_ms)
{
    ULONG serial_mask;
    ULONG timer_mask;

    if (buf == NULL || length == 0 || serial_req == NULL || !serial_open ||
        timer_req == NULL || !timer_open || timeout_ms == 0) {
        return FN_ERR_IO;
    }

    serial_req->IOSer.io_Command = CMD_READ;
    serial_req->IOSer.io_Data = buf;
    serial_req->IOSer.io_Length = length;
    serial_req->IOSer.io_Actual = 0;
    SendIO((struct IORequest *)serial_req);

    timer_req->tr_node.io_Command = TR_ADDREQUEST;
    timer_req->tr_time.tv_secs = (ULONG)(timeout_ms / 1000U);
    timer_req->tr_time.tv_micro = (ULONG)(timeout_ms % 1000U) * 1000UL;
    SendIO((struct IORequest *)timer_req);

    serial_mask = 1UL << serial_port->mp_SigBit;
    timer_mask = 1UL << timer_port->mp_SigBit;
    Wait(serial_mask | timer_mask);

    if (CheckIO((struct IORequest *)serial_req) == NULL) {
        AbortIO((struct IORequest *)serial_req);
        WaitIO((struct IORequest *)serial_req);
        WaitIO((struct IORequest *)timer_req);
        serial_failure_detail = FUJINET_NIO_DETAIL_SERIAL_READ;
        serial_failure_io_error = 0;
        serial_failure_status = 0;
        return FN_ERR_TIMEOUT;
    }

    WaitIO((struct IORequest *)serial_req);
    if (CheckIO((struct IORequest *)timer_req) == NULL) {
        AbortIO((struct IORequest *)timer_req);
        WaitIO((struct IORequest *)timer_req);
    } else {
        WaitIO((struct IORequest *)timer_req);
    }

    if (serial_req->IOSer.io_Error != 0) {
        serial_failure_detail = FUJINET_NIO_DETAIL_SERIAL_READ;
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

    {
        uint8_t read_rc = serial_cmd_read(read_buf, available, timeout_ms);
        if (read_rc != FN_OK) return read_rc;
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

        /* IO_STATF_OVERRUN is a Paula UART receive overrun: the prior RX
         * character was not picked up from SERDATR / INTF_RBF before the next
         * character completed. serial.device can report that flag via
         * SDCMD_QUERY even when the software ring is empty (available == 0).
         * CMD_READ returns immediately when the flag is set, consuming it.
         * Drain it here before CMD_WRITE so the first real CMD_READ sees
         * clean state, whether SETPARAMS or a previous failed exchange left it. */
        if (serial_req->io_Status & IO_STATF_OVERRUN) {
            uint8_t drain_rc = serial_cmd_read(&drain_byte, 1,
                                              FN_SERIAL_BACKEND_TIMEOUT_MS);
            if (drain_rc != FN_OK) {
                if (channel_error == 0) channel_error = drain_rc;
                return;
            }
            /* Flag consumed; loop to verify clean state. */
            serial_flush_drained_overrun = 1;
            continue;
        }

        if (available == 0) return;
        if (available > sizeof(read_buf)) available = sizeof(read_buf);
        {
            uint8_t drain_rc = serial_cmd_read(read_buf, available,
                                              FN_SERIAL_BACKEND_TIMEOUT_MS);
            if (drain_rc != FN_OK) {
                if (channel_error == 0) channel_error = drain_rc;
                return;
            }
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
    /* SERF_RAD_BOOGIE skips extra serial.device checks under 8-bit, no-parity,
     * no-XON/XOFF. It does not change Paula TX/RX direction, SERPER, or create
     * a FIFO. Overrun remains a Paula RBF miss: the prior received character
     * was not cleared before the next one completed. Detection via
     * IO_STATF_OVERRUN / SerErr_LineErr is preserved. FujiBus uses SLIP
     * framing and its own packet integrity rather than RS-232 line-status bits. */
    serial_req->io_SerFlags = SERF_XDISABLED | SERF_RAD_BOOGIE;
    serial_req->IOSer.io_Command = SDCMD_SETPARAMS;
    if (DoIO((struct IORequest *)serial_req) != 0) {
        backend_close();
        return FN_ERR_IO;
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

    /* SDCMD_SETPARAMS can leave IO_STATF_OVERRUN latched. There is no Amiga
     * API to clear it directly (CMD_CLEAR only discards buffered data). This
     * is a Paula receive-overrun latch, not a CIA UART or TX→RX mode switch —
     * Paula RX and TX are independent. Drain if the flag is already visible
     * so the first CMD_READ sees clean state. Timer is already open so the
     * sacrificial CMD_READ cannot hang. */
    serial_req->IOSer.io_Command = SDCMD_QUERY;
    serial_req->IOSer.io_Data = NULL;
    serial_req->IOSer.io_Length = 0;
    serial_req->IOSer.io_Actual = 0;
    DoIO((struct IORequest *)serial_req);
    if (serial_req->io_Status & IO_STATF_OVERRUN) {
        UBYTE drain_byte;
        if (serial_cmd_read(&drain_byte, 1, FN_SERIAL_BACKEND_TIMEOUT_MS) !=
            FN_OK) {
            backend_close();
            return FN_ERR_IO;
        }
    }

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
