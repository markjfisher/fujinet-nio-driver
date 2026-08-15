#include "fujinet_disk_driver.h"
#include "fujinet_disk_dos_envec.h"
#include "fujinet_disk_filesystem.h"

#include <stdio.h>
#include <string.h>

typedef struct fake_client {
    unsigned init_calls;
    unsigned mount_calls;
    unsigned info_calls;
    unsigned read_calls;
    unsigned write_calls;
    unsigned flush_calls;
    unsigned unmount_calls;
    unsigned clear_calls;
    uint8_t slot;
    uint8_t readonly;
    uint8_t type;
    uint16_t sector_size_hint;
    uint32_t first_lba;
    const char *uri;
    uint8_t info_result;
    uint8_t mount_result;
    uint8_t read_result;
    uint8_t write_result;
    uint8_t flush_result;
    uint8_t unmount_result;
    uint8_t clear_result;
    uint8_t invalid_mount;
    unsigned fail_write_call;
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
    if (fake->mount_result != FN_OK) return fake->mount_result;
    memset(info, 0, sizeof(*info));
    info->slot = slot;
    info->flags = FN_DISK_FLAG_MOUNTED |
                  (readonly ? FN_DISK_FLAG_READONLY : 0);
    info->type = FN_DISK_TYPE_RAW;
    info->sector_size = FUJINET_DISK_BLOCK_SIZE;
    info->sector_count = fake->media.sector_count != 0 ?
        fake->media.sector_count : FUJINET_DD_ADF_BLOCK_COUNT;
    if (fake->invalid_mount) info->sector_count = FUJINET_DD_ADF_BLOCK_COUNT - 1;
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

static uint8_t fake_write(void *context, uint8_t slot, uint32_t lba,
                          const uint8_t *data, uint16_t length)
{
    fake_client_t *fake = context;
    (void)lba; (void)data; (void)length;
    fake->slot = slot;
    ++fake->write_calls;
    if (fake->fail_write_call == fake->write_calls) return FN_ERR_IO;
    return fake->write_result;
}
static uint8_t fake_flush(void *context, uint8_t slot)
{ fake_client_t *fake = context; fake->slot = slot; ++fake->flush_calls; return fake->flush_result; }
static uint8_t fake_unmount(void *context, uint8_t slot)
{ fake_client_t *fake = context; fake->slot = slot; ++fake->unmount_calls; return fake->unmount_result; }
static uint8_t fake_clear(void *context, uint8_t slot)
{ fake_client_t *fake = context; fake->slot = slot; ++fake->clear_calls; return fake->clear_result; }

static const fujinet_disk_client_t fake_ops = {
    fake_init,
    fake_mount,
    fake_info,
    fake_read,
    fake_write,
    fake_flush,
    fake_unmount,
    fake_clear
};

static void test_amiga_units_map_to_one_based_diskdevice_slots(void)
{
    uint8_t slot = 0;

    CHECK("unit zero is accepted",
          fujinet_disk_unit_to_slot(0, &slot) == FN_OK);
    CHECK("unit zero maps to slot one", slot == 1);
    CHECK("unit one is accepted",
          fujinet_disk_unit_to_slot(1, &slot) == FN_OK);
    CHECK("unit one maps to slot two", slot == 2);
    CHECK("unit seven maps to slot eight",
          fujinet_disk_unit_to_slot(7, &slot) == FN_OK && slot == 8);
    CHECK("unit eight is rejected",
          fujinet_disk_unit_to_slot(8, &slot) == FN_ERR_NOT_FOUND);
}

static void test_mount_is_explicitly_read_only_auto_detected_and_512_bytes(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
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

static void test_adf_info_accepts_dd_and_hd_rejects_others(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("valid DD ADF mounts",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    fake.media.sector_count = 1759;
    CHECK("non-standard block count is rejected",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_ERR_INVALID);
    fake.media.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    fake.media.sector_size = 256;
    CHECK("non-512-byte geometry is rejected",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_ERR_INVALID);
    fake.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    fake.media.flags &= (uint8_t)~FN_DISK_FLAG_READONLY;
    CHECK("read-only mount tolerates effective writable media",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_OK);

    /* HD ADF: 3520 sectors is also accepted */
    fake.media.sector_count = FUJINET_HD_ADF_BLOCK_COUNT;
    fake.media.flags |= FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    CHECK("HD ADF geometry (3520 sectors) is accepted",
          fujinet_disk_info(&driver, 0, &driver.media) == FN_OK);
}

static void test_hd_adf_mount_and_bounds(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;
    uint8_t data[512];
    uint32_t actual = 0;

    fake.media.sector_count = FUJINET_HD_ADF_BLOCK_COUNT;
    fake.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    fake.media.flags = FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    fake.media.type = FN_DISK_TYPE_RAW;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("HD ADF mounts successfully",
          fujinet_disk_mount(&driver, 0, "hd.adf") == FN_OK);
    CHECK("mounted media records HD sector count",
          driver.media.sector_count == FUJINET_HD_ADF_BLOCK_COUNT);

    fake.read_length = 512;
    CHECK("read within HD ADF succeeds",
          fujinet_disk_read(&driver, 0, 0, data, sizeof(data), &actual) == FN_OK);

    CHECK("read at last HD sector succeeds",
          fujinet_disk_read(&driver, 0,
                            (FUJINET_HD_ADF_BLOCK_COUNT - 1) * FUJINET_DISK_BLOCK_SIZE,
                            data, sizeof(data), &actual) == FN_OK);

    CHECK("read beyond HD ADF end is rejected",
          fujinet_disk_read(&driver, 0,
                            FUJINET_HD_ADF_BLOCK_COUNT * FUJINET_DISK_BLOCK_SIZE,
                            data, sizeof(data), &actual) == FN_ERR_INVALID);

    /* DD-sized read/write rejected on an HD unit */
    CHECK("read at DD boundary on HD unit passes (HD is larger)",
          fujinet_disk_read(&driver, 0,
                            FUJINET_DD_ADF_BLOCK_COUNT * FUJINET_DISK_BLOCK_SIZE,
                            data, sizeof(data), &actual) == FN_OK);
}

static void test_geometry_implies_160_tracks_for_mounted_media(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fake.media.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    fake.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    fake.media.flags = FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    fake.media.type = FN_DISK_TYPE_RAW;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("DD ADF mounts successfully for geometry query",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    CHECK("mounted DD ADF keeps 80 cylinders",
          driver.media.sector_count == FUJINET_DD_ADF_BLOCK_COUNT);
    CHECK("Amiga track count must be 160 for a two-head 80-cylinder disk",
          (80U * 2U) == 160U);
}

static void test_dd_and_hd_profiles_keep_expected_geometry(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t dd;
    fujinet_disk_driver_t hd;

    fake.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    fake.media.flags = FN_DISK_FLAG_MOUNTED | FN_DISK_FLAG_READONLY;
    fake.media.type = FN_DISK_TYPE_RAW;

    fujinet_disk_driver_init(&dd, &fake_ops, &fake, 0);
    fake.media.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    CHECK("DD profile mounts with 1760 sectors",
          fujinet_disk_mount(&dd, 0, "dd.adf") == FN_OK &&
              dd.media.sector_count == FUJINET_DD_ADF_BLOCK_COUNT);

    fujinet_disk_driver_init(&hd, &fake_ops, &fake, 0);
    fake.media.sector_count = FUJINET_HD_ADF_BLOCK_COUNT;
    CHECK("HD profile mounts with 3520 sectors",
          fujinet_disk_mount(&hd, 0, "hd.adf") == FN_OK &&
              hd.media.sector_count == FUJINET_HD_ADF_BLOCK_COUNT);
}

static void test_media_profiles_are_explicit_and_reject_ambiguous_geometry(void)
{
    fn_disk_info_t info = {0};
    fujinet_disk_media_profile_t profile;

    info.flags = FN_DISK_FLAG_MOUNTED;
    info.type = FN_DISK_TYPE_RAW;
    info.sector_size = FUJINET_DISK_BLOCK_SIZE;

    info.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    CHECK("DD media selects DD profile",
          fujinet_disk_classify_media_profile(&info, &profile) == FN_OK &&
              profile.kind == FUJINET_DISK_MEDIA_PROFILE_DD_ADF);
    CHECK("DD profile has exact geometry",
          profile.block_size == 512 && profile.surfaces == 2 &&
              profile.blocks_per_track == 11 && profile.low_cylinder == 0 &&
              profile.high_cylinder == 79 && profile.reserved_blocks == 2 &&
              profile.interleave == 0);

    info.sector_count = FUJINET_HD_ADF_BLOCK_COUNT;
    CHECK("HD media selects HD profile",
          fujinet_disk_classify_media_profile(&info, &profile) == FN_OK &&
              profile.kind == FUJINET_DISK_MEDIA_PROFILE_HD_ADF);
    CHECK("HD profile has distinct blocks per track",
          profile.block_size == 512 && profile.surfaces == 2 &&
              profile.blocks_per_track == 22 && profile.low_cylinder == 0 &&
              profile.high_cylinder == 79 && profile.reserved_blocks == 2 &&
              profile.interleave == 0);

    /* The profile API has no DosType field: geometry classification alone
     * cannot claim OFS or FFS. */
    CHECK("geometry profile remains geometry-only",
          profile.kind == FUJINET_DISK_MEDIA_PROFILE_HD_ADF &&
              profile.block_size == 512 && profile.blocks_per_track == 22);

    info.sector_count = 1680;
    CHECK("ambiguous 1680-sector media is rejected",
          fujinet_disk_classify_media_profile(&info, &profile) == FN_ERR_INVALID);
    info.sector_count = 1000000;
    CHECK("large raw media is rejected",
          fujinet_disk_classify_media_profile(&info, &profile) == FN_ERR_INVALID);
    info.sector_size = 256;
    info.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    CHECK("non-512 media is rejected",
          fujinet_disk_classify_media_profile(&info, &profile) == FN_ERR_INVALID);
}

static void test_media_profiles_are_independent_per_unit(void)
{
    fujinet_disk_driver_t dd = {0};
    fujinet_disk_driver_t hd = {0};
    fujinet_disk_media_profile_t dd_profile;
    fujinet_disk_media_profile_t hd_profile;

    dd.media.flags = FN_DISK_FLAG_MOUNTED;
    dd.media.type = FN_DISK_TYPE_RAW;
    dd.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    dd.media.sector_count = FUJINET_DD_ADF_BLOCK_COUNT;
    hd.media.flags = FN_DISK_FLAG_MOUNTED;
    hd.media.type = FN_DISK_TYPE_RAW;
    hd.media.sector_size = FUJINET_DISK_BLOCK_SIZE;
    hd.media.sector_count = FUJINET_HD_ADF_BLOCK_COUNT;

    CHECK("unit zero selects DD independently",
          fujinet_disk_classify_media_profile(&dd.media, &dd_profile) == FN_OK &&
              dd_profile.blocks_per_track == 11);
    CHECK("unit one selects HD independently",
          fujinet_disk_classify_media_profile(&hd.media, &hd_profile) == FN_OK &&
              hd_profile.blocks_per_track == 22);
    CHECK("unit profiles remain distinct",
          dd_profile.kind != hd_profile.kind &&
              dd_profile.blocks_per_track != hd_profile.blocks_per_track);
}

static void test_filesystem_classifier_accepts_supported_boot_types(void)
{
    static const uint8_t dd_boot[] = {'D', 'O', 'S', 0};
    static const uint8_t hd_boot[] = {'D', 'O', 'S', 0};
    static const uint8_t ffs_boot[] = {'D', 'O', 'S', 1};
    uint32_t dos_type = 0;

    CHECK("standard DD boot block identifies OFS",
          fujinet_disk_classify_filesystem(dd_boot, sizeof(dd_boot), &dos_type) == FN_OK &&
              dos_type == FUJINET_AMIGA_DOS_OFS);
    CHECK("standard HD boot block identifies OFS",
          fujinet_disk_classify_filesystem(hd_boot, sizeof(hd_boot), &dos_type) == FN_OK &&
              dos_type == FUJINET_AMIGA_DOS_OFS);
    CHECK("FFS boot block identifies FFS",
          fujinet_disk_classify_filesystem(ffs_boot, sizeof(ffs_boot), &dos_type) == FN_OK &&
              dos_type == FUJINET_AMIGA_DOS_FFS);
}

static void test_filesystem_classifier_rejects_invalid_boot_types(void)
{
    static const uint8_t bad_signature[] = {'N', 'O', 'S', 0};
    static const uint8_t unsupported_type[] = {'D', 'O', 'S', 2};
    static const uint8_t truncated[] = {'D', 'O', 'S'};
    uint32_t dos_type = 0;

    CHECK("invalid boot signature is rejected",
          fujinet_disk_classify_filesystem(bad_signature, sizeof(bad_signature),
                                            &dos_type) == FN_ERR_INVALID);
    CHECK("unsupported DosType is rejected",
          fujinet_disk_classify_filesystem(unsupported_type,
                                            sizeof(unsupported_type),
                                            &dos_type) == FN_ERR_INVALID);
    CHECK("truncated boot block is rejected",
          fujinet_disk_classify_filesystem(truncated, sizeof(truncated),
                                            &dos_type) == FN_ERR_INVALID);
}

static void test_dos_envec_builder_maps_dd_hd_and_filesystem_type(void)
{
    fujinet_disk_media_profile_t dd = {FUJINET_DISK_MEDIA_PROFILE_DD_ADF,
                                       512, 2, 11, 0, 79, 2, 0};
    fujinet_disk_media_profile_t hd = {FUJINET_DISK_MEDIA_PROFILE_HD_ADF,
                                       512, 2, 22, 0, 79, 2, 0};
    fujinet_disk_dos_envec_t dd_env;
    fujinet_disk_dos_envec_t hd_env;
    fujinet_disk_dos_envec_t dd_ffs_env;
    fujinet_disk_dos_envec_t hd_ofs_env;

    CHECK("DD OFS DosEnvec builds",
          fujinet_disk_build_dos_envec(&dd, FUJINET_AMIGA_DOS_OFS,
                                       &dd_env) == FN_OK);
    CHECK("HD FFS DosEnvec builds",
          fujinet_disk_build_dos_envec(&hd, FUJINET_AMIGA_DOS_FFS,
                                       &hd_env) == FN_OK);
    CHECK("DD FFS DosEnvec builds",
          fujinet_disk_build_dos_envec(&dd, FUJINET_AMIGA_DOS_FFS,
                                       &dd_ffs_env) == FN_OK);
    CHECK("HD OFS DosEnvec builds",
          fujinet_disk_build_dos_envec(&hd, FUJINET_AMIGA_DOS_OFS,
                                       &hd_ofs_env) == FN_OK);
    CHECK("DD fields map exactly",
          dd_env.de_TableSize == 19 && dd_env.de_SizeBlock == 128 &&
              dd_env.de_SecOrg == 0 && dd_env.de_Surfaces == 2 &&
              dd_env.de_SectorPerBlock == 1 &&
              dd_env.de_BlocksPerTrack == 11 && dd_env.de_LowCyl == 0 &&
              dd_env.de_HighCyl == 79 && dd_env.de_Reserved == 2 &&
              dd_env.de_Interleave == 0 &&
              dd_env.de_DosType == FUJINET_AMIGA_DOS_OFS);
    CHECK("HD fields map exactly",
          hd_env.de_TableSize == 19 && hd_env.de_SizeBlock == 128 &&
              hd_env.de_SecOrg == 0 && hd_env.de_Surfaces == 2 &&
              hd_env.de_SectorPerBlock == 1 &&
              hd_env.de_BlocksPerTrack == 22 && hd_env.de_LowCyl == 0 &&
              hd_env.de_HighCyl == 79 && hd_env.de_Reserved == 2 &&
              hd_env.de_Interleave == 0 &&
              hd_env.de_DosType == FUJINET_AMIGA_DOS_FFS);
    CHECK("operational fields preserve MountList values",
          dd_env.de_NumBuffers == 5 && dd_env.de_BufMemType == 1 &&
              dd_env.de_PreAlloc == 0 &&
              dd_env.de_MaxTransfer == 0x7FFFFFFFUL &&
              dd_env.de_Mask == 0xFFFFFFFEUL && dd_env.de_BootPri == 0 &&
              dd_env.de_Baud == 1200 && dd_env.de_Control == 0 &&
              dd_env.de_BootBlocks == 0 &&
              dd_env.handler_stack_size == 32768 &&
              dd_env.handler_priority == 5 && dd_env.handler_glob_vec == -1);
    CHECK("DD and HD differ only by geometry and DosType",
          dd_env.de_SizeBlock == hd_env.de_SizeBlock &&
              dd_env.de_Surfaces == hd_env.de_Surfaces &&
              dd_env.de_LowCyl == hd_env.de_LowCyl &&
              dd_env.de_HighCyl == hd_env.de_HighCyl &&
              dd_env.de_Reserved == hd_env.de_Reserved &&
              dd_env.de_Interleave == hd_env.de_Interleave &&
              dd_env.de_BlocksPerTrack != hd_env.de_BlocksPerTrack &&
              dd_env.de_DosType != hd_env.de_DosType);
}

static void test_dos_envec_builder_rejects_unsupported_dos_type(void)
{
    fujinet_disk_media_profile_t profile = {FUJINET_DISK_MEDIA_PROFILE_DD_ADF,
                                            512, 2, 11, 0, 79, 2, 0};
    fujinet_disk_dos_envec_t envec;

    CHECK("unsupported DosType cannot enter DosEnvec builder",
          fujinet_disk_build_dos_envec(&profile, 0x444F5302UL, &envec) ==
              FN_ERR_INVALID);
}

static void test_info_and_media_read_failures_are_reported(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;
    uint8_t data[512];
    uint32_t actual = 99;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
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
    CHECK("read beyond DD ADF end is rejected",
          fujinet_disk_read(&driver, 0,
                            FUJINET_DD_ADF_BLOCK_COUNT * FUJINET_DISK_BLOCK_SIZE,
                            data, sizeof(data), &actual) == FN_ERR_INVALID);
}

static void test_repeated_mounts_share_one_initialized_session(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
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

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
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

static void test_units_keep_independent_media_and_change_state(void)
{
    fake_client_t first = {0};
    fake_client_t second = {0};
    fujinet_disk_driver_t unit0;
    fujinet_disk_driver_t unit1;

    fujinet_disk_driver_init(&unit0, &fake_ops, &first, 0);
    fujinet_disk_driver_init(&unit1, &fake_ops, &second, 1);
    CHECK("unit zero mounts slot one",
          fujinet_disk_mount(&unit0, 0, "zero.adf") == FN_OK &&
              first.slot == 1);
    CHECK("unit one mounts slot two writable",
          fujinet_disk_mount_mode(&unit1, 1, "one.adf", 1) == FN_OK &&
              second.slot == 2 && second.readonly == 0);
    CHECK("each insertion has its own change count",
          unit0.change_count == 1 && unit1.change_count == 1);
    CHECK("ejecting unit one leaves unit zero mounted",
          fujinet_disk_eject(&unit1, 1) == FN_OK && unit0.mounted &&
              !unit1.mounted && unit1.change_count == 2);
}

static void test_writes_preserve_partial_progress_and_flush_errors(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;
    uint8_t data[1024] = {0};
    uint32_t actual = 0;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("writable mount succeeds",
          fujinet_disk_mount_mode(&driver, 0, "work.adf", 1) == FN_OK);
    CHECK("writable mount probes flush", fake.flush_calls == 1);
    fake.fail_write_call = 2;
    CHECK("second sector failure is reported",
          fujinet_disk_write(&driver, 0, 0, data, sizeof(data), &actual) ==
              FN_ERR_IO);
    CHECK("partial write preserves first sector progress", actual == 512);
    fake.flush_result = FN_ERR_IO;
    CHECK("flush failure is preserved",
          fujinet_disk_flush(&driver, 0) == FN_ERR_IO && driver.mounted);
}

static void test_failed_replacement_and_eject_keep_old_media(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("initial writable mount succeeds",
          fujinet_disk_mount_mode(&driver, 0, "old.adf", 1) == FN_OK);
    fake.mount_result = FN_ERR_IO;
    CHECK("failed remote replacement is reported",
          fujinet_disk_mount_mode(&driver, 0, "new.adf", 1) == FN_ERR_IO);
    CHECK("failed replacement keeps old local media",
          driver.mounted && driver.change_count == 1);
    CHECK("eject fails when remote unmount fails",
          (fake.mount_result = FN_OK, fake.unmount_result = FN_ERR_IO,
           fujinet_disk_eject(&driver, 0) == FN_ERR_IO));
    CHECK("failed eject keeps media inserted", driver.mounted);
}

static void test_invalid_replacement_clears_committed_remote_media(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("initial mount succeeds",
          fujinet_disk_mount(&driver, 0, "old.adf") == FN_OK);
    fake.invalid_mount = 1;
    CHECK("invalid replacement is rejected",
          fujinet_disk_mount(&driver, 0, "bad.adf") == FN_ERR_INVALID);
    CHECK("invalid replacement leaves no stale local media",
          !driver.mounted && driver.media.sector_count == 0 &&
              fake.unmount_calls == 1);
}

static void test_eject_acknowledges_media_change_and_retries(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    CHECK("mount succeeds",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK);
    fake.clear_result = FN_ERR_IO;
    CHECK("eject succeeds despite acknowledgement failure",
          fujinet_disk_eject(&driver, 0) == FN_OK &&
              driver.change_ack_pending);
    fake.clear_result = FN_OK;
    CHECK("eject acknowledgement retries",
          fujinet_disk_acknowledge_change(&driver, 0) == FN_OK &&
              !driver.change_ack_pending && fake.clear_calls == 3);
}

static void test_change_acknowledgement_is_retried(void)
{
    fake_client_t fake = {0};
    fujinet_disk_driver_t driver;

    fujinet_disk_driver_init(&driver, &fake_ops, &fake, 0);
    fake.clear_result = FN_ERR_IO;
    CHECK("mount commits despite acknowledgement failure",
          fujinet_disk_mount(&driver, 0, "disk.adf") == FN_OK &&
              driver.change_ack_pending);
    fake.clear_result = FN_OK;
    CHECK("pending acknowledgement retries successfully",
          fujinet_disk_acknowledge_change(&driver, 0) == FN_OK &&
              !driver.change_ack_pending && fake.clear_calls == 2);
}

int main(void)
{
    test_amiga_units_map_to_one_based_diskdevice_slots();
    test_mount_is_explicitly_read_only_auto_detected_and_512_bytes();
    test_repeated_mounts_share_one_initialized_session();
    test_adf_info_accepts_dd_and_hd_rejects_others();
    test_hd_adf_mount_and_bounds();
    test_geometry_implies_160_tracks_for_mounted_media();
    test_dd_and_hd_profiles_keep_expected_geometry();
    test_media_profiles_are_explicit_and_reject_ambiguous_geometry();
    test_media_profiles_are_independent_per_unit();
    test_filesystem_classifier_accepts_supported_boot_types();
    test_filesystem_classifier_rejects_invalid_boot_types();
    test_dos_envec_builder_maps_dd_hd_and_filesystem_type();
    test_dos_envec_builder_rejects_unsupported_dos_type();
    test_info_and_media_read_failures_are_reported();
    test_reads_are_512_byte_aligned_sector_requests_on_slot_one();
    test_units_keep_independent_media_and_change_state();
    test_writes_preserve_partial_progress_and_flush_errors();
    test_failed_replacement_and_eject_keep_old_media();
    test_invalid_replacement_clears_committed_remote_media();
    test_eject_acknowledges_media_change_and_retries();
    test_change_acknowledgement_is_retried();

    if (failures != 0) {
        fprintf(stderr, "%u Amiga driver contract test(s) failed\n", failures);
        return 1;
    }
    puts("All Amiga driver contract tests passed");
    return 0;
}
