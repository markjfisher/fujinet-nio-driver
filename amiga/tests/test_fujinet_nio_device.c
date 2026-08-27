#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>

#include "fujinet_nio_device.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t (*fujinet_nio_backend_open_fn)(void);
typedef void (*fujinet_nio_backend_close_fn)(void);
typedef uint8_t (*fujinet_nio_backend_exchange_fn)(
    const uint8_t *request, uint16_t request_len, uint8_t *response,
    uint16_t response_capacity, uint16_t *response_len);
typedef uint8_t (*fujinet_nio_backend_set_baud_fn)(uint32_t baud);
typedef uint32_t (*fujinet_nio_backend_get_baud_fn)(void);

void fujinet_nio_native_test_reset(void);
void fujinet_nio_native_test_set_backend(
    fujinet_nio_backend_open_fn open_fn,
    fujinet_nio_backend_close_fn close_fn,
    fujinet_nio_backend_exchange_fn exchange_fn,
    fujinet_nio_backend_set_baud_fn set_baud_fn,
    fujinet_nio_backend_get_baud_fn get_baud_fn);
struct Device *fujinet_nio_native_test_open(struct IORequest *request,
                                            ULONG unit);
BPTR fujinet_nio_native_test_close(struct IORequest *request);
void fujinet_nio_native_test_begin_io(struct IORequest *request);
LONG fujinet_nio_native_test_abort_io(struct IORequest *request);
BPTR fujinet_nio_native_test_expunge(void);
void fujinet_nio_native_test_worker_step(void);
UWORD fujinet_nio_native_test_open_cnt(void);
uint8_t fujinet_nio_native_test_backend_is_open(void);
uint8_t fujinet_nio_native_test_queue_busy(void);
UBYTE fujinet_nio_native_test_lib_flags(void);

static unsigned failures;
static unsigned replies;
static struct Message *reply_log[8];

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

void Disable(void) {}
void Enable(void) {}

void ReplyMsg(struct Message *message)
{
    if (replies < 8) reply_log[replies] = message;
    ++replies;
}

APTR AllocMem(ULONG bytes, ULONG flags)
{
    (void)flags;
    return calloc(1, bytes);
}

void FreeMem(APTR memory, ULONG bytes)
{
    (void)bytes;
    free(memory);
}

static unsigned backend_opens;
static unsigned backend_closes;
static unsigned backend_exchanges;
static uint8_t backend_fatal;
static uint8_t backend_timeout;
static uint8_t backend_delay_abort;
static uint8_t backend_delay_expunge;
static uint8_t backend_delay_close;
static uint8_t backend_oversize_len;
static struct FujiNetNIORequest *delay_target;
static uint8_t canned_response[4] = {9, 8, 7, 6};
static uint16_t canned_len = 4;
static BPTR expunge_from_backend;
static uint32_t backend_baud;

static uint8_t test_backend_open(void)
{
    ++backend_opens;
    return FN_OK;
}

static void test_backend_close(void)
{
    ++backend_closes;
}

static uint8_t test_backend_set_baud(uint32_t baud)
{
    if (baud < 300 || baud > 230400) return FN_ERR_INVALID;
    backend_baud = baud;
    return FN_OK;
}

static uint32_t test_backend_get_baud(void)
{
    return backend_baud;
}

static uint8_t test_backend_exchange(const uint8_t *request,
                                     uint16_t request_len,
                                     uint8_t *response,
                                     uint16_t response_capacity,
                                     uint16_t *response_len)
{
    ++backend_exchanges;
    *response_len = 0;
    if (backend_delay_abort && delay_target != NULL) {
        fujinet_nio_native_test_abort_io(&delay_target->fn_io);
    }
    if (backend_delay_expunge) {
        expunge_from_backend = fujinet_nio_native_test_expunge();
    }
    if (backend_delay_close && delay_target != NULL) {
        fujinet_nio_native_test_close(&delay_target->fn_io);
    }
    if (backend_oversize_len) {
        backend_oversize_len = 0;
        *response_len = (uint16_t)(response_capacity + 1);
        return FN_OK;
    }
    if (backend_fatal) {
        backend_fatal = 0;
        return FN_ERR_TRANSPORT;
    }
    if (backend_timeout) {
        backend_timeout = 0;
        return FN_ERR_TIMEOUT;
    }
    if (request != NULL && request_len > 0 && response != NULL) {
        uint16_t n = request_len;
        if (n > response_capacity) n = response_capacity;
        memcpy(response, request, n);
        *response_len = n;
        return FN_OK;
    }
    if (response != NULL && response_capacity >= canned_len) {
        memcpy(response, canned_response, canned_len);
        *response_len = canned_len;
    }
    return FN_OK;
}

