#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/newstyle.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <proto/exec.h>

#include <string.h>

#include "fujinet_disk_driver.h"
#include "fujinet_disk_device.h"
#include "fujinet_io_queue.h"
#include "fn_platform.h"

#ifdef FUJINET_DISK_NATIVE_TEST
#include <stdlib.h>
#define FN_REGISTER(name)
static uint8_t native_drain_after_begin = 1;
#else
#define FN_REGISTER(name) __asm(name)
#endif

#define DEVICE_NAME FUJINET_DISK_DEVICE_NAME
#define DEVICE_VERSION 0
#define DEVICE_REVISION 1
#define SERVICE_BUFFER_SIZE 1024
#define MAPPINGS_SIZE 17
/* A FujiBus disk exchange can allocate codec buffers and wait on serial I/O.
 * 16 KiB leaves substantial headroom without borrowing an AmigaDOS handler's
 * typically much smaller stack. */
#define WORKER_STACK_SIZE 16384

static const UWORD supported_commands[] = {
    CMD_RESET, CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_START, CMD_STOP,
    CMD_FLUSH,
    ETD_READ, ETD_WRITE, ETD_UPDATE, ETD_CLEAR,
    TD_MOTOR, TD_SEEK, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_EJECT, TD_REMOVE, TD_ADDCHANGEINT, TD_REMCHANGEINT,
    TD_GETDRIVETYPE, TD_GETNUMTRACKS, TD_GETGEOMETRY,
    NSCMD_DEVICEQUERY, FUJINET_DISK_CMD_MOUNT,
    FUJINET_DISK_CMD_MOUNT_WRITABLE, FUJINET_DISK_CMD_MOUNT_CATALOG,
    FUJINET_DISK_CMD_INSPECT_CATALOG, 0
};

struct fujinet_disk_unit_state {
    fujinet_disk_driver_t driver;
    fujinet_nio_disk_context_t nio_context;
    UBYTE stopped;
    struct List change_requests;
    struct Interrupt *legacy_change_interrupt;
};

struct fujinet_change_registration {
    struct Node node;
    struct IORequest *request;
    struct Interrupt *interrupt;
};

struct fujinet_disk_device_base {
    struct Device device;
    BPTR segment_list;
    struct Unit exec_units[FUJINET_DISK_UNIT_COUNT];
    struct fujinet_disk_unit_state units[FUJINET_DISK_UNIT_COUNT];
    UBYTE service_buffer[SERVICE_BUFFER_SIZE];
    /* BeginIO runs on the caller's task stack. Keep catalogue URI scratch in
     * the resident base along with the other serialized request state. */
    char catalog_uri[768];
    char request_uri[768];
    fujinet_io_queue_t io_queue;
    UBYTE io_processing;
    UBYTE transport_closed;
    struct Task worker_task;
    APTR worker_stack;
    BYTE worker_signal;
    struct fujinet_disk_trace trace;
};

static uint8_t remove_change_request(struct fujinet_disk_unit_state *unit,
                                      struct IORequest *request);
static void discard_change_requests(struct fujinet_disk_device_base *base);
static BPTR complete_pending_expunge(struct fujinet_disk_device_base *base);
static void worker_drain(struct fujinet_disk_device_base *base);
static void device_worker_entry(void);

struct ExecBase *SysBase;

#ifndef FUJINET_DISK_NATIVE_TEST
static const char device_name[] = DEVICE_NAME;
static const char device_id[] =
    DEVICE_NAME " 0.1 (10.8.2026)\r\n";
#endif

static BYTE result_to_io_error(uint8_t result)
{
    switch (result) {
    case FN_OK:
        return 0;
    case FN_ERR_INVALID:
        return IOERR_BADLENGTH;
    case FN_ERR_NOT_FOUND:
    case FN_ERR_NOT_READY:
        return TDERR_DiskChanged;
    default:
        return TDERR_NotSpecified;
    }
}

static uint8_t resolve_catalog_slot(struct fujinet_disk_device_base *base,
                                    uint8_t slot, char *uri,
                                    uint16_t capacity)
{
    fn_slot_catalog_io_t io = {base->service_buffer,
                               sizeof(base->service_buffer)};
    fn_slot_catalog_entry_t entry;
    uint8_t result = fn_init();
    if (result == FN_OK) result = fn_slot_catalog_get(&io, slot, &entry);
    if (result != FN_OK || !(entry.flags & FN_SLOT_CATALOG_ENTRY_VALID) ||
        entry.uri_len == 0 || entry.uri_len >= capacity) return FN_ERR_NOT_FOUND;
    memcpy(uri, entry.uri, entry.uri_len);
    uri[entry.uri_len] = '\0';
    return FN_OK;
}

