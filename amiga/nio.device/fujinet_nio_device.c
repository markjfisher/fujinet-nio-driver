#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_nio_device.h"
#include "fujinet_nio_backend.h"
#include "fujinet_io_queue.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

#ifdef FUJINET_NIO_NATIVE_TEST
#include <stdlib.h>
#define FN_REGISTER(name)
#else
#define FN_REGISTER(name) __asm(name)
#endif

#define DEVICE_NAME FUJINET_NIO_DEVICE_NAME
#define DEVICE_VERSION 0
#define DEVICE_REVISION 2
/* Headroom for a later serial/SLIP wait; 2A does not allocate serial buffers. */
#define WORKER_STACK_SIZE 16384

typedef uint8_t (*fujinet_nio_backend_open_fn)(void);
typedef void (*fujinet_nio_backend_close_fn)(void);
typedef uint8_t (*fujinet_nio_backend_exchange_fn)(
    const uint8_t *request, uint16_t request_len, uint8_t *response,
    uint16_t response_capacity, uint16_t *response_len, uint8_t *detail,
    uint8_t *native_io_error, uint16_t *native_status);
typedef uint8_t (*fujinet_nio_backend_set_baud_fn)(uint32_t baud);
typedef uint32_t (*fujinet_nio_backend_get_baud_fn)(void);

enum {
    NIO_WORKER_IDLE = 0,
    NIO_WORKER_IN_PROGRESS = 1,
    NIO_WORKER_COMPLETING = 2
};

struct fujinet_nio_device_base {
    struct Device device;
    BPTR segment_list;
    struct Unit exec_unit;
    fujinet_io_queue_t io_queue;
    struct FujiNetNIORequest *in_progress;
    UBYTE worker_state;
    UBYTE in_progress_aborted;
    UBYTE backend_open;
    UBYTE io_processing;
    struct Task worker_task;
    APTR worker_stack;
    BYTE worker_signal;
    fujinet_nio_backend_open_fn backend_open_fn;
    fujinet_nio_backend_close_fn backend_close_fn;
    fujinet_nio_backend_exchange_fn backend_exchange_fn;
    fujinet_nio_backend_set_baud_fn backend_set_baud_fn;
    fujinet_nio_backend_get_baud_fn backend_get_baud_fn;
};

struct ExecBase *SysBase;

#ifndef FUJINET_NIO_NATIVE_TEST
static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    "$VER: " DEVICE_NAME " 0.2 (29.8.2026) \xa9 2026 Mark Fisher\r\n";
#endif

static uint8_t pad_nonzero(const struct FujiNetNIORequest *req)
{
    return (uint8_t)(req->fn_pad[0] | req->fn_pad[1] | req->fn_pad[2]);
}

static void apply_abort_completion(struct FujiNetNIORequest *req)
{
    req->fn_io.io_Error = IOERR_ABORTED;
    req->fn_nio_error = FN_ERR_ABORTED;
    req->fn_response_length = 0;
}

static void reject_begin_io(struct FujiNetNIORequest *req, BYTE io_error)
{
    req->fn_io.io_Flags &= (UBYTE)~IOF_QUICK;
    req->fn_io.io_Error = io_error;
    req->fn_nio_error = FN_ERR_INVALID;
    req->fn_response_length = 0;
    ReplyMsg(&req->fn_io.io_Message);
}

static void close_backend(struct fujinet_nio_device_base *base)
{
    if (!base->backend_open) return;
    if (base->backend_close_fn != NULL) base->backend_close_fn();
    base->backend_open = 0;
}

static uint8_t ensure_backend_open(struct fujinet_nio_device_base *base)
{
    uint8_t result;

    if (base->backend_open) return FN_OK;
    if (base->backend_open_fn == NULL) return FN_ERR_IO;
    result = base->backend_open_fn();
    if (result == FN_OK) base->backend_open = 1;
    return result;
}

static struct FujiNetNIORequest *dequeue_next(
    struct fujinet_nio_device_base *base)
{
    static const uint8_t stopped[1] = {0};
    fujinet_io_queue_node_t *node = fujinet_io_queue_next(
        &base->io_queue, stopped, 1, 0, 0);
    struct FujiNetNIORequest *req;

