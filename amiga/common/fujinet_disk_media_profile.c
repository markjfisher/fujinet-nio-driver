#include "fujinet_disk_driver.h"

#include <string.h>

static void fill_profile(fujinet_disk_media_profile_t *profile,
                         fujinet_disk_media_profile_kind_t kind,
                         uint32_t blocks_per_track)
{
    memset(profile, 0, sizeof(*profile));
    profile->kind = kind;
    profile->block_size = FUJINET_DISK_BLOCK_SIZE;
    profile->surfaces = 2;
    profile->blocks_per_track = blocks_per_track;
    profile->low_cylinder = 0;
    profile->high_cylinder = 79;
    profile->reserved_blocks = 2;
    profile->interleave = 0;
    profile->dos_type = 0x444F5300UL;
}

uint8_t fujinet_disk_classify_media_profile(
    const fn_disk_info_t *info, fujinet_disk_media_profile_t *profile)
{
    if (info == NULL || profile == NULL ||
        (info->flags & FN_DISK_FLAG_MOUNTED) == 0 ||
        info->type != FN_DISK_TYPE_RAW ||
        info->sector_size != FUJINET_DISK_BLOCK_SIZE) {
        return FN_ERR_INVALID;
    }

    if (info->sector_count == FUJINET_DD_ADF_BLOCK_COUNT) {
        fill_profile(profile, FUJINET_DISK_MEDIA_PROFILE_DD_ADF, 11);
        return FN_OK;
    }
    if (info->sector_count == FUJINET_HD_ADF_BLOCK_COUNT) {
        fill_profile(profile, FUJINET_DISK_MEDIA_PROFILE_HD_ADF, 22);
        return FN_OK;
    }

    return FN_ERR_INVALID;
}
