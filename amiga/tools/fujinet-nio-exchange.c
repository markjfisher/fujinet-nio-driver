#include <devices/serial.h>
#include <devices/timer.h>
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
#include "fujinet_nio_exchange_opts.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

#define MATRIX_PACKET_CAP 1024

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
    struct Device *device = req->fn_io.io_Device;
    struct Unit *unit = req->fn_io.io_Unit;

    memset(req, 0, sizeof(*req));
    req->fn_io.io_Device = device;
    req->fn_io.io_Unit = unit;
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

/* CreateNewProc children share this binary's seglist. Returning from main
 * while they still exist unloads their code; wait until Exec has RemTask'd
 * them. done flags are not enough. */
static void wait_spawned_tasks_gone(void)
{
    ULONG spins;

    for (spins = 0; spins < 200; ++spins) {
        if (FindTask((CONST_STRPTR)"nio-exch-a") == NULL &&
            FindTask((CONST_STRPTR)"nio-exch-b") == NULL)
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
    printf("ISOLATED disk.device=%s FLS=%s\n",
           disk_present ? "present" : "absent",
           fls_present ? "present" : "absent");
    /* Isolation is FindName/FindTask only. Do not open serial.device before
     * the first measured EXCHANGE. DiskDevice may already be resident on a
     * normal Workbench session. FLS remains a useful warning because it can
     * own the legacy serial transport, but it must not prevent diagnostics. */
    return 1;
}

static void write_le32(UBYTE *data, ULONG value)
{
    data[0] = (UBYTE)value;
    data[1] = (UBYTE)(value >> 8);
    data[2] = (UBYTE)(value >> 16);
    data[3] = (UBYTE)(value >> 24);
}

static ULONG read_le32(const UBYTE *data)
{
    return (ULONG)data[0] | ((ULONG)data[1] << 8) |
           ((ULONG)data[2] << 16) | ((ULONG)data[3] << 24);
}

static struct MsgPort *elapsed_port;
static struct timerequest *elapsed_req;
static uint8_t elapsed_ready;

static void close_elapsed_timer(void)
{
    if (elapsed_req != NULL) {
        if (elapsed_ready) CloseDevice((struct IORequest *)elapsed_req);
        DeleteExtIO((struct IORequest *)elapsed_req);
        elapsed_req = NULL;
    }
    elapsed_ready = 0;
    if (elapsed_port != NULL) {
        DeletePort(elapsed_port);
        elapsed_port = NULL;
    }
}

static void open_elapsed_timer(void)
{
    elapsed_port = CreatePort(NULL, 0);
    if (elapsed_port == NULL) return;
    elapsed_req = (struct timerequest *)CreateExtIO(
        elapsed_port, sizeof(*elapsed_req));
    if (elapsed_req == NULL) {
        DeletePort(elapsed_port);
        elapsed_port = NULL;
        return;
    }
    /* MICROHZ/VBLANK + TR_GETSYSTIME. Do not declare a global TimerBase or
     * call proto/timer.h ReadEClock: clib2 uses that symbol as its C clock. */
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)elapsed_req, 0) != 0 &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)elapsed_req, 0) != 0) {
        close_elapsed_timer();
        return;
    }
    elapsed_ready = 1;
}

static void attach_open(struct FujiNetNIORequest *req,
                        const struct IORequest *open_request)
{
    req->fn_io.io_Device = open_request->io_Device;
    req->fn_io.io_Unit = open_request->io_Unit;
}

static void fill_control(struct FujiNetNIORequest *req, struct MsgPort *port,
                         UWORD command, const uint8_t *request,
                         UWORD request_len, uint8_t *response,
                         UWORD response_cap)
{
    memset(req, 0, sizeof(*req));
    req->fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->fn_io.io_Message.mn_ReplyPort = port;
    req->fn_io.io_Message.mn_Length = sizeof(*req);
    req->fn_io.io_Command = command;
    req->fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
    req->fn_request_data = request;
    req->fn_request_length = request_len;
    req->fn_response_data = response;
    req->fn_response_capacity = response_cap;
}

static LONG do_control(struct FujiNetNIORequest *req, struct MsgPort *port,
                       const struct IORequest *open_request, UWORD command,
                       const uint8_t *request, UWORD request_len,
                       uint8_t *response, UWORD response_cap)
{
    fill_control(req, port, command, request, request_len, response,
                 response_cap);
    attach_open(req, open_request);
    return DoIO(&req->fn_io);
}

static int snapshot_time(struct timeval *tv)
{
    if (!elapsed_ready || elapsed_req == NULL) return -1;
    elapsed_req->tr_node.io_Command = TR_GETSYSTIME;
    elapsed_req->tr_node.io_Flags = 0;
    elapsed_req->tr_node.io_Error = 0;
    elapsed_req->tr_time.tv_secs = 0;
    elapsed_req->tr_time.tv_micro = 0;
    if (DoIO((struct IORequest *)elapsed_req) != 0) return -1;
    *tv = elapsed_req->tr_time;
    return 0;
}