    if (node == NULL) return NULL;
    req = (struct FujiNetNIORequest *)node->request;
    FreeMem(node, sizeof(*node));
    return req;
}

static void process_exchange(struct fujinet_nio_device_base *base,
                             struct FujiNetNIORequest *req)
{
    uint8_t nio_error;
    uint8_t completion_stage = 1;
    uint8_t detail = FUJINET_NIO_DETAIL_NONE;
    uint8_t native_io_error = 0;
    uint16_t native_status = 0;
    uint16_t response_len = 0;

    nio_error = ensure_backend_open(base);
    if (nio_error == FN_OK) {
        completion_stage = 2;
        if (base->backend_exchange_fn == NULL) {
            nio_error = FN_ERR_IO;
            detail = FUJINET_NIO_DETAIL_BACKEND_OPEN;
        } else {
            nio_error = base->backend_exchange_fn(
                req->fn_request_data, req->fn_request_length,
                req->fn_response_data, req->fn_response_capacity,
                &response_len, &detail, &native_io_error, &native_status);
            /* TRANSPORT (including Paula overrun) and TIMEOUT both close.
             * Keeping serial.device open after overrun left leftover RX and a
             * latched LineErr on the warm path; the next exchange then failed
             * WARMUP or blocked in CMD_READ. Lazy-reopen on the next EXCHANGE
             * matches SET_BAUD / timeout recovery. */
            if (nio_error == FN_ERR_TRANSPORT || nio_error == FN_ERR_TIMEOUT)
                close_backend(base);
        }
    } else {
        detail = FUJINET_NIO_DETAIL_BACKEND_OPEN;
    }

    Disable();
    base->worker_state = NIO_WORKER_COMPLETING;
    if (base->in_progress_aborted) {
        apply_abort_completion(req);
    } else {
        if (nio_error == FN_OK &&
            response_len > req->fn_response_capacity) {
            nio_error = FN_ERR_IO;
            response_len = 0;
        }
        req->fn_io.io_Error = 0;
        req->fn_nio_error = nio_error;
        req->fn_pad[0] = completion_stage;
        req->fn_pad[1] = nio_error;
        req->fn_pad[2] = detail;
        req->fn_flags = (UWORD)(native_io_error |
            ((native_status >> 8) << 8));
        if (nio_error == FN_OK) req->fn_response_length = response_len;
        else req->fn_response_length = 0;
    }
    base->in_progress = NULL;
    base->in_progress_aborted = 0;
    base->worker_state = NIO_WORKER_IDLE;
    Enable();
    ReplyMsg(&req->fn_io.io_Message);
}

