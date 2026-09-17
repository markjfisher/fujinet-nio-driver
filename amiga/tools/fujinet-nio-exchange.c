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
#include "fujinet_nio_endian.h"
#include "fujinet_nio_exchange_opts.h"
#include "fujinet_nio_serial_config.h"
#include "fujinet_nio_backend.h"
#include "fujinet_disk_device.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"
#include "fn_slip.h"
#include "fujinet_nio_session_diag.h"

#define MATRIX_PACKET_CAP 1024
#define DISK_PROVOCATION_SECTOR 512

/* clib2. Default Shell STACK is 4096; this binary's frames overflow that
 * and Guru #80000006 (CHK). See docs/amiga/cli-stack-and-iorequest.md. */
long __stack = 16384;

static uint8_t matrix_request[MATRIX_PACKET_CAP];
static uint8_t matrix_response[MATRIX_PACKET_CAP];
static uint8_t list_golden[MATRIX_PACKET_CAP];
static unsigned list_golden_len;
static uint8_t list_expect_slip[(MATRIX_PACKET_CAP * 2) + 2];

#define COMPLETION_URI "host:/amiga-e2e-complete/nio-broker-isolated"

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
    if (response != NULL && response_cap != 0)
        memset(response, 0, response_cap);
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

static char probe_serial_name[FUJINET_NIO_SERIAL_NAME_MAX + 1] =
    "serial.device";
static uint32_t probe_serial_unit;