static LONG do_measured_exchange(struct FujiNetNIORequest *req,
                                 char *elapsed_buf)
{
    struct timeval start;
    struct timeval end;
    LONG rc;
    int have_start;

    have_start = snapshot_time(&start) == 0;
    rc = DoIO(&req->fn_io);
    if (!have_start || snapshot_time(&end) != 0) {
        fn_nio_exchange_format_elapsed(0, 0, elapsed_buf, 32);
        return rc;
    }
    {
        unsigned long us;
        long sec = (long)end.tv_secs - (long)start.tv_secs;
        long micro = (long)end.tv_micro - (long)start.tv_micro;

        if (micro < 0) {
            micro += 1000000L;
            sec -= 1;
        }
        if (sec < 0) {
            fn_nio_exchange_format_elapsed(0, 0, elapsed_buf, 32);
            return rc;
        }
        if (sec > 4294L)
            us = 0xFFFFFFFFUL;
        else
            us = (unsigned long)sec * 1000000UL + (unsigned long)micro;
        if (fn_nio_exchange_format_elapsed(1, us, elapsed_buf, 32) != 0)
            fn_nio_exchange_format_elapsed(0, 0, elapsed_buf, 32);
    }
    return rc;
}

static void print_trial_log(const struct FujiNetNIORequest *req,
                            int backend, const char *elapsed)
{
    char line[192];

    if (fn_nio_exchange_format_trial_log(
            line, sizeof(line), (unsigned)req->fn_request_length,
            (unsigned)req->fn_response_length, elapsed,
            (unsigned)req->fn_pad[1], (unsigned)req->fn_pad[2],
            (unsigned)(req->fn_flags & 0xFF),
            (unsigned)(req->fn_flags >> 8), backend) != 0) {
        return;
    }
    printf("%s\n", line);
}

static void print_matrix_usage(void)
{
    fprintf(stderr,
            "Usage: fujinet-nio-exchange --type clock|host-get|file-list "
            "--backend cold|warm [--baud 9600|19200|38400] "
            "[--size 8|16|32|64|128|256|420|512 --uri URI] [--trials N]\n");
}

static int run_set_baud(struct FujiNetNIORequest *req, struct MsgPort *port,
                        const struct IORequest *open_request,
                        unsigned long baud, uint8_t *baud_bytes)
{
    unsigned long set_baud = baud;

    if (set_baud == 0UL) {
        if (do_control(req, port, open_request, FUJINET_NIO_CMD_GET_BAUD, NULL,
                       0, baud_bytes, 4) != 0 ||
            req->fn_nio_error != 0 || req->fn_response_length != 4) {
            fprintf(stderr, "GET_BAUD failed\n");
            return -1;
        }
        set_baud = read_le32(baud_bytes);
    }
    write_le32(baud_bytes, set_baud);
    if (do_control(req, port, open_request, FUJINET_NIO_CMD_SET_BAUD,
                   baud_bytes, 4, NULL, 0) != 0 ||
        req->fn_nio_error != 0) {
        fprintf(stderr, "SET_BAUD failed\n");
        return -1;
    }
    return 0;
}

static int run_get_baud_match(struct FujiNetNIORequest *req,
                              struct MsgPort *port,
                              const struct IORequest *open_request,
                              unsigned long want, uint8_t *baud_bytes)
{
    unsigned long got;

    if (do_control(req, port, open_request, FUJINET_NIO_CMD_GET_BAUD, NULL, 0,
                   baud_bytes, 4) != 0 ||
        req->fn_nio_error != 0 || req->fn_response_length != 4) {
        fprintf(stderr, "GET_BAUD failed\n");
        return -1;
    }
    got = read_le32(baud_bytes);
    if (!fn_nio_exchange_warm_baud_ok(want, got)) {
        fprintf(stderr, "WARM baud mismatch got=%lu want=%lu\n", got, want);
        return -1;
    }
    return 0;
}

static int run_warmup(struct FujiNetNIORequest *req, struct MsgPort *port,
                      const struct IORequest *open_request,
                      const uint8_t *clock_req, int clock_len,
                      uint8_t *response, unsigned response_cap)
{
    fill_exchange(req, port, clock_req, (UWORD)clock_len, response,
                  (UWORD)response_cap);
    attach_open(req, open_request);
    if (DoIO(&req->fn_io) != 0 || !clock_response_ok(req, response)) {
        printf("WARMUP io=%d nio=%u len=%u result=%u cause=%u native=%u "
               "status=%u\n",
               (int)req->fn_io.io_Error, (unsigned)req->fn_nio_error,
               (unsigned)req->fn_response_length, (unsigned)req->fn_pad[1],
               (unsigned)req->fn_pad[2], (unsigned)(req->fn_flags & 0xFF),
               (unsigned)(req->fn_flags >> 8));
        return -1;
    }
    return 0;
}

