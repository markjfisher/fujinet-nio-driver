#include <devices/trackdisk.h>
#include <exec/libraries.h>
#include <fujinet-amiga-disk/device.h>

#include "fujinet_disk_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fujinet_disk_native_test_reset(void);
void fujinet_disk_native_test_commit(uint8_t unit, uint32_t sector_count);
void fujinet_disk_native_test_signal_change(uint8_t unit);
void fujinet_disk_native_test_begin_io(uint8_t unit, struct IORequest *request);
void fujinet_disk_native_test_queue_io(uint8_t unit, struct IORequest *request);
void fujinet_disk_native_test_drain(void);
struct Device *fujinet_disk_native_test_open(struct IORequest *request,
                                             ULONG unit);
BPTR fujinet_disk_native_test_close(struct IORequest *request);
BPTR fujinet_disk_native_test_expunge(void);
UWORD fujinet_disk_native_test_open_cnt(void);
UBYTE fujinet_disk_native_test_lib_flags(void);
uint8_t fujinet_disk_native_test_client_initialized(uint8_t unit);
void fujinet_disk_native_test_set_client_initialized(uint8_t unit, uint8_t value);
uint8_t fujinet_disk_native_test_change_int_count(uint8_t unit);

static unsigned failures;
static unsigned replies;
static unsigned causes;
static unsigned transport_close_count;

#define NATIVE_SEGLIST ((BPTR)1)

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

void Disable(void) {}
void Enable(void) {}
void ReplyMsg(struct Message *message) { (void)message; ++replies; }
void Cause(struct Interrupt *interrupt) { (void)interrupt; ++causes; }
void Remove(struct Node *node)
{
    if (node->ln_Pred != NULL) node->ln_Pred->ln_Succ = node->ln_Succ;
    if (node->ln_Succ != NULL) node->ln_Succ->ln_Pred = node->ln_Pred;
}
void AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
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
void fn_transport_close(void) { ++transport_close_count; }
uint8_t fn_init(void) { return FN_ERR_NOT_READY; }
uint8_t fn_appstore_read(fn_appstore_io_t *io, const char *namespace_name,
                         const char *key, uint32_t offset, uint8_t *buf,
                         uint16_t max_len, fn_appstore_read_t *out)
{
    (void)io; (void)namespace_name; (void)key; (void)offset; (void)buf;
    (void)max_len; (void)out;
    return FN_ERR_NOT_READY;
}
uint8_t fn_appstore_write(fn_appstore_io_t *io, const char *namespace_name,
                          const char *key, uint32_t offset, const uint8_t *data,
                          uint16_t length, fn_appstore_write_t *out)
{
    (void)io; (void)namespace_name; (void)key; (void)offset; (void)data;
    (void)length; (void)out;
    return FN_ERR_NOT_READY;
}
uint8_t fn_slot_catalog_get(fn_slot_catalog_io_t *io, uint8_t index,
                            fn_slot_catalog_entry_t *out)
{
    (void)io; (void)index; (void)out;
    return FN_ERR_NOT_READY;
}

static uint8_t unused_client_init(void *context) { (void)context; return FN_ERR_NOT_READY; }
static uint8_t unused_client_mount(void *context, uint8_t slot, const char *uri,
                                   uint8_t readonly, uint8_t type,
                                   uint16_t hint, fn_disk_info_t *info)
{ (void)context; (void)slot; (void)uri; (void)readonly; (void)type; (void)hint; (void)info; return FN_ERR_NOT_READY; }
static uint8_t unused_client_info(void *context, uint8_t slot, fn_disk_info_t *info)
{ (void)context; (void)slot; (void)info; return FN_ERR_NOT_READY; }
static uint8_t unused_client_read(void *context, uint8_t slot, uint32_t lba,
                                  uint8_t *data, uint16_t capacity, uint16_t *length)
{ (void)context; (void)slot; (void)lba; (void)data; (void)capacity; (void)length; return FN_ERR_NOT_READY; }
static uint8_t unused_client_write(void *context, uint8_t slot, uint32_t lba,
                                   const uint8_t *data, uint16_t length)
{ (void)context; (void)slot; (void)lba; (void)data; (void)length; return FN_ERR_NOT_READY; }
static uint8_t unused_client_slot(void *context, uint8_t slot)
{ (void)context; (void)slot; return FN_ERR_NOT_READY; }
static uint8_t unused_client_inspect(void *context, const char *uri,
                                     fn_disk_inspection_t *inspection)
{ (void)context; (void)uri; (void)inspection; return FN_ERR_NOT_READY; }

const fujinet_disk_client_t fujinet_nio_disk_client = {
    unused_client_init, unused_client_mount, unused_client_info,
    unused_client_read, unused_client_write, unused_client_slot,
    unused_client_slot, unused_client_slot, unused_client_inspect
};

