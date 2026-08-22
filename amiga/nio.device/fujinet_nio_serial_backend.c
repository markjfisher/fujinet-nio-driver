#include <devices/serial.h>
#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_nio_backend.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"
#include "fn_session.h"

extern struct ExecBase *SysBase;

#define FN_SERIAL_BACKEND_BAUD 19200
#define FN_SERIAL_BACKEND_UNIT 0
#define FN_SERIAL_BACKEND_TIMER_UNIT UNIT_MICROHZ
#define FN_SERIAL_BACKEND_POLL_MS 10
#define FN_SERIAL_BACKEND_TIMEOUT_MS 5000
#define FN_SERIAL_BACKEND_WIRE_BUF_SIZE ((FN_MAX_PACKET_SIZE * 2) + 2)

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

static uint8_t serial_write(const uint8_t *buf, uint16_t len)
{
    serial_req->IOSer.io_Command = CMD_WRITE;
    serial_req->IOSer.io_Data = (APTR)buf;
    serial_req->IOSer.io_Length = len;
    if (DoIO((struct IORequest *)serial_req) != 0) return FN_ERR_IO;
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
    if (DoIO((struct IORequest *)serial_req) != 0) return FN_ERR_IO;
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
            if (DoIO((struct IORequest *)timer_req) != 0) return FN_ERR_IO;

            serial_req->IOSer.io_Command = SDCMD_QUERY;
            serial_req->IOSer.io_Data = NULL;
            serial_req->IOSer.io_Length = 0;
            serial_req->IOSer.io_Actual = 0;
            if (DoIO((struct IORequest *)serial_req) != 0) return FN_ERR_IO;
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
    if (DoIO((struct IORequest *)serial_req) != 0) return FN_ERR_IO;
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
    (void)context;
    (void)timeout_ms;
    return serial_write(&value, 1);
}

static uint8_t session_read_byte(void *context, uint8_t *value,
                                 uint16_t timeout_ms)
{
    (void)context;
    return serial_read_byte(value, timeout_ms) == FN_OK ? 1 : 0;
}

static void session_flush(void *context)
{
    (void)context;
}

static const fn_stream_channel_ops_t session_ops = {
    session_open,
    session_close,
    session_write_byte,
    session_read_byte,
    session_flush
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
    release_timer();
    release_serial();
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

    serial_req->io_Baud = FN_SERIAL_BACKEND_BAUD;
    serial_req->io_ReadLen = 8;
    serial_req->io_WriteLen = 8;
    serial_req->io_StopBits = 1;
    serial_req->io_RBufLen = FN_SERIAL_BACKEND_WIRE_BUF_SIZE;
    serial_req->io_SerFlags = SERF_XDISABLED;
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

    if (fn_stream_session_init(&session, &session_ops, 0, wire_buf,
                               sizeof(wire_buf)) != FN_OK ||
        fn_stream_session_open(&session) != FN_OK) {
        backend_close();
        return FN_ERR_IO;
    }
    session_initialized = 1;
    read_pos = 0;
    read_len = 0;
    return FN_OK;
}

uint8_t backend_exchange(
    const uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_len)
{
    uint8_t result;

    if (response_len != NULL) *response_len = 0;
    if (!serial_open || !timer_open || !session_initialized) return FN_ERR_IO;
    if (response_len == NULL) return FN_ERR_INVALID;

    result = fn_stream_session_request(&session, request, request_len, response,
                                       response_capacity, response_len,
                                       FN_SERIAL_BACKEND_TIMEOUT_MS);
    if (result == FN_ERR_IO) return FN_ERR_TRANSPORT;
    return result;
}
