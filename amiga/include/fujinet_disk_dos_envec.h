#ifndef FUJINET_DISK_DOS_ENVEC_H
#define FUJINET_DISK_DOS_ENVEC_H

#include <stdint.h>

#include "fujinet_disk_driver.h"
#include "fujinet_disk_filesystem.h"

/* Host-testable representation of the DosEnvec fields needed by the future
 * MakeDosNode path. de_SizeBlock is measured in longwords, not bytes. */
typedef struct fujinet_disk_dos_envec {
    uint32_t de_SizeBlock;
    uint32_t de_Surfaces;
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
    uint32_t de_DosType;
    int32_t handler_stack_size;
    int32_t handler_priority;
    int32_t handler_glob_vec;
} fujinet_disk_dos_envec_t;

/* Builds only the configuration representation; it does not create a DOS
 * node or start a filesystem handler. Zero means the source MountList omitted
 * that field and this builder supplies no invented value. */
uint8_t fujinet_disk_build_dos_envec(
    const fujinet_disk_media_profile_t *profile,
    uint32_t dos_type,
    fujinet_disk_dos_envec_t *envec);

#endif