uint8_t fujinet_nio_disk_context_init(fujinet_nio_disk_context_t *context)
{
    memset(context, 0, sizeof(*context));
    return FN_OK;
}

static void query_geometry(uint8_t unit, struct DriveGeometry *geometry,
                           struct IOExtTD *request)
{
    memset(geometry, 0, sizeof(*geometry));
    memset(request, 0, sizeof(*request));
    request->iotd_Req.io.io_Command = TD_GETGEOMETRY;
    request->iotd_Req.io_Data = geometry;
    request->iotd_Req.io_Length = sizeof(*geometry);
    fujinet_disk_native_test_begin_io(unit, &request->iotd_Req.io);
}

static void check_dd_geometry(const struct DriveGeometry *geometry)
{
    CHECK("DD sector size", geometry->dg_SectorSize == 512);
    CHECK("DD total sectors", geometry->dg_TotalSectors == 1760);
    CHECK("DD cylinders", geometry->dg_Cylinders == 80);
    CHECK("DD heads", geometry->dg_Heads == 2);
    CHECK("DD cylinder sectors", geometry->dg_CylSectors == 22);
    CHECK("DD track sectors", geometry->dg_TrackSectors == 11);
}

static void check_hd_geometry(const struct DriveGeometry *geometry)
{
    CHECK("HD sector size", geometry->dg_SectorSize == 512);
    CHECK("HD total sectors", geometry->dg_TotalSectors == 3520);
    CHECK("HD cylinders", geometry->dg_Cylinders == 80);
    CHECK("HD heads", geometry->dg_Heads == 2);
    CHECK("HD cylinder sectors", geometry->dg_CylSectors == 44);
    CHECK("HD track sectors", geometry->dg_TrackSectors == 22);
}

static void test_dd_hd_geometry_and_unit_isolation(void)
{
    struct DriveGeometry dd, hd, dd_again;
    struct IOExtTD request;

    fujinet_disk_native_test_reset();
    fujinet_disk_native_test_commit(0, FUJINET_DD_ADF_BLOCK_COUNT);
    fujinet_disk_native_test_commit(1, FUJINET_HD_ADF_BLOCK_COUNT);

    query_geometry(0, &dd, &request);
    CHECK("DD TD_GETGEOMETRY succeeds", request.iotd_Req.io.io_Error == 0);
    check_dd_geometry(&dd);

    query_geometry(1, &hd, &request);
    CHECK("HD TD_GETGEOMETRY succeeds", request.iotd_Req.io.io_Error == 0);
    check_hd_geometry(&hd);

    query_geometry(0, &dd_again, &request);
    CHECK("DD remains independent after HD query", request.iotd_Req.io.io_Error == 0);
    check_dd_geometry(&dd_again);
}

static void test_public_mount_abi_reaches_resident_dispatcher(void)
{
    struct IOExtTD request;
    const char uri[] = "host:/images/test.adf";

    fujinet_disk_native_test_reset();
    memset(&request, 0, sizeof(request));
    request.iotd_Req.io.io_Command = FUJINET_DISK_CMD_MOUNT;
    request.iotd_Req.io_Data = (APTR)uri;
    request.iotd_Req.io_Length = sizeof(uri);
    fujinet_disk_native_test_begin_io(0, &request.iotd_Req.io);

    CHECK("public FMOUNT ABI command reaches resident dispatcher",
          request.iotd_Req.io.io_Error == TDERR_DiskChanged);
}

static void test_legacy_td_remove_is_synchronous(void)
{
    struct IOExtTD request;
    struct Interrupt interrupt;

    fujinet_disk_native_test_reset();
    memset(&request, 0, sizeof(request));
    memset(&interrupt, 0, sizeof(interrupt));
    replies = 0;
    causes = 0;

    request.iotd_Req.io.io_Command = TD_REMOVE;
    request.iotd_Req.io.io_Flags = IOF_QUICK;
    request.iotd_Req.io_Data = &interrupt;
    request.iotd_Req.io_Actual = 99;
    fujinet_disk_native_test_begin_io(0, &request.iotd_Req.io);
    CHECK("TD_REMOVE install succeeds", request.iotd_Req.io.io_Error == 0);
    CHECK("TD_REMOVE install clears quick for reply",
          (request.iotd_Req.io.io_Flags & IOF_QUICK) == 0);
    CHECK("TD_REMOVE install returns zero actual", request.iotd_Req.io_Actual == 0);
    CHECK("TD_REMOVE install is replied", replies == 1);

    fujinet_disk_native_test_signal_change(0);
    CHECK("legacy TD_REMOVE interrupt is caused on media change", causes == 1);

    request.iotd_Req.io.io_Command = TD_REMOVE;
    request.iotd_Req.io.io_Flags = IOF_QUICK;
    request.iotd_Req.io_Data = NULL;
    fujinet_disk_native_test_begin_io(0, &request.iotd_Req.io);
    CHECK("TD_REMOVE removal succeeds", request.iotd_Req.io.io_Error == 0);
    CHECK("TD_REMOVE removal clears quick for reply",
          (request.iotd_Req.io.io_Flags & IOF_QUICK) == 0);
    fujinet_disk_native_test_signal_change(0);
    CHECK("removed legacy interrupt is not caused again", causes == 1);
}