static uint32_t read_le32(const UBYTE *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le32(UBYTE *data, uint32_t value)
{
    data[0] = (UBYTE)value;
    data[1] = (UBYTE)(value >> 8);
    data[2] = (UBYTE)(value >> 16);
    data[3] = (UBYTE)(value >> 24);
}

static void process_control(struct fujinet_nio_device_base *base,
                            struct FujiNetNIORequest *req)
{
    uint8_t nio_error = FN_OK;

    if (req->fn_io.io_Command == FUJINET_NIO_CMD_SET_BAUD) {
        /* This request is serialized behind every exchange. Closing the
         * backend here makes the next exchange reopen serial.device with the
         * new rate; no active stream can observe a mid-frame reconfiguration. */
        close_backend(base);
        if (base->backend_set_baud_fn == NULL) {
            nio_error = FN_ERR_UNSUPPORTED;
        } else {
            nio_error = base->backend_set_baud_fn(read_le32(req->fn_request_data));
        }
    } else if (req->fn_io.io_Command == FUJINET_NIO_CMD_GET_BAUD) {
        if (base->backend_get_baud_fn == NULL) {
            nio_error = FN_ERR_UNSUPPORTED;
        } else {
            write_le32(req->fn_response_data, base->backend_get_baud_fn());
            req->fn_response_length = 4;
        }
    } else {
        nio_error = FN_ERR_INVALID;
    }

    Disable();
    base->worker_state = NIO_WORKER_COMPLETING;
    if (base->in_progress_aborted) {
        apply_abort_completion(req);
    } else {
        req->fn_io.io_Error = 0;
        req->fn_nio_error = nio_error;
        if (nio_error != FN_OK || req->fn_io.io_Command != FUJINET_NIO_CMD_GET_BAUD)
            req->fn_response_length = 0;
    }
    base->in_progress = NULL;
    base->in_progress_aborted = 0;
    base->worker_state = NIO_WORKER_IDLE;
    Enable();
    ReplyMsg(&req->fn_io.io_Message);
}

static void worker_pump(struct fujinet_nio_device_base *base)
{
    for (;;) {
        struct FujiNetNIORequest *req;

        Disable();
        req = dequeue_next(base);
        if (req == NULL) {
            base->io_processing = 0;
            Enable();
            return;
        }
        base->in_progress = req;
        base->in_progress_aborted = 0;
        base->worker_state = NIO_WORKER_IN_PROGRESS;
        Enable();
        if (req->fn_io.io_Command == FUJINET_NIO_CMD_EXCHANGE)
            process_exchange(base, req);
        else
            process_control(base, req);
#ifdef FUJINET_NIO_NATIVE_TEST
        return;
#endif
    }
}

static void device_worker_entry(void);
static BPTR device_expunge(register struct fujinet_nio_device_base *base
                               FN_REGISTER("a6"));

static struct fujinet_nio_device_base *device_init(
    register struct fujinet_nio_device_base *base FN_REGISTER("d0"),
    register BPTR segment_list FN_REGISTER("a0"),
    register struct ExecBase *sys_base FN_REGISTER("a6"))
{
    SysBase = sys_base;
    base->segment_list = segment_list;
    fujinet_io_queue_init(&base->io_queue);
#ifndef FUJINET_NIO_NATIVE_TEST
    base->backend_open_fn = backend_open;
    base->backend_close_fn = backend_close;
    base->backend_exchange_fn = backend_exchange;
    base->backend_set_baud_fn = backend_set_baud;
    base->backend_get_baud_fn = backend_get_baud;
    base->worker_signal = AllocSignal(-1);
    if (base->worker_signal == -1) return NULL;
    base->worker_stack = AllocMem(WORKER_STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (base->worker_stack == NULL) {
        FreeSignal(base->worker_signal);
        return NULL;
    }
    base->worker_task.tc_SPLower = base->worker_stack;
    base->worker_task.tc_SPUpper =
        (UBYTE *)base->worker_stack + WORKER_STACK_SIZE;
    base->worker_task.tc_SPReg = (UBYTE *)base->worker_task.tc_SPUpper - 4;
    base->worker_task.tc_UserData = base;
    if (AddTask(&base->worker_task, (APTR)device_worker_entry, NULL) == NULL) {
        FreeMem(base->worker_stack, WORKER_STACK_SIZE);
        FreeSignal(base->worker_signal);
        return NULL;
    }
#endif
    return base;
}

static struct Device *device_open(
    register struct IORequest *request FN_REGISTER("a1"),
    register ULONG unit_number FN_REGISTER("d0"),
    register ULONG flags FN_REGISTER("d1"),
    register struct fujinet_nio_device_base *base FN_REGISTER("a6"))
{
    struct FujiNetNIORequest *req = (struct FujiNetNIORequest *)request;

    (void)flags;
    if (unit_number != FUJINET_NIO_DEVICE_UNIT) {
        request->io_Error = IOERR_OPENFAIL;
        req->fn_nio_error = FN_ERR_NOT_FOUND;
        return NULL;
    }

    request->io_Error = 0;
    request->io_Device = &base->device;
    request->io_Unit = &base->exec_unit;
    ++base->device.dd_Library.lib_OpenCnt;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return &base->device;
}

static BPTR device_close(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_nio_device_base *base FN_REGISTER("a6"))
{
    fujinet_io_queue_node_t *queued;
    uint8_t reply_aborted = 0;
    uint8_t delayed_expunge = 0;

    /*
     * Callers must AbortIO/WaitIO before CloseDevice. A still-queued
     * request on this IORequest is completed as aborted so it is not
     * left in the FIFO. An in-progress exchange is not cancelled; the
     * worker still ReplyMsgs once.
     */
    Disable();
    queued = fujinet_io_queue_remove_request(&base->io_queue, request);
    if (queued != NULL) {
        struct FujiNetNIORequest *req = (struct FujiNetNIORequest *)request;
        FreeMem(queued, sizeof(*queued));
        apply_abort_completion(req);
        reply_aborted = 1;
    }
    request->io_Device = NULL;
    request->io_Unit = NULL;
    if (base->device.dd_Library.lib_OpenCnt != 0) {
        --base->device.dd_Library.lib_OpenCnt;
    }
    /* OpenCnt 0 does not close the backend. LIBF_DELEXP does, via expunge. */
    delayed_expunge =
        (uint8_t)(base->device.dd_Library.lib_OpenCnt == 0 &&
                  (base->device.dd_Library.lib_Flags & LIBF_DELEXP) != 0);
    Enable();
    if (reply_aborted) ReplyMsg(&request->io_Message);
    if (delayed_expunge) return device_expunge(base);
    return 0;
}

static BPTR device_expunge(
    register struct fujinet_nio_device_base *base FN_REGISTER("a6"))
{
    BPTR segment_list;

    Disable();
    if (base->device.dd_Library.lib_OpenCnt != 0 ||
        base->io_queue.head != NULL ||
        base->in_progress != NULL) {
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
        Enable();
        return 0;
    }
    segment_list = base->segment_list;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    Enable();

#ifndef FUJINET_NIO_NATIVE_TEST
    if (base->worker_stack != NULL) {
        RemTask(&base->worker_task);
        FreeMem(base->worker_stack, WORKER_STACK_SIZE);
        base->worker_stack = NULL;
    }
    if (base->worker_signal != -1) {
        FreeSignal(base->worker_signal);
        base->worker_signal = -1;
    }
#endif
    close_backend(base);
#ifndef FUJINET_NIO_NATIVE_TEST
    Forbid();
    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - base->device.dd_Library.lib_NegSize,
            (ULONG)base->device.dd_Library.lib_NegSize +
                (ULONG)base->device.dd_Library.lib_PosSize);
    Permit();
#endif
    return segment_list;
}

static ULONG device_reserved(void)
{
    return 0;
}

#ifndef FUJINET_NIO_NATIVE_TEST
static void device_worker_entry(void)
{
    struct fujinet_nio_device_base *base =
        (struct fujinet_nio_device_base *)FindTask(NULL)->tc_UserData;
    ULONG signal_mask = 1UL << base->worker_signal;

    for (;;) {
        Wait(signal_mask);
        worker_pump(base);
    }
}
#endif

static void device_begin_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_nio_device_base *base FN_REGISTER("a6"))
{
    struct FujiNetNIORequest *req = (struct FujiNetNIORequest *)request;
    fujinet_io_queue_node_t *node;

    if (request->io_Command != FUJINET_NIO_CMD_EXCHANGE &&
        request->io_Command != FUJINET_NIO_CMD_SET_BAUD &&
        request->io_Command != FUJINET_NIO_CMD_GET_BAUD) {
        reject_begin_io(req, IOERR_NOCMD);
        return;
    }
    if (req->fn_struct_size != FUJINET_NIO_REQUEST_SIZE) {
        reject_begin_io(req, IOERR_BADLENGTH);
        return;
    }
    if (req->fn_flags != 0 || pad_nonzero(req)) {
        reject_begin_io(req, IOERR_NOCMD);
        return;
    }
    if (req->fn_request_data == NULL && req->fn_request_length != 0) {
        reject_begin_io(req, IOERR_BADADDRESS);
        return;
    }
    if (req->fn_response_data == NULL && req->fn_response_capacity != 0) {
        reject_begin_io(req, IOERR_BADADDRESS);
        return;
    }
    if (request->io_Command == FUJINET_NIO_CMD_EXCHANGE) {
        if (req->fn_request_length > FN_MAX_PACKET_SIZE ||
            req->fn_response_capacity > FN_MAX_PACKET_SIZE) {
            reject_begin_io(req, IOERR_BADLENGTH);
            return;
        }
    } else if (request->io_Command == FUJINET_NIO_CMD_SET_BAUD) {
        if (req->fn_request_data == NULL || req->fn_request_length != 4 ||
            req->fn_response_capacity != 0) {
            reject_begin_io(req, IOERR_BADLENGTH);
            return;
        }
    } else if (req->fn_request_length != 0 || req->fn_response_data == NULL ||
               req->fn_response_capacity < 4) {
        reject_begin_io(req, IOERR_BADLENGTH);
        return;
    }

    Disable();
    req->fn_io.io_Flags &= (UBYTE)~IOF_QUICK;
    node = AllocMem(sizeof(*node), MEMF_PUBLIC | MEMF_CLEAR);
    if (node == NULL) {
        Enable();
        req->fn_io.io_Error = IOERR_OPENFAIL;
        req->fn_nio_error = FN_ERR_IO;
        req->fn_response_length = 0;
        ReplyMsg(&req->fn_io.io_Message);
        return;
    }
    req->fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    fujinet_io_queue_append(&base->io_queue, node, req, FUJINET_NIO_DEVICE_UNIT,
                            req->fn_io.io_Command);
    base->io_processing = 1;
#ifndef FUJINET_NIO_NATIVE_TEST
    Signal(&base->worker_task, 1UL << base->worker_signal);
#endif
    Enable();
}