static void reset_harness(void)
{
    replies = 0;
    memset(reply_log, 0, sizeof(reply_log));
    backend_opens = 0;
    backend_closes = 0;
    backend_exchanges = 0;
    backend_fatal = 0;
    backend_timeout = 0;
    backend_delay_abort = 0;
    backend_delay_expunge = 0;
    backend_delay_close = 0;
    backend_oversize_len = 0;
    delay_target = NULL;
    expunge_from_backend = (BPTR)0;
    backend_baud = 19200;
    fujinet_nio_native_test_reset();
    fujinet_nio_native_test_set_backend(test_backend_open, test_backend_close,
                                        test_backend_exchange,
                                        test_backend_set_baud,
                                        test_backend_get_baud);
}

static void init_exchange(struct FujiNetNIORequest *req,
                          const UBYTE *request_data, UWORD request_len,
                          UBYTE *response_data, UWORD response_cap)
{
    memset(req, 0, sizeof(*req));
    req->fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
    req->fn_io.io_Flags = IOF_QUICK;
    req->fn_struct_size = (UWORD)FUJINET_NIO_REQUEST_SIZE;
    req->fn_request_data = request_data;
    req->fn_request_length = request_len;
    req->fn_response_data = response_data;
    req->fn_response_capacity = response_cap;
    req->fn_response_length = 0xFFFF;
}

static void write_le32(UBYTE *data, uint32_t value)
{
    data[0] = (UBYTE)value;
    data[1] = (UBYTE)(value >> 8);
    data[2] = (UBYTE)(value >> 16);
    data[3] = (UBYTE)(value >> 24);
}