static void reclaim_io(struct IORequest *io);

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
    result = OpenDevice((CONST_STRPTR)probe_serial_name, probe_serial_unit,
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
    reclaim_io(&req.fn_io);
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

static struct MsgPort *elapsed_port;
static struct timerequest *elapsed_req;
static uint8_t elapsed_ready;

/* WaitIO only if CheckIO says the request is still outstanding. A second
 * WaitIO after DoIO has already taken the reply can pull the next message
 * or hang. AbortIO on an idle request is a no-op. Do not call this on an
 * IORequest used only for OpenDevice. */
static void reclaim_io(struct IORequest *io)
{
    if (io == NULL || io->io_Device == NULL) return;
    if (CheckIO(io) == NULL) {
        AbortIO(io);
        WaitIO(io);
    }
}

static void close_elapsed_timer(void)
{
    if (elapsed_req != NULL) {
        if (elapsed_ready) {
            reclaim_io((struct IORequest *)elapsed_req);
            CloseDevice((struct IORequest *)elapsed_req);
        }
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

static void print_c0_diag(const struct FujiNetNIORequest *req)
{
    unsigned native = (unsigned)(req->fn_flags & 0xFF);
    unsigned ov = (unsigned)(req->fn_flags >> 8);
    const char *cls;

    if (req->fn_pad[1] == 0) return;
    if (req->fn_pad[2] != FUJINET_NIO_DETAIL_SESSION_IO &&
        req->fn_pad[2] != FUJINET_NIO_DETAIL_SERIAL_READ &&
        req->fn_pad[2] !=
            FUJINET_NIO_DETAIL_FLUSH_DRAINED_THEN_READ_FAILED)
        return;
    /* io_Error values are small; SLIP END is 0xC0. */
    if (native != 0 && native < 32U && ov == 0) {
        printf("isr_native_io_error=%u\n", native);
        return;
    }
    if (native == 0xC0U)
        cls = "A";
    else if (ov != 0)
        cls = "C";
    else
        cls = "B";
    printf("isr_first=%02x hw_ov=%u c0_class=%s\n", native, ov, cls);
}

static void print_fujibus_prefix(const uint8_t *buf, unsigned len)
{
    unsigned n;
    unsigned i;

    if (buf == NULL || len == 0) {
        printf("fujibus=-\n");
        return;
    }
    n = len;
    if (n > 32) n = 32;
    printf("fujibus=");
    for (i = 0; i < n; ++i) {
        if (i != 0) putchar(' ');
        printf("%02x", (unsigned)buf[i]);
    }
    printf("\n");
}

static void print_hex_span(const char *label, const uint8_t *buf, unsigned n)
{
    unsigned i;

    printf("%s", label);
    if (buf == NULL || n == 0) {
        printf("-\n");
        return;
    }
    for (i = 0; i < n; ++i) {
        if (i != 0) putchar(' ');
        printf("%02x", (unsigned)buf[i]);
    }
    printf("\n");
}

static void print_mismatch_context(const uint8_t *got, unsigned got_len,
                                   const uint8_t *exp, unsigned exp_len,
                                   int mis)
{
    unsigned start;
    unsigned i;
    unsigned n;

    if (mis < 0) return;
    start = ((unsigned)mis >= 4U) ? (unsigned)mis - 4U : 0U;
    printf("got@%u=", start);
    n = (got_len > start) ? got_len - start : 0U;
    if (n > 12U) n = 12U;
    if (got == NULL || n == 0) {
        printf("-\n");
    } else {
        for (i = 0; i < n; ++i) {
            if (i != 0) putchar(' ');
            printf("%02x", (unsigned)got[start + i]);
        }
        printf("\n");
    }
    printf("exp@%u=", start);
    n = (exp_len > start) ? exp_len - start : 0U;
    if (n > 12U) n = 12U;
    if (exp == NULL || n == 0) {
        printf("-\n");
    } else {
        for (i = 0; i < n; ++i) {
            if (i != 0) putchar(' ');
            printf("%02x", (unsigned)exp[start + i]);
        }
        printf("\n");
    }
}

static void print_file_list_diff(const uint8_t *got, unsigned got_len)
{
    const uint8_t list_head[3] = { 0xC0, 0xFE, 0x02 };
    const uint8_t *exp;
    unsigned exp_len;
    int mis;
    unsigned exp_b;
    unsigned got_b;

    if (list_golden_len != 0) {
        exp_len = fn_slip_encode(list_golden, (uint16_t)list_golden_len,
                                 list_expect_slip);
        exp = list_expect_slip;
    } else {
        exp = list_head;
        exp_len = 3;
        printf("golden=- (need one OK file-list for a full-byte diff)\n");
    }

    mis = fn_nio_session_diag_first_mismatch(got, got_len, exp, exp_len);
    if (mis < 0) {
        printf("mismatch=-\n");
        return;
    }
    exp_b = ((unsigned)mis < exp_len) ? exp[mis] : 0U;
    got_b = ((unsigned)mis < got_len) ? got[mis] : 0U;
    printf("mismatch=%d exp=%02x got=%02x\n", mis, exp_b, got_b);
    print_mismatch_context(got, got_len, exp, exp_len, mis);

    /* Opening C0 FE is already gone: diff against FujiBus after that so a
     * later hole (missing descriptor 0x01) is not hidden at offset 0. */
    if (exp_len >= 3U && exp[0] == 0xC0U && exp[1] == 0xFEU &&
        got_len >= 1U && got[0] == 0x02U) {
        mis = fn_nio_session_diag_first_mismatch(got, got_len, exp + 2,
                                                 exp_len - 2U);
        if (mis < 0) {
            printf("after_prefix mismatch=-\n");
        } else {
            exp_b = ((unsigned)mis < exp_len - 2U) ? exp[2U + (unsigned)mis] : 0U;
            got_b = ((unsigned)mis < got_len) ? got[mis] : 0U;
            printf("after_prefix mismatch=%d exp=%02x got=%02x\n",
                   mis, exp_b, got_b);
            print_mismatch_context(got, got_len, exp + 2, exp_len - 2U, mis);
        }
    }
}

static void print_wire_peek(const uint8_t *buf, int file_list)
{
    fn_nio_session_diag_t d;
    const uint8_t *exp = NULL;
    unsigned exp_len = 0;
    unsigned show;
    int align;
    unsigned n;
    unsigned i;

    if (buf == NULL) return;
    if (fn_nio_session_diag_parse(buf, MATRIX_PACKET_CAP, &d) != 0) {
        n = buf[0];
        if (n == 0 || n > 32) {
            printf("peek=-\n");
            return;
        }
        printf("slip class=legacy-peek (fujinet-nio.device 0.7 not loaded)\n");
        printf("peek=");
        for (i = 0; i < n; ++i) {
            if (i != 0) putchar(' ');
            printf("%02x", (unsigned)buf[1 + i]);
        }
        printf("\n");
        if (file_list)
            print_file_list_diff(buf + 1, n);
        return;
    }

    printf("slip class=%s raw=%u copy=%u decoded=%u pkt=%u c0=%u last=%02x "
           "first=%02x %02x %02x leftover=%u\n",
           fn_nio_session_diag_class(&d), (unsigned)d.raw_len,
           (unsigned)d.raw_copy_len, (unsigned)d.decoded_len,
           (unsigned)d.pkt_len, (unsigned)d.c0_count,
           (unsigned)d.last_byte, (unsigned)d.first3[0],
           (unsigned)d.first3[1], (unsigned)d.first3[2],
           (unsigned)d.leftover_len);
    show = d.raw_copy_len;
    if (show > 32U) show = 32U;
    print_hex_span("peek=", d.raw, show);

    if (file_list)
        print_file_list_diff(d.raw, d.raw_copy_len);

    print_hex_span("ring=", d.leftover, d.leftover_len);
    if (file_list && list_golden_len != 0 && d.leftover_len != 0) {
        exp_len = fn_slip_encode(list_golden, (uint16_t)list_golden_len,
                                 list_expect_slip);
        exp = list_expect_slip;
        align = fn_nio_session_diag_ring_align(d.leftover, d.leftover_len, exp,
                                               exp_len);
        printf("ring_align=%d\n", align);
    }
}

static void print_matrix_usage(void)
{
    fprintf(stderr,
            "Usage: fujinet-nio-exchange --type clock|host-get|file-list "
            "--backend cold|warm [--installed-backend serial|native] "
            "[--baud 300..230400] "
            "[--serial-device NAME] [--serial-unit 0..255] "
            "[--size 8|16|32|64|128|256|420|512 --uri URI] "
            "[--list-flags 0..255] [--trials N]\n"
            "       fujinet-nio-exchange --type disk-read|disk-write "
            "--provocation --backend cold --baud 300..230400 --slot 1..8 "
            "--lba N [--serial-device NAME] [--serial-unit 0..255] "
            "[--trials N]\n"
            "Installed backend declares existing hardware; it does not detect or select it.\n"
            "Backend cold|warm controls lifecycle; installed backend defaults to serial.\n"
            "Native requires warm clock or file-list; cold reset is unsupported.\n"
            "Native rejects baud, serial, host-get, disk and provocation options.\n");
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
        set_baud = fujinet_nio_get_le32(baud_bytes);
    }
    fujinet_nio_put_le32(baud_bytes, set_baud);
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
    got = fujinet_nio_get_le32(baud_bytes);
    if (!fn_nio_exchange_warm_baud_ok(want, got)) {
        fprintf(stderr, "WARM baud mismatch got=%lu want=%lu\n", got, want);
        return -1;
    }
    return 0;
}

static int run_set_serial(struct FujiNetNIORequest *req, struct MsgPort *port,
                          const struct IORequest *open_request,
                          const char *name, unsigned long unit)
{
    uint8_t payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    uint16_t payload_len;

    if (fujinet_nio_serial_encode(payload, sizeof(payload), &payload_len,
                                  (uint32_t)unit, name) != FN_OK) {
        fprintf(stderr, "SET_SERIAL encode failed\n");
        return -1;
    }
    if (do_control(req, port, open_request, FUJINET_NIO_CMD_SET_SERIAL,
                   payload, payload_len, NULL, 0) != 0 ||
        req->fn_nio_error != 0) {
        fprintf(stderr, "SET_SERIAL failed\n");
        return -1;
    }
    return 0;
}

static int run_get_serial_match(struct FujiNetNIORequest *req,
                                struct MsgPort *port,
                                const struct IORequest *open_request,
                                const char *want_name, unsigned long want_unit)
{
    uint8_t payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    char got_name[FUJINET_NIO_SERIAL_NAME_MAX + 1];
    uint32_t got_unit;

    if (do_control(req, port, open_request, FUJINET_NIO_CMD_GET_SERIAL, NULL, 0,
                   payload, sizeof(payload)) != 0 ||
        req->fn_nio_error != 0 ||
        fujinet_nio_serial_decode(payload, req->fn_response_length, &got_unit,
                                  got_name, sizeof(got_name)) != FN_OK) {
        fprintf(stderr, "GET_SERIAL failed\n");
        return -1;
    }
    if (got_unit != (uint32_t)want_unit || strcmp(got_name, want_name) != 0) {
        fprintf(stderr,
                "WARM serial mismatch got=%s unit=%lu want=%s unit=%lu\n",
                got_name, (unsigned long)got_unit, want_name, want_unit);
        return -1;
    }
    return 0;
}

static int cache_serial_probe(const struct IORequest *open_request,
                              struct MsgPort *port)
{
    struct FujiNetNIORequest req;
    uint8_t payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    char name[FUJINET_NIO_SERIAL_NAME_MAX + 1];
    uint32_t unit;

    if (do_control(&req, port, open_request, FUJINET_NIO_CMD_GET_SERIAL, NULL, 0,
                   payload, sizeof(payload)) != 0 ||
        req.fn_nio_error != 0 ||
        fujinet_nio_serial_decode(payload, req.fn_response_length, &unit, name,
                                  sizeof(name)) != FN_OK) {
        return -1;
    }
    strcpy(probe_serial_name, name);
    probe_serial_unit = unit;
    return 0;
}

static int run_disk_provocation(const struct fn_nio_exchange_opts *opts)
{
    struct MsgPort *port;
    struct IORequest nio_open;
    struct FujiNetNIORequest nio_req;
    struct IOStdReq disk;
    struct Device *disk_device;
    struct Unit *disk_unit;
    struct fujinet_disk_trace trace;
    uint8_t baud_bytes[4];
    uint8_t buffer[DISK_PROVOCATION_SECTOR];
    unsigned trial;
    int is_write = opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE;
    int failures = 0;

    printf("PROVOCATION baud=%lu backend=cold pacing=tx_byte_gap_us:0 "
           "tx_chunk_size:0 tx_chunk_gap_us:0 op=%s slot=%u lba=%lu "
           "trials=%u\n", opts->baud, is_write ? "WRITE" : "READ", opts->slot,
           (unsigned long)opts->lba, opts->trials);
    printf("PROVOCATION requires ESP uart.set tx_byte_gap_us=0 "
           "tx_chunk_size=0 tx_chunk_gap_us=0; restore 16/2000 after run\n");

    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;
    memset(&nio_open, 0, sizeof(nio_open));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &nio_open, 0) != 0) {
        DeletePort(port);
        return RETURN_FAIL;
    }
    memset(&disk, 0, sizeof(disk));
    disk.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    disk.io_Message.mn_ReplyPort = port;
    disk.io_Message.mn_Length = sizeof(disk);
    if (OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME,
                   (ULONG)(opts->slot - 1),
                   (struct IORequest *)&disk, 0) != 0) {
        CloseDevice(&nio_open);
        DeletePort(port);
        return RETURN_FAIL;
    }
    disk_device = disk.io_Device;
    disk_unit = disk.io_Unit;

    disk.io_Command = FUJINET_DISK_CMD_TRACE_CLEAR;
    disk.io_Data = NULL;
    disk.io_Length = 0;
    (void)DoIO((struct IORequest *)&disk);
    open_elapsed_timer();

    for (trial = 0; trial < opts->trials; ++trial) {
        ULONG trace_index;
        UBYTE attempt;
        char elapsed[32];

        if (opts->serial_device != NULL &&
            run_set_serial(&nio_req, port, &nio_open, opts->serial_device,
                           opts->serial_unit) != 0) {
            failures = 1;
            break;
        }
        if (run_set_baud(&nio_req, port, &nio_open, opts->baud,
                         baud_bytes) != 0) {
            failures = 1;
            break;
        }
        for (trace_index = 0; trace_index < DISK_PROVOCATION_SECTOR; ++trace_index)
            buffer[trace_index] = (uint8_t)(trace_index ^ 0x5A);
        memset(&disk, 0, sizeof(disk));
        disk.io_Device = disk_device;
        disk.io_Unit = disk_unit;
        disk.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        disk.io_Message.mn_ReplyPort = port;
        disk.io_Message.mn_Length = sizeof(disk);
        disk.io_Command = is_write ? CMD_WRITE : CMD_READ;
        disk.io_Data = buffer;
        disk.io_Length = DISK_PROVOCATION_SECTOR;
        disk.io_Offset = (ULONG)opts->lba * DISK_PROVOCATION_SECTOR;
        {
            struct timeval start;
            struct timeval end;
            int have_start = snapshot_time(&start) == 0;
            (void)DoIO((struct IORequest *)&disk);
            if (have_start && snapshot_time(&end) == 0) {
                unsigned long us;
                long sec = (long)end.tv_secs - (long)start.tv_secs;
                long micro = (long)end.tv_micro - (long)start.tv_micro;
                if (micro < 0) { micro += 1000000L; --sec; }
                us = sec >= 0 ? (unsigned long)sec * 1000000UL +
                               (unsigned long)micro : 0;
                fn_nio_exchange_format_elapsed(1, us, elapsed, 32);
            } else {
                fn_nio_exchange_format_elapsed(0, 0, elapsed, 32);
            }
        }

        memset(&trace, 0, sizeof(trace));
        disk.io_Command = FUJINET_DISK_CMD_TRACE;
        disk.io_Data = &trace;
        disk.io_Length = sizeof(trace);
        (void)DoIO((struct IORequest *)&disk);
        if (trace.count == 0) {
            printf("trial=%u trace=missing io_error=%d io_actual=%lu\n",
                   trial + 1, (int)disk.io_Error,
                   (unsigned long)disk.io_Actual);
            failures = 1;
            continue;
        }
        trace_index = trace.count - 1;
        for (attempt = 0; attempt < trace.exchange_attempts[trace_index]; ++attempt) {
            printf("trial=%u baud=%lu cold=1 pacing=0/0/0 op=%s slot=%u "
                   "lba=%lu req_len=%u resp_len=%u elapsed_us=%s result=%u "
                   "cause=%u native=%u status=%u attempt=%u/%u io_Error=%d "
                   "io_Actual=%lu write_pattern= i^0x5a\n", trial + 1,
                   opts->baud, is_write ? "WRITE" : "READ", opts->slot,
                   (unsigned long)opts->lba,
                   is_write ? 526U : 14U,
                   (unsigned)trace.exchange_response_lengths[trace_index][attempt],
                   elapsed,
                   (unsigned)trace.exchange_results[trace_index][attempt],
                   (unsigned)trace.exchange_causes[trace_index][attempt],
                   (unsigned)trace.exchange_native_errors[trace_index][attempt],
                   (unsigned)trace.exchange_statuses[trace_index][attempt],
                   (unsigned)(attempt + 1),
                   (unsigned)trace.exchange_attempts[trace_index],
                   (int)trace.errors[trace_index],
                   (unsigned long)trace.actuals[trace_index]);
        }
        if (trace.errors[trace_index] == 0 &&
            (trace.exchange_attempts[trace_index] == 0 ||
             trace.exchange_attempts[trace_index] > FUJINET_DISK_TRACE_ATTEMPTS ||
             trace.actuals[trace_index] != DISK_PROVOCATION_SECTOR ||
             trace.exchange_results[trace_index][trace.exchange_attempts[trace_index] - 1] != FN_OK ||
             trace.exchange_response_lengths[trace_index][trace.exchange_attempts[trace_index] - 1] !=
                 (is_write ? 17U : 529U)))
            failures = 1;
    }

    close_elapsed_timer();
    reclaim_io((struct IORequest *)&disk);
    CloseDevice((struct IORequest *)&disk);
    CloseDevice(&nio_open);
    DeletePort(port);
    return failures ? RETURN_FAIL : RETURN_OK;
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
    uint8_t clock_req[FN_HEADER_SIZE];
    uint8_t baud_bytes[4];
    int steps[5];
    int nsteps;
    int request_len;
    int clock_len;
    unsigned trial;
    int status = RETURN_OK;

    if (fn_nio_exchange_opts_parse(argc, argv, &opts) != 0) {
        print_matrix_usage();
        return RETURN_ERROR;
    }
    printf("installed_backend=%s lifecycle=%s\n",
           opts.installed_backend == FN_NIO_EXCHANGE_INSTALLED_NATIVE ?
               "native" : "serial",
           opts.backend == FN_NIO_EXCHANGE_BACKEND_COLD ? "cold" : "warm");
    list_golden_len = 0;

    if (opts.type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
        opts.type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE)
        return run_disk_provocation(&opts);

    nsteps = fn_nio_exchange_opts_plan(&opts, steps, 5);
    if (nsteps < 0) return RETURN_FAIL;

    clock_len = fn_nio_exchange_build_clock_get(clock_req, sizeof(clock_req));
    if (clock_len < 0) return RETURN_FAIL;
    if (opts.type == FN_NIO_EXCHANGE_TYPE_CLOCK) {
        request_len = fn_nio_exchange_build_clock_get(
            matrix_request, sizeof(matrix_request));
    } else if (opts.type == FN_NIO_EXCHANGE_TYPE_HOST_GET) {
        request_len = fn_nio_exchange_build_host_get(
            matrix_request, sizeof(matrix_request));
    } else {
        request_len = fn_nio_exchange_build_file_list(
            matrix_request, sizeof(matrix_request), opts.uri, opts.size,
            opts.has_list_flags ? (int)opts.list_flags : -1);
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

    memset(&req, 0, sizeof(req));
    open_elapsed_timer();

    for (trial = 0; trial < opts.trials; ++trial) {
        char elapsed[32];
        int si;
        int abort_trials = 0;

        for (si = 0; si < nsteps; ++si) {
            int step = steps[si];
            int step_failed = 0;

            if (step == FN_NIO_EXCHANGE_STEP_SET_SERIAL) {
                step_failed = run_set_serial(&req, port, &open_request,
                                             opts.serial_device,
                                             opts.serial_unit);
            } else if (step == FN_NIO_EXCHANGE_STEP_GET_SERIAL) {
                step_failed = run_get_serial_match(&req, port, &open_request,
                                                   opts.serial_device,
                                                   opts.serial_unit);
            } else if (step == FN_NIO_EXCHANGE_STEP_SET_BAUD) {
                step_failed = run_set_baud(&req, port, &open_request, opts.baud,
                                           baud_bytes);
            } else if (step == FN_NIO_EXCHANGE_STEP_GET_BAUD) {
                step_failed = run_get_baud_match(&req, port, &open_request,
                                                 opts.baud, baud_bytes);
            } else if (step == FN_NIO_EXCHANGE_STEP_WARMUP) {
                step_failed = run_warmup(&req, port, &open_request, clock_req,
                                         clock_len, matrix_response,
                                         sizeof(matrix_response));
            } else if (step == FN_NIO_EXCHANGE_STEP_MEASURE) {
                fill_exchange(&req, port, matrix_request, (UWORD)request_len,
                              matrix_response, sizeof(matrix_response));
                attach_open(&req, &open_request);
                do_measured_exchange(&req, elapsed);
                print_trial_log(&req, opts.backend, elapsed);
                if (opts.installed_backend == FN_NIO_EXCHANGE_INSTALLED_SERIAL &&
                    req.fn_pad[1] != 0)
                    print_c0_diag(&req);
                if (opts.installed_backend == FN_NIO_EXCHANGE_INSTALLED_SERIAL &&
                    (req.fn_pad[2] == FUJINET_NIO_DETAIL_SESSION_IO ||
                    req.fn_pad[2] == FUJINET_NIO_DETAIL_TIMEOUT ||
                    req.fn_pad[2] == FUJINET_NIO_DETAIL_SERIAL_READ ||
                    req.fn_pad[2] ==
                        FUJINET_NIO_DETAIL_FLUSH_DRAINED_THEN_READ_FAILED))
                    print_wire_peek(matrix_response,
                                    opts.type == FN_NIO_EXCHANGE_TYPE_FILE_LIST);
                if (req.fn_io.io_Error != 0 || req.fn_nio_error != FN_OK)
                    step_failed = -1;
                else if (fn_nio_exchange_verify_fujibus(
                             matrix_request, (unsigned)request_len,
                             matrix_response,
                             (unsigned)req.fn_response_length) != 0) {
                    printf("fujibus=bad\n");
                    print_fujibus_prefix(matrix_response,
                                         (unsigned)req.fn_response_length);
                    step_failed = -1;
                } else if (opts.type == FN_NIO_EXCHANGE_TYPE_FILE_LIST &&
                           req.fn_response_length > 0 &&
                           req.fn_response_length <= sizeof(list_golden)) {
                    memcpy(list_golden, matrix_response,
                           req.fn_response_length);
                    list_golden_len = (unsigned)req.fn_response_length;
                }
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

    reclaim_io(&req.fn_io);
    CloseDevice(&open_request);
    close_elapsed_timer();
    DeletePort(port);
    return status;
}

static int run_isolation_suite(void)
{
    struct MsgPort *port;
    struct FujiNetNIORequest req;
    uint8_t clock_req[FN_HEADER_SIZE];
    uint8_t bad_req[1];
    uint8_t list_req[128];
    uint8_t response[256];
    int clock_len;
    int list_len;
    LONG serial_after;
    ULONG spins;
    struct Process *proc_a;
    struct Process *proc_b;
    int packet_len;
    int failures = 0;

    if (!isolation_ok()) return RETURN_FAIL;

    clock_len = fn_nio_exchange_build_clock_get(clock_req, sizeof(clock_req));
    if (clock_len < 0) return RETURN_FAIL;
    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;

    init_req(&req, port, clock_req, clock_len, response, sizeof(response));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &req.fn_io, 0) != 0) {
        printf("OPEN FAIL io=%d nio=%u\n", (int)req.fn_io.io_Error,
               (unsigned)req.fn_nio_error);
        DeletePort(port);
        return RETURN_FAIL;
    }
    (void)cache_serial_probe(&req.fn_io, port);

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

    reclaim_io(&req.fn_io);
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

    reclaim_io(&req.fn_io);
    CloseDevice(&req.fn_io);
    DeletePort(port);

    memset(&job_a, 0, sizeof(job_a));
    memset(&job_b, 0, sizeof(job_b));
    spawned_a = 0;
    spawned_b = 0;
    packet_len = fn_nio_exchange_build_clock_get(
        job_a.request, sizeof(job_a.request));
    if (packet_len < 0) return RETURN_FAIL;
    job_a.request_length = (UWORD)packet_len;
    packet_len = fn_nio_exchange_build_clock_get_tz(
        job_b.request, sizeof(job_b.request));
    if (packet_len < 0) return RETURN_FAIL;
    job_b.request_length = (UWORD)packet_len;
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

    list_len = fn_nio_exchange_build_file_list(
        list_req, sizeof(list_req), COMPLETION_URI, sizeof(response), -1);
    if (list_len < 0) return RETURN_FAIL;
    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;

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
    reclaim_io(&req.fn_io);
    CloseDevice(&req.fn_io);
    DeletePort(port);

    wait_spawned_tasks_gone();
    if (failures) return RETURN_FAIL;
    printf("PASS isolated-exchange\n");
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    /* Redirected logs must not sit in a full stdio buffer across a crash. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_matrix_usage();
            return RETURN_OK;
        }
        return run_matrix(argc, argv);
    }
    return run_isolation_suite();
}
