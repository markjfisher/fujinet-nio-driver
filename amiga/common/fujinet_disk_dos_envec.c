#include <fujinet-amiga-disk/support.h>

#include <string.h>

uint8_t fujinet_disk_build_dos_envec(
    const fujinet_disk_media_profile_t *profile,
    uint32_t dos_type,
    fujinet_disk_dos_envec_t *envec)
{
    if (profile == NULL || envec == NULL ||
        (profile->kind != FUJINET_DISK_MEDIA_PROFILE_DD_ADF &&
         profile->kind != FUJINET_DISK_MEDIA_PROFILE_HD_ADF) ||
        profile->block_size != 512 ||
        (dos_type != FUJINET_AMIGA_DOS_OFS &&
         dos_type != FUJINET_AMIGA_DOS_FFS)) {
        return FN_ERR_INVALID;
    }

    memset(envec, 0, sizeof(*envec));
    /* Classic DosEnvec contains entries 0..19; de_TableSize is the
     * highest valid index, hence 19 rather than the entry count 20. */
    envec->de_TableSize = 19;
    envec->de_SizeBlock = profile->block_size / 4;
    envec->de_SecOrg = 0;
    envec->de_Surfaces = profile->surfaces;
    envec->de_SectorPerBlock = 1;
    envec->de_BlocksPerTrack = profile->blocks_per_track;
    envec->de_LowCyl = profile->low_cylinder;
    envec->de_HighCyl = profile->high_cylinder;
    envec->de_Reserved = profile->reserved_blocks;
    envec->de_PreAlloc = 0;
    envec->de_Interleave = profile->interleave;
    envec->de_NumBuffers = 5;
    envec->de_BufMemType = 1;
    envec->de_MaxTransfer = 0x7FFFFFFFUL;
    envec->de_Mask = 0xFFFFFFFEUL;
    envec->de_BootPri = 0;
    envec->de_DosType = dos_type;
    envec->de_Baud = 1200;
    envec->de_Control = 0;
    envec->de_BootBlocks = 0;
    envec->handler_stack_size = 32768;
    envec->handler_priority = 5;
    envec->handler_glob_vec = -1;
    return FN_OK;
}

void fujinet_disk_serialize_dos_envec(const fujinet_disk_dos_envec_t *envec,
                                      uint32_t classic_envec[20])
{
    if (envec == NULL || classic_envec == NULL) return;
    classic_envec[0] = envec->de_TableSize;
    classic_envec[1] = envec->de_SizeBlock;
    classic_envec[2] = envec->de_SecOrg;
    classic_envec[3] = envec->de_Surfaces;
    classic_envec[4] = envec->de_SectorPerBlock;
    classic_envec[5] = envec->de_BlocksPerTrack;
    classic_envec[6] = envec->de_Reserved;
    classic_envec[7] = envec->de_PreAlloc;
    classic_envec[8] = envec->de_Interleave;
    classic_envec[9] = envec->de_LowCyl;
    classic_envec[10] = envec->de_HighCyl;
    classic_envec[11] = envec->de_NumBuffers;
    classic_envec[12] = envec->de_BufMemType;
    classic_envec[13] = envec->de_MaxTransfer;
    classic_envec[14] = envec->de_Mask;
    classic_envec[15] = (uint32_t)envec->de_BootPri;
    classic_envec[16] = envec->de_DosType;
    classic_envec[17] = envec->de_Baud;
    classic_envec[18] = envec->de_Control;
    classic_envec[19] = envec->de_BootBlocks;
}