static LONG device_abort_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_nio_device_base *base FN_REGISTER("a6"))
{
    struct FujiNetNIORequest *req = (struct FujiNetNIORequest *)request;
    fujinet_io_queue_node_t *queued;

    Disable();
    queued = fujinet_io_queue_remove_request(&base->io_queue, request);
    if (queued != NULL) {
        FreeMem(queued, sizeof(*queued));
        apply_abort_completion(req);
        Enable();
        ReplyMsg(&request->io_Message);
        return 0;
    }
    if (base->in_progress == req &&
        base->worker_state == NIO_WORKER_IN_PROGRESS) {
        base->in_progress_aborted = 1;
        Enable();
        return 0;
    }
    Enable();
    return 0;
}

#ifndef FUJINET_NIO_NATIVE_TEST
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
    sizeof(struct fujinet_nio_device_base),
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
#endif

#ifdef FUJINET_NIO_NATIVE_TEST
static struct fujinet_nio_device_base native_test_base;
static struct ExecBase native_test_sys_base;

void fujinet_nio_native_test_reset(void)
{
    memset(&native_test_base, 0, sizeof(native_test_base));
    (void)device_init(&native_test_base, (BPTR)1, &native_test_sys_base);
}

void fujinet_nio_native_test_set_backend(
    fujinet_nio_backend_open_fn open_fn,
    fujinet_nio_backend_close_fn close_fn,
    fujinet_nio_backend_exchange_fn exchange_fn,
    fujinet_nio_backend_set_baud_fn set_baud_fn,
    fujinet_nio_backend_get_baud_fn get_baud_fn)
{
    native_test_base.backend_open_fn = open_fn;
    native_test_base.backend_close_fn = close_fn;
    native_test_base.backend_exchange_fn = exchange_fn;
    native_test_base.backend_set_baud_fn = set_baud_fn;
    native_test_base.backend_get_baud_fn = get_baud_fn;
}