static uint32_t read_le32(const UBYTE *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void open_unit0(struct FujiNetNIORequest *req)
{
    CHECK("OpenDevice unit 0",
          fujinet_nio_native_test_open(&req->fn_io, FUJINET_NIO_DEVICE_UNIT) !=
              NULL);
    CHECK("OpenDevice io_Error", req->fn_io.io_Error == 0);
}

static void test_stub_ndk_symbols(void)
{
    CHECK("IOERR_NOCMD is NDK 47.1 -3", IOERR_NOCMD == (BYTE)-3);
    CHECK("IOERR_BADLENGTH is NDK 47.1 -4", IOERR_BADLENGTH == (BYTE)-4);
}

static void test_happy_injected(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[2] = {1, 2};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 2, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    CHECK("queued clears IOF_QUICK", (req.fn_io.io_Flags & IOF_QUICK) == 0);
    CHECK("not replied until worker", replies == 0);
    fujinet_nio_native_test_worker_step();
    CHECK("happy io_Error", req.fn_io.io_Error == 0);
    CHECK("happy fn_nio_error", req.fn_nio_error == FN_OK);
    CHECK("happy response length", req.fn_response_length == 2);
    CHECK("happy response bytes", response[0] == 1 && response[1] == 2);
    CHECK("one ReplyMsg", replies == 1);
    CHECK("backend opened once", backend_opens == 1);
    CHECK("one exchange", backend_exchanges == 1);
}

static void test_baud_controls(void)
{
    struct FujiNetNIORequest exchange, set, get;
    UBYTE request[1] = {1};
    UBYTE response[8];
    UBYTE baud_bytes[4];

    reset_harness();
    init_exchange(&exchange, request, sizeof(request), response, sizeof(response));
    open_unit0(&exchange);
    fujinet_nio_native_test_begin_io(&exchange.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("baud setup exchange opened backend", backend_opens == 1);

    write_le32(baud_bytes, 115200);
    init_exchange(&set, baud_bytes, sizeof(baud_bytes), NULL, 0);
    set.fn_io.io_Command = FUJINET_NIO_CMD_SET_BAUD;
    fujinet_nio_native_test_open(&set.fn_io, FUJINET_NIO_DEVICE_UNIT);
    replies = 0;
    fujinet_nio_native_test_begin_io(&set.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("baud set replied", replies == 1);
    CHECK("baud set FN_OK", set.fn_nio_error == FN_OK);
    CHECK("baud set no exchange", backend_exchanges == 1);
    CHECK("baud set closes current backend", backend_closes == 1);
    CHECK("baud set value", backend_baud == 115200);

    init_exchange(&get, NULL, 0, baud_bytes, sizeof(baud_bytes));
    get.fn_io.io_Command = FUJINET_NIO_CMD_GET_BAUD;
    fujinet_nio_native_test_open(&get.fn_io, FUJINET_NIO_DEVICE_UNIT);
    replies = 0;
    fujinet_nio_native_test_begin_io(&get.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("baud get replied", replies == 1);
    CHECK("baud get FN_OK", get.fn_nio_error == FN_OK);
    CHECK("baud get length", get.fn_response_length == 4);
    CHECK("baud get value", read_le32(baud_bytes) == 115200);
}

static void test_empty_request(void)
{
    struct FujiNetNIORequest req;
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, NULL, 0, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    CHECK("empty request not BADADDRESS", req.fn_io.io_Error != IOERR_BADADDRESS);
    fujinet_nio_native_test_worker_step();
    CHECK("empty request exchanged", backend_exchanges == 1);
    CHECK("empty request io_Error", req.fn_io.io_Error == 0);
    CHECK("empty request FN_OK", req.fn_nio_error == FN_OK);
}

static void test_overlapping_malformed(void)
{
    struct FujiNetNIORequest req;
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, NULL, 0, response, sizeof(response));
    req.fn_io.io_Command = (UWORD)(FUJINET_NIO_CMD_GET_BAUD + 1);
    req.fn_struct_size = 1;
    req.fn_flags = 1;
    req.fn_pad[0] = 1;
    open_unit0(&req);
    replies = 0;
    fujinet_nio_native_test_begin_io(&req.fn_io);
    CHECK("overlap first-match IOERR_NOCMD", req.fn_io.io_Error == IOERR_NOCMD);
    CHECK("overlap FN_ERR_INVALID", req.fn_nio_error == FN_ERR_INVALID);
    CHECK("overlap length 0", req.fn_response_length == 0);
    CHECK("overlap IOF_QUICK cleared", (req.fn_io.io_Flags & IOF_QUICK) == 0);
    CHECK("overlap not queued", backend_exchanges == 0);
    CHECK("overlap immediate ReplyMsg", replies == 1);
    CHECK("overlap queue idle", fujinet_nio_native_test_queue_busy() == 0);
}

static void expect_beginio_reject(const char *label, struct FujiNetNIORequest *req,
                                 BYTE io_error)
{
    (void)label;
    replies = 0;
    fujinet_nio_native_test_begin_io(&req->fn_io);
    CHECK(label, req->fn_io.io_Error == io_error);
    CHECK(label, req->fn_nio_error == FN_ERR_INVALID);
    CHECK(label, req->fn_response_length == 0);
    CHECK(label, backend_exchanges == 0);
    CHECK(label, replies == 1);
    CHECK(label, fujinet_nio_native_test_queue_busy() == 0);
}

static void test_beginio_first_match_rows(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {0};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    req.fn_struct_size = 1;
    open_unit0(&req);
    expect_beginio_reject("bad fn_struct_size", &req, IOERR_BADLENGTH);

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    req.fn_flags = 1;
    open_unit0(&req);
    expect_beginio_reject("nonzero fn_flags", &req, IOERR_NOCMD);

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    req.fn_pad[2] = 1;
    open_unit0(&req);
    expect_beginio_reject("nonzero fn_pad", &req, IOERR_NOCMD);

    reset_harness();
    init_exchange(&req, NULL, 1, response, sizeof(response));
    open_unit0(&req);
    expect_beginio_reject("NULL request nonzero length", &req, IOERR_BADADDRESS);

    reset_harness();
    init_exchange(&req, request_bytes, 1, NULL, 8);
    open_unit0(&req);
    expect_beginio_reject("NULL response nonzero capacity", &req, IOERR_BADADDRESS);

    reset_harness();
    init_exchange(&req, request_bytes, (UWORD)(FN_MAX_PACKET_SIZE + 1),
                  response, sizeof(response));
    open_unit0(&req);
    expect_beginio_reject("request length oversize", &req, IOERR_BADLENGTH);
}

static void test_response_oversize(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {0};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response,
                  (UWORD)(FN_MAX_PACKET_SIZE + 1));
    open_unit0(&req);
    replies = 0;
    fujinet_nio_native_test_begin_io(&req.fn_io);
    CHECK("oversize IOERR_BADLENGTH", req.fn_io.io_Error == IOERR_BADLENGTH);
    CHECK("oversize FN_ERR_INVALID", req.fn_nio_error == FN_ERR_INVALID);
    CHECK("oversize length 0", req.fn_response_length == 0);
    CHECK("oversize not queued", backend_exchanges == 0);
}

static void test_null_zero_not_badaddress(void)
{
    struct FujiNetNIORequest req;

    reset_harness();
    init_exchange(&req, NULL, 0, NULL, 0);
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    CHECK("NULL+0 not BADADDRESS", req.fn_io.io_Error != IOERR_BADADDRESS);
    fujinet_nio_native_test_worker_step();
    CHECK("NULL+0 exchanged", backend_exchanges == 1);
}

static void test_abort_queued(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {4};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    replies = 0;
    CHECK("abort queued returns 0",
          fujinet_nio_native_test_abort_io(&req.fn_io) == 0);
    CHECK("abort queued IOERR_ABORTED", req.fn_io.io_Error == IOERR_ABORTED);
    CHECK("abort queued FN_ERR_ABORTED", req.fn_nio_error == FN_ERR_ABORTED);
    CHECK("abort queued length 0", req.fn_response_length == 0);
    CHECK("abort queued one reply", replies == 1);
    CHECK("abort queued no backend", backend_exchanges == 0);
    fujinet_nio_native_test_worker_step();
    CHECK("abort queued still one reply", replies == 1);
    CHECK("abort queued still no backend", backend_exchanges == 0);
}

static void test_abort_in_progress(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {5};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    backend_delay_abort = 1;
    delay_target = &req;
    replies = 0;
    fujinet_nio_native_test_worker_step();
    CHECK("in-progress backend ran", backend_exchanges == 1);
    CHECK("in-progress IOERR_ABORTED", req.fn_io.io_Error == IOERR_ABORTED);
    CHECK("in-progress FN_ERR_ABORTED", req.fn_nio_error == FN_ERR_ABORTED);
    CHECK("in-progress discarded length", req.fn_response_length == 0);
    CHECK("in-progress one ReplyMsg", replies == 1);
}

static void test_opencnt_zero_keeps_backend(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {6};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("backend open after first exchange",
          fujinet_nio_native_test_backend_is_open() == 1);
    fujinet_nio_native_test_close(&req.fn_io);
    CHECK("OpenCnt is 0", fujinet_nio_native_test_open_cnt() == 0);
    CHECK("backend not closed on OpenCnt 0", backend_closes == 0);
    CHECK("backend still open", fujinet_nio_native_test_backend_is_open() == 1);

    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("lazy open not repeated", backend_opens == 1);
    CHECK("second exchange reused backend", backend_exchanges == 2);
}

static void test_open_invalid_unit(void)
{
    struct FujiNetNIORequest req;
    struct FujiNetNIORequest bad;
    UBYTE request_bytes[1] = {0};
    UBYTE response[8];
    struct Device *device;
    UWORD open_cnt;

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    open_cnt = fujinet_nio_native_test_open_cnt();
    init_exchange(&bad, request_bytes, 1, response, sizeof(response));
    device = fujinet_nio_native_test_open(&bad.fn_io, 1);
    CHECK("invalid unit returns NULL", device == NULL);
    CHECK("invalid unit IOERR_OPENFAIL", bad.fn_io.io_Error == IOERR_OPENFAIL);
    CHECK("invalid unit FN_ERR_NOT_FOUND", bad.fn_nio_error == FN_ERR_NOT_FOUND);
    CHECK("invalid unit OpenCnt unchanged",
          fujinet_nio_native_test_open_cnt() == open_cnt);
}

static void test_fatal_backend(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {7};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    backend_fatal = 1;
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("fatal io_Error stays Exec-ok", req.fn_io.io_Error == 0);
    CHECK("fatal FN_ERR_TRANSPORT", req.fn_nio_error == FN_ERR_TRANSPORT);
    CHECK("fatal length 0", req.fn_response_length == 0);
    CHECK("fatal closed backend", backend_closes == 1);
    CHECK("fatal backend not open", fujinet_nio_native_test_backend_is_open() == 0);

    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("next exchange lazy-reopens", backend_opens == 2);
    CHECK("reopen FN_OK", req.fn_nio_error == FN_OK);
    CHECK("reopen io_Error 0", req.fn_io.io_Error == 0);
}

static void test_timeout_resets_backend(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {7};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    backend_timeout = 1;
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("timeout io_Error stays Exec-ok", req.fn_io.io_Error == 0);
    CHECK("timeout FN_ERR_TIMEOUT", req.fn_nio_error == FN_ERR_TIMEOUT);
    CHECK("timeout length 0", req.fn_response_length == 0);
    CHECK("timeout closed backend", backend_closes == 1);
    CHECK("timeout backend not open",
          fujinet_nio_native_test_backend_is_open() == 0);

    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("timeout next exchange lazy-reopens", backend_opens == 2);
    CHECK("timeout reopen FN_OK", req.fn_nio_error == FN_OK);
}

static void test_expunge_busy(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {8};
    UBYTE response[8];
    BPTR expunge_result;

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    expunge_result = fujinet_nio_native_test_expunge();
    CHECK("expunge refused while OpenCnt", expunge_result == 0);
    CHECK("DELEXP set while busy",
          (fujinet_nio_native_test_lib_flags() & LIBF_DELEXP) != 0);

    fujinet_nio_native_test_begin_io(&req.fn_io);
    backend_delay_expunge = 1;
    replies = 0;
    fujinet_nio_native_test_worker_step();
    CHECK("expunge refused in-progress", expunge_from_backend == 0);
    CHECK("live request not aborted", req.fn_io.io_Error == 0);
    CHECK("live request FN_OK", req.fn_nio_error == FN_OK);
    CHECK("in-progress still one reply", replies == 1);
    CHECK("expunge did not close backend", backend_closes == 0);
}

static void test_expunge_idle(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {8};
    UBYTE response[8];
    BPTR expunge_result;

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    fujinet_nio_native_test_close(&req.fn_io);
    CHECK("idle expunge OpenCnt 0", fujinet_nio_native_test_open_cnt() == 0);
    CHECK("idle expunge queue empty", fujinet_nio_native_test_queue_busy() == 0);
    CHECK("idle expunge backend still open",
          fujinet_nio_native_test_backend_is_open() == 1);
    expunge_result = fujinet_nio_native_test_expunge();
    CHECK("idle expunge returns segment list", expunge_result == (BPTR)1);
    CHECK("idle expunge closed backend", backend_closes == 1);
    CHECK("idle expunge backend_is_open 0",
          fujinet_nio_native_test_backend_is_open() == 0);
}

static void test_abort_in_progress_fatal_closes(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {9};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    backend_delay_abort = 1;
    backend_fatal = 1;
    delay_target = &req;
    replies = 0;
    fujinet_nio_native_test_worker_step();
    CHECK("abort+fatal IOERR_ABORTED", req.fn_io.io_Error == IOERR_ABORTED);
    CHECK("abort+fatal FN_ERR_ABORTED", req.fn_nio_error == FN_ERR_ABORTED);
    CHECK("abort+fatal one ReplyMsg", replies == 1);
    CHECK("abort+fatal opens once", backend_opens == 1);
    CHECK("abort+fatal closes once", backend_closes == 1);
    CHECK("abort+fatal backend not open",
          fujinet_nio_native_test_backend_is_open() == 0);
}

static void test_delayed_expunge_on_final_close(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {8};
    UBYTE response[8];
    BPTR expunge_result;
    BPTR close_result;

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    expunge_result = fujinet_nio_native_test_expunge();
    CHECK("busy expunge defers", expunge_result == 0);
    CHECK("busy expunge sets DELEXP",
          (fujinet_nio_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("busy expunge keeps backend",
          fujinet_nio_native_test_backend_is_open() == 1);
    close_result = fujinet_nio_native_test_close(&req.fn_io);
    CHECK("final close OpenCnt 0", fujinet_nio_native_test_open_cnt() == 0);
    CHECK("final close completes delayed expunge", close_result == (BPTR)1);
    CHECK("delayed expunge closed backend", backend_closes == 1);
    CHECK("delayed expunge backend_is_open 0",
          fujinet_nio_native_test_backend_is_open() == 0);
}

static void test_backend_oversize_response_is_fn_err_io(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {1};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    backend_oversize_len = 1;
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("oversize backend io_Error 0", req.fn_io.io_Error == 0);
    CHECK("oversize backend FN_ERR_IO", req.fn_nio_error == FN_ERR_IO);
    CHECK("oversize backend length 0", req.fn_response_length == 0);
    CHECK("oversize backend still open",
          fujinet_nio_native_test_backend_is_open() == 1);
}

static void test_close_in_progress_does_not_abort(void)
{
    struct FujiNetNIORequest req;
    UBYTE request_bytes[1] = {3};
    UBYTE response[8];

    reset_harness();
    init_exchange(&req, request_bytes, 1, response, sizeof(response));
    open_unit0(&req);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    backend_delay_close = 1;
    delay_target = &req;
    replies = 0;
    fujinet_nio_native_test_worker_step();
    CHECK("close in-progress OpenCnt 0", fujinet_nio_native_test_open_cnt() == 0);
    CHECK("close in-progress does not abort", req.fn_io.io_Error == 0);
    CHECK("close in-progress FN_OK", req.fn_nio_error == FN_OK);
    CHECK("close in-progress one ReplyMsg", replies == 1);
    CHECK("close in-progress did not close backend", backend_closes == 0);
}

static void test_fifo_order(void)
{
    struct FujiNetNIORequest a, b;
    UBYTE req_a[1] = {1};
    UBYTE req_b[1] = {2};
    UBYTE resp_a[8], resp_b[8];

    reset_harness();
    init_exchange(&a, req_a, 1, resp_a, sizeof(resp_a));
    init_exchange(&b, req_b, 1, resp_b, sizeof(resp_b));
    open_unit0(&a);
    fujinet_nio_native_test_open(&b.fn_io, FUJINET_NIO_DEVICE_UNIT);
    fujinet_nio_native_test_begin_io(&a.fn_io);
    fujinet_nio_native_test_begin_io(&b.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("FIFO first completed", a.fn_nio_error == FN_OK);
    CHECK("FIFO first payload", resp_a[0] == 1);
    CHECK("FIFO second still pending", replies == 1);
    fujinet_nio_native_test_worker_step();
    CHECK("FIFO both replied", replies == 2);
    CHECK("FIFO second completed", b.fn_nio_error == FN_OK);
    CHECK("FIFO second payload", resp_b[0] == 2);
}

int main(void)
{
    test_stub_ndk_symbols();
    test_happy_injected();
    test_baud_controls();
    test_empty_request();
    test_overlapping_malformed();
    test_beginio_first_match_rows();
    test_response_oversize();
    test_null_zero_not_badaddress();
    test_abort_queued();
    test_abort_in_progress();
    test_abort_in_progress_fatal_closes();
    test_open_invalid_unit();
    test_opencnt_zero_keeps_backend();
    test_fatal_backend();
    test_timeout_resets_backend();
    test_expunge_busy();
    test_expunge_idle();
    test_delayed_expunge_on_final_close();
    test_backend_oversize_response_is_fn_err_io();
    test_close_in_progress_does_not_abort();
    test_fifo_order();

    if (failures) {
        fprintf(stderr, "%u fujinet-nio.device host tests failed\n", failures);
        return 1;
    }
    return 0;
}
