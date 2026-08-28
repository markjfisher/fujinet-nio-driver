#include <devices/serial.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/nodes.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

#include "fujinet_nio_device.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

#define COMPLETION_URI "host:/amiga-e2e-complete/nio-broker-isolated"
#define FILE_CMD_LIST 0x02

struct exchange_job {
    volatile ULONG started;
    volatile ULONG done;
    BYTE io_error;
    UBYTE nio_error;
    UBYTE broker_stage;
    UBYTE broker_result;
    UBYTE broker_cause;
    UWORD response_length;
    UBYTE response[64];
    UBYTE request[16];
    UWORD request_length;
};

static struct exchange_job job_a;
static struct exchange_job job_b;

static uint8_t packet_checksum(const uint8_t *data, uint16_t len)
{
    uint16_t chk = 0;
    uint16_t i;
    for (i = 0; i < len; ++i) {
        chk = (uint16_t)(chk + data[i]);
        chk = (uint16_t)(((chk >> 8) + (chk & 0xFF)) & 0xFFFF);
    }
    return (uint8_t)(chk & 0xFF);
}

static uint16_t build_clock_cmd(uint8_t *buf, uint8_t cmd)
{
    buf[0] = FN_DEVICE_CLOCK;
    buf[1] = cmd;
    buf[2] = FN_HEADER_SIZE;
    buf[3] = 0;
    buf[4] = 0;
    buf[5] = 0;
    buf[4] = packet_checksum(buf, FN_HEADER_SIZE);
    return FN_HEADER_SIZE;
}

static uint16_t build_clock_get(uint8_t *buf)
{
    return build_clock_cmd(buf, FN_CMD_CLOCK_GET);
}

static uint16_t build_file_list(uint8_t *buf, const char *uri)
{
    uint16_t uri_len = (uint16_t)strlen(uri);
    uint16_t payload = (uint16_t)(1 + 2 + uri_len + 2 + 2);
    uint16_t total = (uint16_t)(FN_HEADER_SIZE + payload);
    uint16_t offset = 0;

    buf[offset++] = FN_DEVICE_FILE;
    buf[offset++] = FILE_CMD_LIST;
    buf[offset++] = (uint8_t)(total & 0xFF);
    buf[offset++] = (uint8_t)(total >> 8);
    buf[offset++] = 0;
    buf[offset++] = 0;
    buf[offset++] = 1;
    buf[offset++] = (uint8_t)(uri_len & 0xFF);
    buf[offset++] = (uint8_t)(uri_len >> 8);
    memcpy(buf + offset, uri, uri_len);
    offset = (uint16_t)(offset + uri_len);
    buf[offset++] = 0;
    buf[offset++] = 0;
    buf[offset++] = 0;
    buf[offset++] = 1;
    buf[4] = packet_checksum(buf, offset);
    return offset;
}

static void fill_exchange(struct FujiNetNIORequest *req, struct MsgPort *port,
                          const uint8_t *request, UWORD request_len,
                          uint8_t *response, UWORD response_cap)
{
    req->fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->fn_io.io_Message.mn_ReplyPort = port;
    req->fn_io.io_Message.mn_Length = sizeof(*req);
    req->fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
    req->fn_io.io_Flags = 0;
    req->fn_io.io_Error = 0;
    req->fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
    req->fn_flags = 0;
    req->fn_request_data = request;
    req->fn_request_length = request_len;
    req->fn_response_data = response;
    req->fn_response_capacity = response_cap;
    req->fn_response_length = 0;
    req->fn_nio_error = 0;
    req->fn_pad[0] = 0;
    req->fn_pad[1] = 0;
    req->fn_pad[2] = 0;
}

static void init_req(struct FujiNetNIORequest *req, struct MsgPort *port,
                     const uint8_t *request, UWORD request_len,
                     uint8_t *response, UWORD response_cap)
{
    memset(req, 0, sizeof(*req));
    fill_exchange(req, port, request, request_len, response, response_cap);
}

static LONG do_exchange(struct FujiNetNIORequest *req, struct MsgPort *port)
{
    req->fn_io.io_Message.mn_ReplyPort = port;
    return DoIO(&req->fn_io);
}

static LONG try_open_serial(void)
{
    struct MsgPort *port;
    struct IOExtSer *serial;
    LONG result;

    port = CreatePort(NULL, 0);
    if (port == NULL) return IOERR_OPENFAIL;
    serial = (struct IOExtSer *)CreateExtIO(port, sizeof(*serial));
    if (serial == NULL) {
        DeletePort(port);
        return IOERR_OPENFAIL;
    }
    result = OpenDevice((CONST_STRPTR)"serial.device", 0,
                        (struct IORequest *)serial, 0);
    if (result == 0) CloseDevice((struct IORequest *)serial);
    DeleteExtIO((struct IORequest *)serial);
    DeletePort(port);
    return result;
}