static const char k_mount_uri[] = "host:/images/test.adf";

static void issue_mount(uint8_t unit, struct IOExtTD *request, int queue_only)
{
    memset(request, 0, sizeof(*request));
    request->iotd_Req.io.io_Command = FUJINET_DISK_CMD_MOUNT;
    request->iotd_Req.io_Data = (APTR)k_mount_uri;
    request->iotd_Req.io_Length = sizeof(k_mount_uri);
    if (queue_only)
        fujinet_disk_native_test_queue_io(unit, &request->iotd_Req.io);
    else
        fujinet_disk_native_test_begin_io(unit, &request->iotd_Req.io);
}

static void reset_lifecycle(void)
{
    fujinet_disk_native_test_reset();
    transport_close_count = 0;
}

static void test_ordinary_idle_does_not_close(void)
{
    struct IOExtTD request;

    reset_lifecycle();
    fujinet_disk_native_test_set_client_initialized(0, 1);
    issue_mount(0, &request, 0);
    CHECK("ordinary idle does not close", transport_close_count == 0);
    CHECK("ordinary idle leaves client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 1);

    issue_mount(0, &request, 0);
    CHECK("work after idle still does not close", transport_close_count == 0);
    CHECK("work after idle keeps client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 1);
}

static void test_safe_idle_expunge_closes_once(void)
{
    struct IOExtTD request;

    reset_lifecycle();
    fujinet_disk_native_test_set_client_initialized(0, 1);
    CHECK("safe expunge OpenCnt 0", fujinet_disk_native_test_open_cnt() == 0);
    CHECK("safe expunge returns sentinel",
          fujinet_disk_native_test_expunge() == NATIVE_SEGLIST);
    CHECK("safe idle expunge closes once", transport_close_count == 1);
    CHECK("safe idle expunge clears client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 0);
    CHECK("safe idle expunge clears DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) == 0);

    issue_mount(0, &request, 0);
    fujinet_disk_native_test_drain();
    CHECK("repeat drain after idle expunge does not close again",
          transport_close_count == 1);
    CHECK("repeat expunge after complete returns sentinel",
          fujinet_disk_native_test_expunge() == NATIVE_SEGLIST);
    CHECK("repeat expunge after complete does not close again",
          transport_close_count == 1);
}

static void test_expunge_deferred_until_last_close(void)
{
    struct IOExtTD open_req;
    struct IOExtTD change_req;
    struct Interrupt interrupt;

    reset_lifecycle();
    memset(&open_req, 0, sizeof(open_req));
    memset(&change_req, 0, sizeof(change_req));
    memset(&interrupt, 0, sizeof(interrupt));
    CHECK("open succeeds",
          fujinet_disk_native_test_open(&open_req.iotd_Req.io, 0) != NULL);
    CHECK("open OpenCnt 1", fujinet_disk_native_test_open_cnt() == 1);

    change_req.iotd_Req.io.io_Command = TD_ADDCHANGEINT;
    change_req.iotd_Req.io_Data = &interrupt;
    fujinet_disk_native_test_begin_io(0, &change_req.iotd_Req.io);
    CHECK("change-int registered before deferred expunge",
          fujinet_disk_native_test_change_int_count(0) == 1);

    CHECK("busy expunge returns 0", fujinet_disk_native_test_expunge() == 0);
    CHECK("busy expunge sets DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("busy expunge does not close", transport_close_count == 0);
    CHECK("live change-int kept until complete",
          fujinet_disk_native_test_change_int_count(0) == 1);

    CHECK("last close returns sentinel",
          fujinet_disk_native_test_close(&open_req.iotd_Req.io) ==
              NATIVE_SEGLIST);
    CHECK("last close OpenCnt 0", fujinet_disk_native_test_open_cnt() == 0);
    CHECK("last close completes teardown once", transport_close_count == 1);
    CHECK("complete discards change-ints",
          fujinet_disk_native_test_change_int_count(0) == 0);
    CHECK("last close clears DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) == 0);
}

static void test_expunge_in_progress_completes_on_idle(void)
{
    struct IOExtTD request;

    reset_lifecycle();
    fujinet_disk_native_test_set_client_initialized(0, 1);
    issue_mount(0, &request, 1);
    CHECK("queued work OpenCnt 0", fujinet_disk_native_test_open_cnt() == 0);
    CHECK("in-progress expunge returns 0",
          fujinet_disk_native_test_expunge() == 0);
    CHECK("in-progress expunge sets DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("in-progress expunge does not close", transport_close_count == 0);
    CHECK("in-progress keeps client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 1);

    fujinet_disk_native_test_drain();
    CHECK("queued mount completed before teardown close",
          request.iotd_Req.io.io_Error == TDERR_DiskChanged);
    CHECK("worker idle does not close transport", transport_close_count == 0);
    CHECK("worker idle leaves DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("worker idle keeps client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 1);

    CHECK("follow-up expunge returns sentinel",
          fujinet_disk_native_test_expunge() == NATIVE_SEGLIST);
    CHECK("follow-up expunge closes once", transport_close_count == 1);
    CHECK("follow-up expunge clears DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) == 0);
    CHECK("follow-up expunge clears client_initialized",
          fujinet_disk_native_test_client_initialized(0) == 0);

    fujinet_disk_native_test_drain();
    CHECK("second idle does not close again", transport_close_count == 1);
}

static void test_last_close_while_queued_defers_to_idle(void)
{
    struct IOExtTD open_req;
    struct IOExtTD request;

    reset_lifecycle();
    memset(&open_req, 0, sizeof(open_req));
    CHECK("open before queued expunge",
          fujinet_disk_native_test_open(&open_req.iotd_Req.io, 0) != NULL);
    issue_mount(0, &request, 1);
    CHECK("expunge while open and queued",
          fujinet_disk_native_test_expunge() == 0);
    CHECK("queued expunge does not close", transport_close_count == 0);
    CHECK("last close while queued does not close",
          fujinet_disk_native_test_close(&open_req.iotd_Req.io) == 0);
    CHECK("last close while queued OpenCnt 0",
          fujinet_disk_native_test_open_cnt() == 0);
    CHECK("DELEXP remains until idle",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("still no close after last close", transport_close_count == 0);

    fujinet_disk_native_test_drain();
    CHECK("worker idle after last-close-busy does not close",
          transport_close_count == 0);
    CHECK("DELEXP remains after worker idle",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);
    CHECK("follow-up expunge after last-close-busy returns sentinel",
          fujinet_disk_native_test_expunge() == NATIVE_SEGLIST);
    CHECK("follow-up expunge after last-close-busy closes once",
          transport_close_count == 1);
    CHECK("follow-up expunge after last-close-busy clears DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) == 0);
}

static void test_last_close_without_delexp_does_not_close(void)
{
    struct IOExtTD open_req;

    reset_lifecycle();
    memset(&open_req, 0, sizeof(open_req));
    CHECK("open for last-close",
          fujinet_disk_native_test_open(&open_req.iotd_Req.io, 0) != NULL);
    CHECK("last close without DELEXP",
          fujinet_disk_native_test_close(&open_req.iotd_Req.io) == 0);
    CHECK("OpenCnt 0 without DELEXP does not close",
          transport_close_count == 0);
}

static void test_open_cancels_deferred_expunge(void)
{
    struct IOExtTD first;
    struct IOExtTD second;
    struct IOExtTD request;

    reset_lifecycle();
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK("open before deferred expunge",
          fujinet_disk_native_test_open(&first.iotd_Req.io, 0) != NULL);
    CHECK("expunge while open defers",
          fujinet_disk_native_test_expunge() == 0);
    CHECK("deferred DELEXP is set",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) != 0);

    CHECK("re-open cancels DELEXP",
          fujinet_disk_native_test_open(&second.iotd_Req.io, 0) != NULL);
    CHECK("open cleared DELEXP",
          (fujinet_disk_native_test_lib_flags() & LIBF_DELEXP) == 0);
    fujinet_disk_native_test_set_client_initialized(0, 1);
    CHECK("close after cancel",
          fujinet_disk_native_test_close(&second.iotd_Req.io) == 0);
    CHECK("close first after cancel",
          fujinet_disk_native_test_close(&first.iotd_Req.io) == 0);
    issue_mount(0, &request, 0);
    CHECK("ordinary idle after cancel does not close",
          transport_close_count == 0);
    CHECK("client_initialized survives cancelled teardown",
          fujinet_disk_native_test_client_initialized(0) == 1);
}

int main(void)
{
    test_dd_hd_geometry_and_unit_isolation();
    test_public_mount_abi_reaches_resident_dispatcher();
    test_legacy_td_remove_is_synchronous();
    test_ordinary_idle_does_not_close();
    test_safe_idle_expunge_closes_once();
    test_expunge_deferred_until_last_close();
    test_expunge_in_progress_completes_on_idle();
    test_last_close_while_queued_defers_to_idle();
    test_last_close_without_delexp_does_not_close();
    test_open_cancels_deferred_expunge();
    if (failures != 0) return 1;
    puts("All Amiga resident device contract tests passed");
    return 0;
}