static uint8_t load_mappings(struct fujinet_disk_device_base *base,
                             UBYTE *mappings)
{
    fn_appstore_io_t io = {base->service_buffer, sizeof(base->service_buffer)};
    fn_appstore_read_t read_result;
    uint8_t result;
    memset(mappings, 0, MAPPINGS_SIZE);
    mappings[0] = 1;
    result = fn_init();
    if (result != FN_OK) return result;
    result = fn_appstore_read(&io, "config-nio", "mappings", 0,
                              mappings, MAPPINGS_SIZE, &read_result);
    if (result != FN_OK) return result;
    if (!(read_result.flags & FN_APPSTORE_READ_EXISTS) ||
        read_result.bytes_read != MAPPINGS_SIZE || mappings[0] != 1) {
        memset(mappings, 0, MAPPINGS_SIZE);
        mappings[0] = 1;
    }
    return FN_OK;
}

static uint8_t store_mappings(struct fujinet_disk_device_base *base,
                              const UBYTE *mappings)
{
    fn_appstore_io_t io = {base->service_buffer, sizeof(base->service_buffer)};
    fn_appstore_write_t write_result;
    uint8_t result = fn_init();
    if (result != FN_OK) return result;
    result = fn_appstore_write(&io, "config-nio", "mappings", 0,
                               mappings, MAPPINGS_SIZE, &write_result);
    if (result != FN_OK || write_result.bytes_written != MAPPINGS_SIZE)
        return FN_ERR_IO;
    return FN_OK;
}

static uint8_t request_unit_index(struct fujinet_disk_device_base *base,
                                  const struct IORequest *request)
{
    uint8_t i;
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i) {
        if (request->io_Unit == &base->exec_units[i]) return i;
    }
    return FUJINET_DISK_UNIT_COUNT;
}

static void signal_media_change(struct fujinet_disk_unit_state *unit)
{
    struct Node *node;
    for (node = unit->change_requests.lh_Head; node->ln_Succ != NULL;) {
        struct Node *next = node->ln_Succ;
        struct fujinet_change_registration *registration =
            (struct fujinet_change_registration *)node;
        if (registration->interrupt != NULL) Cause(registration->interrupt);
        node = next;
    }
    if (unit->legacy_change_interrupt != NULL)
        Cause(unit->legacy_change_interrupt);
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static uint8_t remove_all_change_requests(struct fujinet_disk_unit_state *unit,
                                          struct IORequest *request);

static struct fujinet_disk_device_base *device_init(
    register struct fujinet_disk_device_base *base FN_REGISTER("d0"),
    register BPTR segment_list FN_REGISTER("a0"),
    register struct ExecBase *sys_base FN_REGISTER("a6"))
{
    uint8_t i;
    SysBase = sys_base;
    base->segment_list = segment_list;
    base->worker_signal = -1;  /* -1 = no signal allocated */
    base->transport_closed = 0;
    fujinet_io_queue_init(&base->io_queue);
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i) {
        init_list(&base->units[i].change_requests);
        fujinet_nio_disk_context_init(&base->units[i].nio_context);
        fujinet_disk_driver_init(&base->units[i].driver,
                                 &fujinet_nio_disk_client,
                                  &base->units[i].nio_context, i);
    }
#ifndef FUJINET_DISK_NATIVE_TEST
    base->worker_signal = AllocSignal(-1);
    if (base->worker_signal == -1) return NULL;
    base->worker_stack = AllocMem(WORKER_STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (base->worker_stack == NULL) {
        FreeSignal(base->worker_signal);
        return NULL;
    }
    base->worker_task.tc_SPLower = base->worker_stack;
    base->worker_task.tc_SPUpper = (UBYTE *)base->worker_stack + WORKER_STACK_SIZE;
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
    register struct fujinet_disk_device_base *base FN_REGISTER("a6"))
{
    uint8_t slot;

    (void)flags;
    if (fujinet_disk_unit_to_slot(unit_number, &slot) != FN_OK) {
        request->io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    request->io_Error = 0;
    request->io_Device = &base->device;
    request->io_Unit = &base->exec_units[unit_number];
    ++base->device.dd_Library.lib_OpenCnt;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return &base->device;
}

static BPTR device_close(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_disk_device_base *base FN_REGISTER("a6"))
{
    uint8_t i;
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i) {
        (void)remove_all_change_requests(&base->units[i], request);
    }
    {
        fujinet_io_queue_node_t *queued =
            fujinet_io_queue_remove_request(&base->io_queue, request);
        if (queued != NULL) {
            FreeMem(queued, sizeof(*queued));
            request->io_Error = IOERR_ABORTED;
            ReplyMsg(&request->io_Message);
        }
    }
    request->io_Device = NULL;
    request->io_Unit = NULL;
    Disable();
    if (base->device.dd_Library.lib_OpenCnt != 0) {
        --base->device.dd_Library.lib_OpenCnt;
    }
    {
        uint8_t delayed_expunge =
            (uint8_t)(base->device.dd_Library.lib_OpenCnt == 0 &&
                      (base->device.dd_Library.lib_Flags & LIBF_DELEXP) != 0);
        Enable();
        if (delayed_expunge)
            return complete_pending_expunge(base);
    }
    return 0;
}

static BPTR device_expunge(
    register struct fujinet_disk_device_base *base FN_REGISTER("a6"))
{
    /* Defer while open or busy (OpenCnt != 0, io_queue nonempty, or
     * io_processing != 0). Complete only from Expunge or last CloseDevice on a
     * non-worker task; the worker must not tear the device down (nowhere to
     * return the seglist, and OpenDevice could revive a half-torn-down device). */
    base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
    return complete_pending_expunge(base);
}

static ULONG device_reserved(void)
{
    return 0;
}

static void abort_queued_unit(struct fujinet_disk_device_base *base,
                              uint8_t unit_index)
{
    fujinet_io_queue_node_t *node;
    while ((node = fujinet_io_queue_take_unit(&base->io_queue, unit_index)) != NULL) {
        struct IORequest *queued = (struct IORequest *)node->request;
        FreeMem(node, sizeof(*node));
        queued->io_Error = IOERR_ABORTED;
        ReplyMsg(&queued->io_Message);
    }
}

static uint8_t remove_change_request(struct fujinet_disk_unit_state *unit,
                                     struct IORequest *request)
{
    struct Node *node;
    for (node = unit->change_requests.lh_Head; node->ln_Succ != NULL;
         node = node->ln_Succ) {
        struct fujinet_change_registration *registration =
            (struct fujinet_change_registration *)node;
        if (registration->request == request) {
            Remove(node);
            FreeMem(registration, sizeof(*registration));
            return 1;
        }
    }
    return 0;
}

static struct fujinet_change_registration *find_change_request(
    struct fujinet_disk_unit_state *unit, struct IORequest *request)
{
    struct Node *node;
    for (node = unit->change_requests.lh_Head; node->ln_Succ != NULL;
         node = node->ln_Succ) {
        struct fujinet_change_registration *registration =
            (struct fujinet_change_registration *)node;
        if (registration->request == request)
            return registration;
    }
    return NULL;
}

static uint8_t remove_all_change_requests(struct fujinet_disk_unit_state *unit,
                                          struct IORequest *request)
{
    uint8_t removed = 0;
    while (remove_change_request(unit, request))
        removed = 1;
    return removed;
}

static void discard_change_requests(struct fujinet_disk_device_base *base)
{
    uint8_t i;
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i) {
        struct Node *node = base->units[i].change_requests.lh_Head;
        while (node->ln_Succ != NULL) {
            struct Node *next = node->ln_Succ;
            Remove(node);
            FreeMem(node, sizeof(struct fujinet_change_registration));
            node = next;
        }
    }
}

