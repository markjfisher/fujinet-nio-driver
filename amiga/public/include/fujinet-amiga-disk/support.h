#ifndef FUJINET_AMIGA_DISK_SUPPORT_H
#define FUJINET_AMIGA_DISK_SUPPORT_H

#include <stdint.h>
#include <stddef.h>
#include "fujinet-nio.h"

typedef enum fujinet_disk_media_profile_kind {
    FUJINET_DISK_MEDIA_PROFILE_DD_ADF = 1,
    FUJINET_DISK_MEDIA_PROFILE_HD_ADF = 2
} fujinet_disk_media_profile_kind_t;

typedef struct fujinet_disk_media_profile {
    fujinet_disk_media_profile_kind_t kind;
    uint32_t block_size, surfaces, blocks_per_track, low_cylinder;
    uint32_t high_cylinder, reserved_blocks, interleave;
} fujinet_disk_media_profile_t;

typedef struct fujinet_disk_dos_envec {
    uint32_t de_TableSize, de_SizeBlock, de_SecOrg, de_Surfaces;
    uint32_t de_SectorPerBlock, de_BlocksPerTrack, de_Reserved, de_PreAlloc;
    uint32_t de_Interleave, de_LowCyl, de_HighCyl, de_NumBuffers;
    uint32_t de_BufMemType, de_MaxTransfer, de_Mask, de_BootPri;
    uint32_t de_DosType, de_Baud, de_Control, de_BootBlocks;
    int32_t handler_stack_size, handler_priority, handler_glob_vec;
} fujinet_disk_dos_envec_t;

#define FUJINET_AMIGA_DOS_OFS 0x444f5300UL
#define FUJINET_AMIGA_DOS_FFS 0x444f5301UL

uint8_t fujinet_disk_classify_media_profile(const fn_disk_info_t *info,
                                             fujinet_disk_media_profile_t *profile);
uint8_t fujinet_disk_classify_filesystem(const uint8_t *boot_block,
                                         size_t boot_block_length,
                                         uint32_t *dos_type);
uint8_t fujinet_disk_build_dos_envec(const fujinet_disk_media_profile_t *profile,
                                     uint32_t dos_type,
                                     fujinet_disk_dos_envec_t *envec);
void fujinet_disk_serialize_dos_envec(const fujinet_disk_dos_envec_t *envec,
                                      uint32_t classic_envec[20]);

#endif