static int clock_cmd_response_ok(const struct FujiNetNIORequest *req,
                                 const uint8_t *response, uint8_t cmd)
{
    return req->fn_io.io_Error == 0 && req->fn_nio_error == FN_OK &&
           req->fn_response_length >= FN_HEADER_SIZE &&
           response[0] == FN_DEVICE_CLOCK &&
           response[1] == cmd;
}

static int clock_response_ok(const struct FujiNetNIORequest *req,
                             const uint8_t *response)
{
    return clock_cmd_response_ok(req, response, FN_CMD_CLOCK_GET);
}

static void run_job(struct exchange_job *job)
{
    struct MsgPort *port;
    struct FujiNetNIORequest req;
    uint8_t response[64];

    port = CreatePort(NULL, 0);
    if (port == NULL) {
        job->io_error = IOERR_OPENFAIL;
        job->nio_error = FN_ERR_IO;
        job->done = 1;
        return;
    }
    init_req(&req, port, job->request, job->request_length, response,
             sizeof(response));
    job->started = 1;
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &req.fn_io, 0) != 0) {
        job->io_error = req.fn_io.io_Error;
        job->nio_error = req.fn_nio_error;
        DeletePort(port);
        job->done = 1;
        return;
    }
    do_exchange(&req, port);
    job->io_error = req.fn_io.io_Error;
    job->nio_error = req.fn_nio_error;
    job->broker_stage = req.fn_pad[0];
    job->broker_result = req.fn_pad[1];
    job->broker_cause = req.fn_pad[2];
    job->response_length = req.fn_response_length;
    if (req.fn_response_length > 0 &&
        req.fn_response_length <= sizeof(job->response)) {
        memcpy(job->response, response, req.fn_response_length);
    }
    CloseDevice(&req.fn_io);
    DeletePort(port);
    job->done = 1;
}

static void job_a_entry(void)
{
    run_job(&job_a);
}

static void job_b_entry(void)
{
    run_job(&job_b);
}

static uint8_t spawned_a;
static uint8_t spawned_b;

static void wait_spawned_jobs(void)
{
    ULONG spins;

    for (spins = 0; spins < 400; ++spins) {
        if ((!spawned_a || job_a.done) && (!spawned_b || job_b.done))
            return;
        Delay(1);
    }
}

static int isolation_ok(void)
{
    int disk_present =
        FindName(&SysBase->DeviceList,
                 (CONST_STRPTR) "fujinet-disk.device") != NULL;
    int fls_present = FindTask((CONST_STRPTR) "FLS") != NULL;
    LONG serial_before = try_open_serial();
    printf("ISOLATED disk.device=%s FLS=%s serial-free-before=%d\n",
           disk_present ? "present" : "absent",
           fls_present ? "present" : "absent",
           serial_before == 0);
    /* The tool is also the field diagnostic for a normal Workbench session,
     * where DiskDevice may legitimately already be resident.  FLS remains a
     * useful warning because it can own the legacy serial transport, but it
     * must not prevent direct broker diagnostics. */
    return serial_before == 0;
}