/* Returns stored segment_list on successful idle teardown, or 0 if deferred. */
static BPTR complete_pending_expunge(struct fujinet_disk_device_base *base)
{
    BPTR segment_list;
    uint8_t i;

    Disable();
    if ((base->device.dd_Library.lib_Flags & LIBF_DELEXP) == 0 ||
        base->device.dd_Library.lib_OpenCnt != 0 ||
        base->io_queue.head != NULL ||
        base->io_processing != 0) {
        Enable();
        return 0;
    }
    /* Snapshot seglist while still under Disable; safe because the gate above
     * guarantees no concurrent modification. */
    segment_list = base->segment_list;
    base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    Enable();

#ifndef FUJINET_DISK_NATIVE_TEST
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
    discard_change_requests(base);
    Disable();
    if (!base->transport_closed) {
        base->transport_closed = 1;
        Enable();
        fn_transport_close();
    } else {
        Enable();
    }
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i)
        base->units[i].driver.client_initialized = 0;
#ifndef FUJINET_DISK_NATIVE_TEST
    Forbid();
    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - base->device.dd_Library.lib_NegSize,
            (ULONG)base->device.dd_Library.lib_NegSize +
                (ULONG)base->device.dd_Library.lib_PosSize);
    Permit();
#endif
    return segment_list;
}

static struct IORequest *next_runnable_request(
    struct fujinet_disk_device_base *base)
{
    uint8_t stopped[FUJINET_DISK_UNIT_COUNT];
    uint8_t i;
    fujinet_io_queue_node_t *node;
    for (i = 0; i < FUJINET_DISK_UNIT_COUNT; ++i)
        stopped[i] = base->units[i].stopped;
    node = fujinet_io_queue_next(&base->io_queue, stopped,
                                 FUJINET_DISK_UNIT_COUNT, CMD_START,
                                 CMD_FLUSH);
    if (node == NULL) return NULL;
    {
        struct IORequest *request = (struct IORequest *)node->request;
        FreeMem(node, sizeof(*node));
        return request;
    }
}

