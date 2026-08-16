#include <devices/trackdisk.h>
#include <fujinet-amiga-disk/device.h>

#include "fujinet_disk_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fujinet_disk_native_test_reset(void);
void fujinet_disk_native_test_commit(uint8_t unit, uint32_t sector_count);
void fujinet_disk_native_test_begin_io(uint8_t unit, struct IORequest *request);

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

void Disable(void) {}
void Enable(void) {}
void ReplyMsg(struct Message *message) { (void)message; }
void Cause(struct Interrupt *interrupt) { (void)interrupt; }
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
void fn_transport_close(void) {}
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

int main(void)
{
    test_dd_hd_geometry_and_unit_isolation();
    test_public_mount_abi_reaches_resident_dispatcher();
    if (failures != 0) return 1;
    puts("All Amiga resident device contract tests passed");
    return 0;
}