int main(void)
{
    struct MsgPort *port;
    struct FujiNetNIORequest req;
    uint8_t clock_req[FN_HEADER_SIZE];
    uint8_t bad_req[1];
    uint8_t list_req[128];
    uint8_t response[256];
    uint16_t clock_len;
    uint16_t list_len;
    LONG serial_after;
    ULONG spins;
    struct Process *proc_a;
    struct Process *proc_b;
    int failures = 0;

    if (!isolation_ok()) return RETURN_FAIL;

    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;

    clock_len = build_clock_get(clock_req);
    init_req(&req, port, clock_req, clock_len, response, sizeof(response));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &req.fn_io, 0) != 0) {
        printf("OPEN FAIL io=%d nio=%u\n", (int)req.fn_io.io_Error,
               (unsigned)req.fn_nio_error);
        DeletePort(port);
        return RETURN_FAIL;
    }

    do_exchange(&req, port);
    printf("EXCHANGE io=%d nio=%u len=%u stage=%u result=%u cause=%u native=%u status-hi=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2],
           (unsigned)(req.fn_flags & 0xFF), (unsigned)(req.fn_flags >> 8));
    if (!clock_response_ok(&req, response)) failures = 1;

    fill_exchange(&req, port, clock_req, clock_len, response, sizeof(response));
    do_exchange(&req, port);
    printf("REUSE io=%d nio=%u len=%u stage=%u result=%u cause=%u native=%u status-hi=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2],
           (unsigned)(req.fn_flags & 0xFF), (unsigned)(req.fn_flags >> 8));
    if (!clock_response_ok(&req, response)) failures = 1;

    CloseDevice(&req.fn_io);
    serial_after = try_open_serial();
    printf("RESIDENT serial-busy-after-opencnt0=%d\n", serial_after != 0);
    if (serial_after == 0) failures = 1;

    init_req(&req, port, clock_req, clock_len, response, sizeof(response));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &req.fn_io, 0) != 0) {
        printf("REOPEN FAIL io=%d nio=%u\n", (int)req.fn_io.io_Error,
               (unsigned)req.fn_nio_error);
        DeletePort(port);
        return RETURN_FAIL;
    }
    do_exchange(&req, port);
    printf("AFTER_OPENCNT0 io=%d nio=%u len=%u stage=%u result=%u cause=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2]);
    if (!clock_response_ok(&req, response)) failures = 1;

    bad_req[0] = 0x99;
    fill_exchange(&req, port, bad_req, 1, response, sizeof(response));
    do_exchange(&req, port);
    printf("TIMEOUT io=%d nio=%u len=%u stage=%u result=%u cause=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2]);
    if (req.fn_io.io_Error != 0 || req.fn_nio_error != FN_ERR_TIMEOUT ||
        req.fn_response_length != 0) {
        failures = 1;
    }

    serial_after = try_open_serial();
    printf("TIMEOUT_RESET serial-busy=%d\n", serial_after != 0);
    if (serial_after != 0) failures = 1;

    fill_exchange(&req, port, clock_req, clock_len, response, sizeof(response));
    do_exchange(&req, port);
    printf("RECOVERY io=%d nio=%u len=%u stage=%u result=%u cause=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2]);
    if (!clock_response_ok(&req, response)) failures = 1;

    CloseDevice(&req.fn_io);
    DeletePort(port);

    memset(&job_a, 0, sizeof(job_a));
    memset(&job_b, 0, sizeof(job_b));
    spawned_a = 0;
    spawned_b = 0;
    job_a.request_length = build_clock_cmd(job_a.request, FN_CMD_CLOCK_GET);
    job_b.request_length = build_clock_cmd(job_b.request, FN_CMD_CLOCK_GET_TZ);
    proc_a = CreateNewProcTags(NP_Entry, (ULONG)job_a_entry, NP_StackSize,
                               8192, NP_Name, (ULONG) "nio-exch-a", TAG_DONE);
    if (proc_a != NULL) spawned_a = 1;
    proc_b = CreateNewProcTags(NP_Entry, (ULONG)job_b_entry, NP_StackSize,
                               8192, NP_Name, (ULONG) "nio-exch-b", TAG_DONE);
    if (proc_b != NULL) spawned_b = 1;
    if (proc_a == NULL || proc_b == NULL) {
        printf("CONCURRENT spawn-fail\n");
        wait_spawned_jobs();
        return RETURN_FAIL;
    }
    for (spins = 0; spins < 400 && !(job_a.done && job_b.done); ++spins)
        Delay(1);
    if (!(job_a.done && job_b.done)) {
        printf("CONCURRENT wait-expire\n");
        return RETURN_FAIL;
    }
    printf("CONCURRENT a_io=%d a_nio=%u a_len=%u a_stage=%u a_result=%u a_cause=%u a_cmd=%u b_io=%d b_nio=%u b_len=%u b_stage=%u b_result=%u b_cause=%u b_cmd=%u\n",
           (int)job_a.io_error, (unsigned)job_a.nio_error,
           (unsigned)job_a.response_length, (unsigned)job_a.broker_stage,
           (unsigned)job_a.broker_result, (unsigned)job_a.broker_cause,
           (unsigned)job_a.response[1],
           (int)job_b.io_error, (unsigned)job_b.nio_error,
           (unsigned)job_b.response_length, (unsigned)job_b.broker_stage,
           (unsigned)job_b.broker_result, (unsigned)job_b.broker_cause,
           (unsigned)job_b.response[1]);
    if (!(job_a.io_error == 0 && job_b.io_error == 0 &&
          job_a.nio_error == FN_OK && job_b.nio_error == FN_OK &&
          job_a.response_length >= FN_HEADER_SIZE &&
          job_b.response_length >= FN_HEADER_SIZE &&
          job_a.response[0] == FN_DEVICE_CLOCK &&
          job_b.response[0] == FN_DEVICE_CLOCK &&
          job_a.response[1] == FN_CMD_CLOCK_GET &&
          job_b.response[1] == FN_CMD_CLOCK_GET_TZ)) {
        failures = 1;
    } else {
        serial_after = try_open_serial();
        printf("CONCURRENT serial-busy-after-concurrent=%d\n",
               serial_after != 0);
        if (serial_after == 0) failures = 1;
    }

    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;
    list_len = build_file_list(list_req, COMPLETION_URI);
    init_req(&req, port, list_req, list_len, response, sizeof(response));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &req.fn_io, 0) != 0) {
        DeletePort(port);
        return RETURN_FAIL;
    }
    do_exchange(&req, port);
    printf("MARKER io=%d nio=%u len=%u stage=%u result=%u cause=%u\n",
           (int)req.fn_io.io_Error, (unsigned)req.fn_nio_error,
           (unsigned)req.fn_response_length, (unsigned)req.fn_pad[0],
           (unsigned)req.fn_pad[1], (unsigned)req.fn_pad[2]);
    if (req.fn_io.io_Error != 0 || req.fn_nio_error != FN_OK) failures = 1;
    CloseDevice(&req.fn_io);
    DeletePort(port);

    if (failures) return RETURN_FAIL;
    printf("PASS isolated-exchange\n");
    return RETURN_OK;
}