static void device_process_request(struct IORequest *request,
                                   struct fujinet_disk_device_base *base)
{
    struct IOStdReq *io = (struct IOStdReq *)request;
    struct fujinet_disk_unit_state *unit;
    uint8_t unit_index;
    uint8_t result;
    UWORD trace_index = FUJINET_DISK_TRACE_CAPACITY;

    unit_index = request_unit_index(base, request);
    if (unit_index >= FUJINET_DISK_UNIT_COUNT) {
        request->io_Error = IOERR_ABORTED;
        if ((request->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&request->io_Message);
        return;
    }
    unit = &base->units[unit_index];
    request->io_Error = 0;
    io->io_Actual = 0;

    /* A failed remote ClearChanged never rolls back a committed local media
     * transition. Retry it opportunistically on subsequent unit activity. */
    (void)fujinet_disk_acknowledge_change(&unit->driver, unit_index);

    if (request->io_Command != FUJINET_DISK_CMD_TRACE &&
        base->trace.count < FUJINET_DISK_TRACE_CAPACITY) {
        trace_index = base->trace.count++;
        base->trace.commands[trace_index] = request->io_Command;
        base->trace.offsets[trace_index] = io->io_Offset;
        base->trace.lengths[trace_index] = io->io_Length;
    }

    switch (request->io_Command) {
    case FUJINET_DISK_CMD_MOUNT:
    case FUJINET_DISK_CMD_MOUNT_WRITABLE:
        {
        uint32_t old_count = unit->driver.change_count;
        if (io->io_Data == NULL || io->io_Length == 0 ||
            io->io_Length > sizeof(base->request_uri) ||
            ((const char *)io->io_Data)[io->io_Length - 1] != '\0') {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            memcpy(base->request_uri, io->io_Data, io->io_Length);
        if (request->io_Command == FUJINET_DISK_CMD_MOUNT_WRITABLE)
            result = fujinet_disk_mount_mode(&unit->driver,
                                             unit_index,
                                             base->request_uri, 1);
        else
            result = fujinet_disk_mount(&unit->driver,
                                        unit_index,
                                        base->request_uri);
        request->io_Error = result_to_io_error(result);
        if (unit->driver.change_count != old_count) signal_media_change(unit);
        }
        }
        break;
    case FUJINET_DISK_CMD_MOUNT_CATALOG:
        {
        struct fujinet_disk_catalog_mount *catalog =
            (struct fujinet_disk_catalog_mount *)io->io_Data;
        UBYTE old_mappings[MAPPINGS_SIZE];
        UBYTE new_mappings[MAPPINGS_SIZE];
        UBYTE mappings_loaded = 0;
        uint32_t old_count = unit->driver.change_count;
        if (catalog == NULL || io->io_Length < sizeof(*catalog)) {
            request->io_Error = IOERR_BADLENGTH;
            break;
        }
        result = resolve_catalog_slot(base, catalog->catalog_slot,
                                      base->catalog_uri,
                                      sizeof(base->catalog_uri));
        if (result == FN_OK)
            result = load_mappings(base, old_mappings);
        if (result == FN_OK) mappings_loaded = 1;
        if (result == FN_OK) {
            memcpy(new_mappings, old_mappings, MAPPINGS_SIZE);
            new_mappings[1 + unit_index * 2] =
                (UBYTE)(1 | (catalog->writable ? 0 : 2));
            new_mappings[2 + unit_index * 2] = catalog->catalog_slot;
            result = store_mappings(base, new_mappings);
        }
        if (result == FN_OK)
            result = fujinet_disk_mount_mode(&unit->driver, unit_index,
                                             base->catalog_uri,
                                             catalog->writable != 0);
        if (result != FN_OK && mappings_loaded)
            (void)store_mappings(base, old_mappings);
        request->io_Error = result_to_io_error(result);
        if (unit->driver.change_count != old_count) signal_media_change(unit);
        }
        break;
    case FUJINET_DISK_CMD_INSPECT_CATALOG:
        {
        struct fujinet_disk_catalog_inspection *catalog =
            (struct fujinet_disk_catalog_inspection *)io->io_Data;
        if (catalog == NULL || io->io_Length < sizeof(*catalog) ||
            unit->driver.client == NULL || unit->driver.client->inspect == NULL) {
            request->io_Error = IOERR_BADLENGTH;
            break;
        }
        result = fujinet_disk_inspect_catalog(unit->driver.client,
                                               unit->driver.client_context,
                                               (fujinet_disk_catalog_resolve_fn)resolve_catalog_slot,
                                               base, catalog->catalog_slot,
                                               base->catalog_uri,
                                               sizeof(base->catalog_uri),
                                               &catalog->inspection);
        request->io_Error = result_to_io_error(result);
        if (result == FN_OK)
            io->io_Actual = sizeof(*catalog);
        }
        break;
    case FUJINET_DISK_CMD_TRACE:
        if (io->io_Data == NULL || io->io_Length < sizeof(base->trace)) {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            memcpy(io->io_Data, &base->trace, sizeof(base->trace));
            io->io_Actual = sizeof(base->trace);
        }
        break;
    case CMD_READ:
    case ETD_READ:
        {
        ULONG media_bytes = unit->driver.mounted ?
            (ULONG)unit->driver.media.sector_count * unit->driver.media.sector_size : 0;
        if (!unit->driver.mounted) {
            request->io_Error = TDERR_DiskChanged;
        } else if (io->io_Data == NULL || io->io_Offset >= media_bytes ||
                   io->io_Length > media_bytes - io->io_Offset) {
            request->io_Error = IOERR_BADADDRESS;
        } else if (io->io_Length == 0 ||
                   (io->io_Offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
                   (io->io_Length % FUJINET_DISK_BLOCK_SIZE) != 0) {
            request->io_Error = IOERR_BADLENGTH;
        } else if (request->io_Command == ETD_READ &&
                   ((struct IOExtTD *)request)->iotd_Count < unit->driver.change_count) {
            request->io_Error = TDERR_DiskChanged;
        } else if (request->io_Command == ETD_READ &&
                   ((struct IOExtTD *)request)->iotd_SecLabel != NULL) {
            request->io_Error = IOERR_NOCMD;
        } else {
            result = fujinet_disk_read(&unit->driver, unit_index,
                                       io->io_Offset, (uint8_t *)io->io_Data,
                                       io->io_Length, &io->io_Actual);
            request->io_Error = result_to_io_error(result);
        }
        }
        break;
    case CMD_WRITE:
    case ETD_WRITE:
        {
        ULONG media_bytes = unit->driver.mounted ?
            (ULONG)unit->driver.media.sector_count * unit->driver.media.sector_size : 0;
        if (!unit->driver.mounted) {
            request->io_Error = TDERR_DiskChanged;
        } else if (!unit->driver.writable) {
            request->io_Error = TDERR_WriteProt;
        } else if (io->io_Data == NULL || io->io_Offset >= media_bytes ||
                   io->io_Length > media_bytes - io->io_Offset) {
            request->io_Error = IOERR_BADADDRESS;
        } else if (io->io_Length == 0 ||
                   (io->io_Offset % FUJINET_DISK_BLOCK_SIZE) != 0 ||
                   (io->io_Length % FUJINET_DISK_BLOCK_SIZE) != 0) {
            request->io_Error = IOERR_BADLENGTH;
        } else if (request->io_Command == ETD_WRITE &&
                   (((struct IOExtTD *)request)->iotd_Count <
                    unit->driver.change_count)) {
            request->io_Error = TDERR_DiskChanged;
        } else if (request->io_Command == ETD_WRITE &&
                   ((struct IOExtTD *)request)->iotd_SecLabel != NULL) {
            request->io_Error = IOERR_NOCMD;
        } else {
            result = fujinet_disk_write(&unit->driver,
                                        unit_index,
                                        io->io_Offset,
                                        (const uint8_t *)io->io_Data,
                                        io->io_Length, &io->io_Actual);
            request->io_Error = result_to_io_error(result);
        }
        }
        break;
    case CMD_RESET:
    case CMD_UPDATE:
    case ETD_UPDATE:
        if (request->io_Command == ETD_UPDATE &&
            ((struct IOExtTD *)request)->iotd_Count < unit->driver.change_count)
            request->io_Error = TDERR_DiskChanged;
        else {
            result = fujinet_disk_flush(&unit->driver, unit_index);
            request->io_Error = result_to_io_error(result);
        }
        break;
    case CMD_CLEAR:
    case ETD_CLEAR:
        if (request->io_Command == ETD_CLEAR &&
            ((struct IOExtTD *)request)->iotd_Count < unit->driver.change_count)
            request->io_Error = TDERR_DiskChanged;
        break;
    case CMD_START:
        unit->stopped = 0;
        break;
    case CMD_STOP:
        unit->stopped = 1;
        break;
    case CMD_FLUSH:
        abort_queued_unit(base, unit_index);
    case TD_MOTOR:
    case TD_SEEK:
        break;
    case TD_CHANGENUM:
        io->io_Actual = unit->driver.change_count;
        break;
    case TD_CHANGESTATE:
        io->io_Actual = unit->driver.mounted ? 0 : 1;
        break;
    case TD_PROTSTATUS:
        io->io_Actual = (!unit->driver.mounted || !unit->driver.writable) ? 1 : 0;
        break;
    case TD_EJECT:
        {
        UBYTE old_mappings[MAPPINGS_SIZE];
        UBYTE new_mappings[MAPPINGS_SIZE];
        UBYTE mappings_loaded = 0;
        uint32_t old_count = unit->driver.change_count;
        result = load_mappings(base, old_mappings);
        if (result == FN_OK) mappings_loaded = 1;
        if (result == FN_OK) {
            memcpy(new_mappings, old_mappings, MAPPINGS_SIZE);
            new_mappings[1 + unit_index * 2] = 0;
            new_mappings[2 + unit_index * 2] = 0;
            result = store_mappings(base, new_mappings);
        }
        if (result == FN_OK)
            result = fujinet_disk_eject(&unit->driver, unit_index);
        if (result != FN_OK && mappings_loaded)
            (void)store_mappings(base, old_mappings);
        request->io_Error = result_to_io_error(result);
        if (unit->driver.change_count != old_count) signal_media_change(unit);
        }
        break;
    case TD_GETDRIVETYPE:
        io->io_Actual = DRIVE3_5;
        break;
    case TD_GETNUMTRACKS:
        io->io_Actual = unit->driver.mounted ? 160 : 0;
        break;
    case TD_GETGEOMETRY:
        if (io->io_Data == NULL || io->io_Length < sizeof(struct DriveGeometry)) {
            request->io_Error = IOERR_BADLENGTH;
        } else if (!unit->driver.mounted) {
            request->io_Error = TDERR_DiskChanged;
        } else {
            struct DriveGeometry *geometry = (struct DriveGeometry *)io->io_Data;
            ULONG total = (ULONG)unit->driver.media.sector_count;
            memset(geometry, 0, sizeof(*geometry));
            geometry->dg_SectorSize = FUJINET_DISK_BLOCK_SIZE;
            geometry->dg_TotalSectors = total;
            geometry->dg_Cylinders = 80;
            geometry->dg_Heads = 2;
            /* HD ADF has 22 sectors/track; DD has 11. */
            if (total == FUJINET_HD_ADF_BLOCK_COUNT) {
                geometry->dg_CylSectors = 44;
                geometry->dg_TrackSectors = 22;
            } else {
                geometry->dg_CylSectors = 22;
                geometry->dg_TrackSectors = 11;
            }
            geometry->dg_BufMemType = MEMF_PUBLIC;
            geometry->dg_DeviceType = DG_DIRECT_ACCESS;
            geometry->dg_Flags = DGF_REMOVABLE;
            io->io_Actual = sizeof(*geometry);
        }
        break;
    case TD_ADDCHANGEINT:
        {
        struct fujinet_change_registration *registration =
            find_change_request(unit, request);
        if (registration == NULL) {
            registration = AllocMem(sizeof(*registration), MEMF_PUBLIC | MEMF_CLEAR);
            if (registration == NULL) {
                request->io_Error = TDERR_NotSpecified;
                break;
            }
            registration->request = request;
            AddTail(&unit->change_requests, &registration->node);
        }
        registration->interrupt = (struct Interrupt *)io->io_Data;
        }
        if (trace_index < FUJINET_DISK_TRACE_CAPACITY) {
            base->trace.actuals[trace_index] = io->io_Actual;
            base->trace.errors[trace_index] = request->io_Error;
        }
        request->io_Flags &= (UBYTE)~IOF_QUICK;
        return;
    case TD_REMOVE:
        /* TD_REMOVE predates TD_ADDCHANGEINT.  It synchronously installs the
         * unit's single legacy change interrupt, or removes it when io_Data
         * is NULL.  Unlike TD_ADDCHANGEINT, its IORequest is never retained. */
        if (io->io_Data != NULL && unit->legacy_change_interrupt != NULL &&
            unit->legacy_change_interrupt != (struct Interrupt *)io->io_Data) {
            request->io_Error = IOERR_UNITBUSY;
        } else {
            unit->legacy_change_interrupt = (struct Interrupt *)io->io_Data;
        }
        /* Classic trackdisk completes TD_REMOVE as a normal reply even when
         * the caller entered with IOF_QUICK. TD_ADDCHANGEINT is the retained
         * request; TD_REMOVE is not. */
        request->io_Flags &= (UBYTE)~IOF_QUICK;
        break;
    case TD_REMCHANGEINT:
        (void)remove_all_change_requests(unit, request);
        break;
    case NSCMD_DEVICEQUERY:
        if (io->io_Data == NULL ||
            io->io_Length < sizeof(struct NSDeviceQueryResult)) {
            request->io_Error = IOERR_BADLENGTH;
        } else {
            struct NSDeviceQueryResult *query =
                (struct NSDeviceQueryResult *)io->io_Data;
            query->nsdqr_DevQueryFormat = 0;
            query->nsdqr_SizeAvailable = sizeof(*query);
            query->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
            query->nsdqr_DeviceSubType = 0;
            query->nsdqr_SupportedCommands = (APTR)supported_commands;
            io->io_Actual = sizeof(*query);
        }
        break;
    default:
        request->io_Error = IOERR_NOCMD;
        break;
    }

    if (trace_index < FUJINET_DISK_TRACE_CAPACITY) {
        base->trace.actuals[trace_index] = io->io_Actual;
        base->trace.errors[trace_index] = request->io_Error;
    }

    if ((request->io_Flags & IOF_QUICK) == 0) {
        ReplyMsg(&request->io_Message);
    }
}

static void worker_drain(struct fujinet_disk_device_base *base)
{
    for (;;) {
        struct IORequest *request;

        Disable();
        request = next_runnable_request(base);
        if (request != NULL) {
            Enable();
            device_process_request(request, base);
            continue;
        }
        base->io_processing = 0;
        Enable();
        return;
    }
}

#ifndef FUJINET_DISK_NATIVE_TEST
static void device_worker_entry(void)
{
    struct fujinet_disk_device_base *base =
        (struct fujinet_disk_device_base *)FindTask(NULL)->tc_UserData;
    ULONG signal_mask = 1UL << base->worker_signal;

    for (;;) {
        Wait(signal_mask);
        worker_drain(base);
    }
}
#endif

static void device_begin_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_disk_device_base *base FN_REGISTER("a6"))
{
    uint8_t unit_index;
    uint8_t local_quick = 0;
    fujinet_io_queue_node_t *node;

    Disable();
    unit_index = request_unit_index(base, request);
    if (unit_index >= FUJINET_DISK_UNIT_COUNT) {
        Enable();
        request->io_Error = IOERR_OPENFAIL;
        if ((request->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&request->io_Message);
        return;
    }
    /* TD_REMCHANGEINT reuses the request retained by TD_ADDCHANGEINT, so it
     * must never enter the ordinary FIFO. */
    if (request->io_Command == TD_REMCHANGEINT) {
        remove_change_request(&base->units[unit_index], request);
        request->io_Error = 0;
        ((struct IOStdReq *)request)->io_Actual = 0;
        Enable();
        if ((request->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&request->io_Message);
        return;
    }
    if (request->io_Command == CMD_FLUSH) {
        abort_queued_unit(base, unit_index);
        request->io_Error = 0;
        ((struct IOStdReq *)request)->io_Actual = 0;
        Enable();
        if ((request->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&request->io_Message);
        return;
    }
    unit_index = request_unit_index(base, request);
    switch (request->io_Command) {
    case TD_REMOVE:
        /* TD_REMOVE is the synchronous legacy interrupt install/remove ABI;
         * its request is never retained in the data-path FIFO. */
        if (((struct IOStdReq *)request)->io_Data != NULL &&
            base->units[unit_index].legacy_change_interrupt != NULL &&
            base->units[unit_index].legacy_change_interrupt !=
                (struct Interrupt *)((struct IOStdReq *)request)->io_Data)
            request->io_Error = IOERR_UNITBUSY;
        else {
            base->units[unit_index].legacy_change_interrupt =
                (struct Interrupt *)((struct IOStdReq *)request)->io_Data;
            request->io_Error = 0;
        }
        ((struct IOStdReq *)request)->io_Actual = 0;
        request->io_Flags &= (UBYTE)~IOF_QUICK;
        Enable();
        ReplyMsg(&request->io_Message);
        return;
    case TD_CHANGENUM:
        ((struct IOStdReq *)request)->io_Actual =
            base->units[unit_index].driver.change_count;
        local_quick = 1;
        break;
    case TD_CHANGESTATE:
        ((struct IOStdReq *)request)->io_Actual =
            base->units[unit_index].driver.mounted ? 0 : 1;
        local_quick = 1;
        break;
    case TD_PROTSTATUS:
        ((struct IOStdReq *)request)->io_Actual =
            (!base->units[unit_index].driver.mounted ||
             !base->units[unit_index].driver.writable) ? 1 : 0;
        local_quick = 1;
        break;
    default: break;
    }
    /* Status is locally computed, but while the worker owns a preceding
     * request it must retain FIFO ordering with that request. */
    if (local_quick && !base->io_processing) {
        request->io_Error = 0;
        Enable();
        if ((request->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&request->io_Message);
        return;
    }
    request->io_Flags &= (UBYTE)~IOF_QUICK;
    node = AllocMem(sizeof(*node), MEMF_PUBLIC | MEMF_CLEAR);
    if (node == NULL) {
        request->io_Error = TDERR_NotSpecified;
        Enable();
        ReplyMsg(&request->io_Message);
        return;
    }
    /* Private FIFO insertion bypasses PutMsg(), so preserve the Exec message
     * state transition before a later worker ReplyMsg(). */
    request->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    fujinet_io_queue_append(&base->io_queue, node, request, unit_index,
                            request->io_Command);
    base->io_processing = 1;
#ifndef FUJINET_DISK_NATIVE_TEST
    Signal(&base->worker_task, 1UL << base->worker_signal);
#endif
    Enable();
#ifdef FUJINET_DISK_NATIVE_TEST
    if (native_drain_after_begin)
        worker_drain(base);
#endif
}

static LONG device_abort_io(
    register struct IORequest *request FN_REGISTER("a1"),
    register struct fujinet_disk_device_base *base FN_REGISTER("a6"))
{
    struct Node *node;
    uint8_t unit_index = request_unit_index(base, request);
    Disable();
    if (unit_index < FUJINET_DISK_UNIT_COUNT) {
        for (node = base->units[unit_index].change_requests.lh_Head;
             node->ln_Succ != NULL; node = node->ln_Succ) {
            struct fujinet_change_registration *registration =
                (struct fujinet_change_registration *)node;
            if (registration->request == request) {
                (void)remove_all_change_requests(&base->units[unit_index], request);
                request->io_Error = IOERR_ABORTED;
                ReplyMsg(&request->io_Message);
                Enable();
                return 0;
            }
        }
    }
    {
        fujinet_io_queue_node_t *queued =
            fujinet_io_queue_remove_request(&base->io_queue, request);
        if (queued != NULL) {
            FreeMem(queued, sizeof(*queued));
            request->io_Error = IOERR_ABORTED;
            ReplyMsg(&request->io_Message);
            Enable();
            return 0;
        }
    }
    Enable();
    return IOERR_NOCMD;
}

#ifndef FUJINET_DISK_NATIVE_TEST
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
    sizeof(struct fujinet_disk_device_base),
    (ULONG)device_vectors,
    0,
    (ULONG)device_init
};

#ifndef FUJINET_DISK_NATIVE_TEST
static const char device_end;
#endif

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

#ifdef FUJINET_DISK_NATIVE_TEST
static struct fujinet_disk_device_base native_test_base;
static struct ExecBase native_test_sys_base;

void fujinet_disk_native_test_reset(void)
{
    memset(&native_test_base, 0, sizeof(native_test_base));
    native_drain_after_begin = 1;
    (void)device_init(&native_test_base, (BPTR)1, &native_test_sys_base);
}

void fujinet_disk_native_test_commit(uint8_t unit, uint32_t sector_count)
{
    fujinet_disk_driver_t *driver;
    if (unit >= FUJINET_DISK_UNIT_COUNT) return;
    driver = &native_test_base.units[unit].driver;
    driver->mounted = 1;
    driver->writable = 0;
    driver->media.flags = FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    driver->media.slot = (uint8_t)(FUJINET_DISK_FIRST_SLOT + unit);
    driver->media.type = FN_DISK_TYPE_RAW;
    driver->media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    driver->media.sector_count = sector_count;
}

void fujinet_disk_native_test_signal_change(uint8_t unit)
{
    if (unit >= FUJINET_DISK_UNIT_COUNT) return;
    signal_media_change(&native_test_base.units[unit]);
}

void fujinet_disk_native_test_begin_io(uint8_t unit, struct IORequest *request)
{
    if (unit >= FUJINET_DISK_UNIT_COUNT) return;
    request->io_Unit = &native_test_base.exec_units[unit];
    device_begin_io(request, &native_test_base);
}

void fujinet_disk_native_test_queue_io(uint8_t unit, struct IORequest *request)
{
    uint8_t previous = native_drain_after_begin;
    native_drain_after_begin = 0;
    fujinet_disk_native_test_begin_io(unit, request);
    native_drain_after_begin = previous;
}

void fujinet_disk_native_test_drain(void)
{
    worker_drain(&native_test_base);
}

struct Device *fujinet_disk_native_test_open(struct IORequest *request,
                                             ULONG unit)
{
    return device_open(request, unit, 0, &native_test_base);
}

BPTR fujinet_disk_native_test_close(struct IORequest *request)
{
    return device_close(request, &native_test_base);
}

BPTR fujinet_disk_native_test_expunge(void)
{
    return device_expunge(&native_test_base);
}

UWORD fujinet_disk_native_test_open_cnt(void)
{
    return native_test_base.device.dd_Library.lib_OpenCnt;
}

UBYTE fujinet_disk_native_test_lib_flags(void)
{
    return native_test_base.device.dd_Library.lib_Flags;
}

uint8_t fujinet_disk_native_test_client_initialized(uint8_t unit)
{
    if (unit >= FUJINET_DISK_UNIT_COUNT) return 0;
    return native_test_base.units[unit].driver.client_initialized;
}

void fujinet_disk_native_test_set_client_initialized(uint8_t unit, uint8_t value)
{
    if (unit >= FUJINET_DISK_UNIT_COUNT) return;
    native_test_base.units[unit].driver.client_initialized = value;
}

uint8_t fujinet_disk_native_test_change_int_count(uint8_t unit)
{
    uint8_t count = 0;
    struct Node *node;
    if (unit >= FUJINET_DISK_UNIT_COUNT) return 0;
    for (node = native_test_base.units[unit].change_requests.lh_Head;
         node->ln_Succ != NULL; node = node->ln_Succ)
        ++count;
    return count;
}
#endif

static const char device_end = 0;
