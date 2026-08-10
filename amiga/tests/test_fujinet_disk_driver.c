#include "fujinet_disk_driver.h"

#include <stdio.h>
#include <string.h>

typedef struct fake_client {
    unsigned init_calls;
    unsigned mount_calls;
    unsigned info_calls;
    unsigned read_calls;
    uint8_t slot;
    uint8_t readonly;
    uint8_t type;
    uint16_t sector_size_hint;
    uint32_t first_lba;
    const char *uri;
    uint8_t info_result;
    uint8_t read_result;
    uint16_t read_length;
    fn_disk_info_t media;
} fake_client_t;

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static uint8_t fake_init(void *context)
{
    fake_client_t *fake = (fake_client_t *)context;
    ++fake->init_calls;
    return FN_OK;
}

static uint8_t fake_mount(void *context, uint8_t slot, const char *uri,
                          uint8_t readonly, uint8_t type,
                          uint16_t sector_size_hint, fn_disk_info_t *info)
{
    fake_client_t *fake = (fake_client_t *)context;
    ++fake->mount_calls;
    fake->slot = slot;
    fake->uri = uri;
    fake->readonly = readonly;
    fake->type = type;
    fake->sector_size_hint = sector_size_hint;
    memset(info, 0, sizeof(*info));
    info->slot = slot;
    info->flags = FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    info->type = FN_DISK_TYPE_RAW;
    info->sector_size = FUJINET_DISK_BLOCK_SIZE;
    info->sector_count = FUJINET_ADF_BLOCK_COUNT;
    fake->media = *info;
    return FN_OK;
}

static uint8_t fake_info(void *context, uint8_t slot, fn_disk_info_t *info)
{
    fake_client_t *fake = (fake_client_t *)context;
    ++fake->info_calls;
    fake->slot = slot;
    if (fake->info_result != FN_OK) {
        return fake->info_result;
    }
    *info = fake->media;
    return FN_OK;
}

static uint8_t fake_read(void *context, uint8_t slot, uint32_t lba,
                         uint8_t *data, uint16_t capacity, uint16_t *length)
{
    fake_client_t *fake = (fake_client_t *)context;
    unsigned i;

    if (fake->read_result != FN_OK) {
        return fake->read_result;
    }
    if (fake->read_calls == 0) {
        fake->first_lba = lba;
    }
    ++fake->read_calls;
    fake->slot = slot;
    for (i = 0; i < capacity; ++i) {
        data[i] = (uint8_t)lba;
    }
    *length = fake->read_length != 0 ? fake->read_length : capacity;
    return FN_OK;
}

static const fujinet_disk_client_t fake_ops = {
    fake_init,
    fake_mount,
    fake_info,
    fake_read
};

static void test_only_amiga_unit_zero_maps_to_diskdevice_slot_one(void)
{
    uint8_t slot = 0;

    CHECK("unit zero is accepted",
          fujinet_disk_unit_to_slot(0, &slot) == FN_OK);
    CHECK("unit zero maps to slot one", slot == 1);
    CHECK("additional units are rejected",
          fujinet_disk_unit_to_slot(1, &slot) == FN_ERR_NOT_FOUND);
}

static void test_mount_is_explicitly_read_only_auto_detected_and_512_bytes(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake);
    CHECK("mount succeeds",
          fujinet_disk_mount(&driver, 0, "tnfs://host/workbench.adf") == FN_OK);
    CHECK("Mount uses DiskDevice slot one", fake.slot == 1);
    CHECK("Mount is read only", fake.readonly == 1);
    CHECK("Mount asks DiskDevice to detect ADF/raw format",
          fake.type == FN_DISK_TYPE_AUTO);
    CHECK("Mount supplies the Amiga 512-byte block hint",
          fake.sector_size_hint == 512);
    CHECK("Mount verifies geometry with an Info request", fake.info_calls == 1);
    CHECK("Mount passes the configured URI",
          strcmp(fake.uri, "tnfs://host/workbench.adf") == 0);
}

static void test_standard_adf_info_requires_read_only_raw_512_by_1760_geometry(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake);
    CHECK("valid standard ADF mounts",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    fake.media.sector_count = 1759;
    CHECK("non-standard block count is rejected",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_ERR_INVALID);
    fake.media.sector_count = FUJINET_ADF_BLOCK_COUNT;
    fake.media.sector_size = 256;
    CHECK("non-512-byte geometry is rejected",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_ERR_INVALID);
    fake.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    fake.media.flags &= (uint8_t)~FN_DISK_FLAG_READONLY;
    CHECK("writable effective media is rejected in read-only stage",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_ERR_INVALID);
}

static void test_info_and_media_read_failures_are_reported(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;
    uint8_t data[512];
    uint32_t actual = 99;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake);
    fake.info_result = FN_ERR_IO;
    CHECK("Info transport failure fails Mount",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_ERR_IO);
    CHECK("failed Info leaves media unmounted", driver.mounted == 0);

    fake.info_result = FN_OK;
    CHECK("retry mounts standard ADF",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    fake.read_result = FN_ERR_TIMEOUT;
    CHECK("media read error is preserved",
          fujinet_disk_read(&driver, 0, 0, data, sizeof(data), &actual) ==
              FN_ERR_TIMEOUT);
    CHECK("failed read reports no transferred bytes", actual == 0);
    fake.read_result = FN_OK;
    fake.read_length = 511;
    CHECK("short sector response is rejected",
          fujinet_disk_read(&driver, 0, 0, data, sizeof(data), &actual) ==
              FN_ERR_IO);
    CHECK("read beyond standard ADF is rejected",
          fujinet_disk_read(&driver, 0, FUJINET_ADF_BYTE_SIZE, data,
                            sizeof(data), &actual) == FN_ERR_INVALID);
}

static void test_repeated_mounts_share_one_initialized_session(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake);
    CHECK("first mount succeeds",
          fujinet_disk_mount(&driver, 0, "one.adf") == FN_OK);
    CHECK("second mount succeeds",
          fujinet_disk_mount(&driver, 0, "two.adf") == FN_OK);
    CHECK("client session initializes once", fake.init_calls == 1);
    CHECK("each mount is sent", fake.mount_calls == 2);
}

static void test_reads_are_512_byte_aligned_sector_requests_on_slot_one(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;
    uint8_t data[1024];
    uint32_t actual = 0;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake);
    CHECK("mount before read succeeds",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    CHECK("two-block read succeeds",
          fujinet_disk_read(&driver, 0, 1024, data, sizeof(data), &actual) == FN_OK);
    CHECK("read uses slot one", fake.slot == 1);
    CHECK("byte offset converts to first LBA", fake.first_lba == 2);
    CHECK("one DiskDevice request is sent per block", fake.read_calls == 2);
    CHECK("all requested bytes are reported", actual == sizeof(data));
    CHECK("unaligned reads are rejected",
          fujinet_disk_read(&driver, 0, 1, data, 512, &actual) == FN_ERR_INVALID);
}

int main(void)
{
    test_only_amiga_unit_zero_maps_to_diskdevice_slot_one();
    test_mount_is_explicitly_read_only_auto_detected_and_512_bytes();
    test_repeated_mounts_share_one_initialized_session();
    test_standard_adf_info_requires_read_only_raw_512_by_1760_geometry();
    test_info_and_media_read_failures_are_reported();
    test_reads_are_512_byte_aligned_sector_requests_on_slot_one();

    if (failures != 0) {
        fprintf(stderr, "%u Amiga driver contract test(s) failed\n", failures);
        return 1;
    }
    puts("All Amiga driver contract tests passed");
    return 0;
}