struct Device *fujinet_nio_native_test_open(struct IORequest *request,
                                            ULONG unit)
{
    return device_open(request, unit, 0, &native_test_base);
}

BPTR fujinet_nio_native_test_close(struct IORequest *request)
{
    return device_close(request, &native_test_base);
}

void fujinet_nio_native_test_begin_io(struct IORequest *request)
{
    device_begin_io(request, &native_test_base);
}

LONG fujinet_nio_native_test_abort_io(struct IORequest *request)
{
    return device_abort_io(request, &native_test_base);
}

BPTR fujinet_nio_native_test_expunge(void)
{
    return device_expunge(&native_test_base);
}

void fujinet_nio_native_test_worker_step(void)
{
    worker_pump(&native_test_base);
}

UWORD fujinet_nio_native_test_open_cnt(void)
{
    return native_test_base.device.dd_Library.lib_OpenCnt;
}

uint8_t fujinet_nio_native_test_backend_is_open(void)
{
    return native_test_base.backend_open;
}

uint8_t fujinet_nio_native_test_queue_busy(void)
{
    return native_test_base.io_queue.head != NULL ||
           native_test_base.in_progress != NULL;
}

UBYTE fujinet_nio_native_test_lib_flags(void)
{
    return native_test_base.device.dd_Library.lib_Flags;
}
#endif
