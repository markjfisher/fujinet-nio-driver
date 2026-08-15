#ifndef FUJINET_DISK_DOS_ENVEC_H
#define FUJINET_DISK_DOS_ENVEC_H

#include <stdint.h>

#include "fujinet_disk_driver.h"
#include "fujinet_disk_filesystem.h"

/* Host-testable representation of the complete classic DosEnvec environment.
 * de_TableSize is the last populated zero-based index, and de_SizeBlock is
 * measured in longwords, not bytes. */
typedef struct fujinet_disk_dos_envec {
    uint32_t de_TableSize;
    uint32_t de_SizeBlock;
    uint32_t de_SecOrg;
    uint32_t de_Surfaces;
    uint32_t de_SectorPerBlock;
    uint32_t de_BlocksPerTrack;
    uint32_t de_LowCyl;
    uint32_t de_HighCyl;
    uint32_t de_Reserved;
    uint32_t de_PreAlloc;
    uint32_t de_Interleave;
    uint32_t de_NumBuffers;
    uint32_t de_BufMemType;
    uint32_t de_MaxTransfer;
    uint32_t de_Mask;
    int32_t de_BootPri;
    uint32_t de_DosType;
    uint32_t de_Baud;
    uint32_t de_Control;
    uint32_t de_BootBlocks;
    int32_t handler_stack_size;
    int32_t handler_priority;
    int32_t handler_glob_vec;
} fujinet_disk_dos_envec_t;

/* Builds only the effective static-MountList representation; it does not
 * create a DOS node or start a filesystem handler. Defaults are explicit so
 * the result can be reused for later dynamic-node construction. */
uint8_t fujinet_disk_build_dos_envec(
    const fujinet_disk_media_profile_t *profile,
    uint32_t dos_type,
    fujinet_disk_dos_envec_t *envec);

#endif