static int run_matrix(int argc, char **argv)
{
    struct fn_nio_exchange_opts opts;
    struct MsgPort *port;
    struct IORequest open_request;
    struct FujiNetNIORequest req;
    uint8_t request[MATRIX_PACKET_CAP];
    uint8_t response[MATRIX_PACKET_CAP];
    uint8_t clock_req[FN_HEADER_SIZE];
    uint8_t baud_bytes[4];
    int steps[4];
    int nsteps;
    int request_len;
    int clock_len;
    unsigned trial;
    int status = RETURN_OK;

    if (fn_nio_exchange_opts_parse(argc, argv, &opts) != 0) {
        print_matrix_usage();
        return RETURN_ERROR;
    }

    nsteps = fn_nio_exchange_opts_plan(&opts, steps, 4);
    if (nsteps < 0) return RETURN_FAIL;

    clock_len = fn_nio_exchange_build_clock_get(clock_req, sizeof(clock_req));
    if (clock_len < 0) return RETURN_FAIL;
    if (opts.type == FN_NIO_EXCHANGE_TYPE_CLOCK) {
        request_len = fn_nio_exchange_build_clock_get(request, sizeof(request));
    } else if (opts.type == FN_NIO_EXCHANGE_TYPE_HOST_GET) {
        request_len = fn_nio_exchange_build_host_get(request, sizeof(request));
    } else {
        request_len = fn_nio_exchange_build_file_list(
            request, sizeof(request), opts.uri, opts.size);
    }
    if (request_len < 0) return RETURN_FAIL;

    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;
    memset(&open_request, 0, sizeof(open_request));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &open_request, 0) != 0) {
        DeletePort(port);
        fprintf(stderr, "Cannot open %s\n", FUJINET_NIO_DEVICE_NAME);
        return RETURN_FAIL;
    }

    open_elapsed_timer();

    for (trial = 0; trial < opts.trials; ++trial) {
        char elapsed[32];
        int si;
        int abort_trials = 0;

        for (si = 0; si < nsteps; ++si) {
            int step = steps[si];
            int step_failed = 0;

            if (step == FN_NIO_EXCHANGE_STEP_SET_BAUD) {
                step_failed = run_set_baud(&req, port, &open_request, opts.baud,
                                           baud_bytes);
            } else if (step == FN_NIO_EXCHANGE_STEP_GET_BAUD) {
                step_failed = run_get_baud_match(&req, port, &open_request,
                                                 opts.baud, baud_bytes);
            } else if (step == FN_NIO_EXCHANGE_STEP_WARMUP) {
                step_failed = run_warmup(&req, port, &open_request, clock_req,
                                         clock_len, response,
                                         sizeof(response));
            } else if (step == FN_NIO_EXCHANGE_STEP_MEASURE) {
                fill_exchange(&req, port, request, (UWORD)request_len,
                              response, sizeof(response));
                attach_open(&req, &open_request);
                do_measured_exchange(&req, elapsed);
                print_trial_log(&req, opts.backend, elapsed);
                if (req.fn_io.io_Error != 0 || req.fn_nio_error != FN_OK)
                    step_failed = -1;
            } else {
                step_failed = -1;
            }

            if (step_failed != 0) {
                status = RETURN_FAIL;
                if (fn_nio_exchange_step_failure_aborts(step)) {
                    abort_trials = 1;
                    break;
                }
                /* Skip MEASURE this trial; the next trial WARMUPs again on a
                 * backend that TRANSPORT/TIMEOUT closed. */
                if (step == FN_NIO_EXCHANGE_STEP_WARMUP) break;
            }
        }
        if (abort_trials) break;
    }

    close_elapsed_timer();
    CloseDevice(&open_request);
    DeletePort(port);
    return status;
}

int main(int argc, char **argv)
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

    /* Redirected logs must not sit in a full stdio buffer across a crash. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2) return run_matrix(argc, argv);

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
    /* native = serial.device io_Error; status-hi = high byte of io_Status:
     * 1=IO_STATF_OVERRUN, 2=IO_STATF_FRAMEERROR, 4=IO_STATF_PARITYERR.
     * cause=9 means flush drained IO_STATF_OVERRUN before CMD_WRITE but
     * CMD_READ still failed; cause=7 means flush drain never triggered. */
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

    wait_spawned_tasks_gone();

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

    wait_spawned_tasks_gone();
    if (failures) return RETURN_FAIL;
    printf("PASS isolated-exchange\n");
    return RETURN_OK;
}
