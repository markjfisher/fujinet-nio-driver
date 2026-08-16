#ifndef NATIVE_TEST_TRACKDISK_H
#define NATIVE_TEST_TRACKDISK_H

#include <exec/io.h>

struct DriveGeometry { ULONG dg_SectorSize, dg_TotalSectors, dg_CylSectors, dg_Cylinders, dg_Heads, dg_TrackSectors, dg_BufMemType, dg_DeviceType, dg_Flags; UBYTE dg_Reserved; };
#define ETD_READ 10U
#define ETD_WRITE 11U
#define ETD_UPDATE 12U
#define ETD_CLEAR 13U
#define TD_MOTOR 14U
#define TD_SEEK 15U
#define TD_CHANGENUM 16U
#define TD_CHANGESTATE 17U
#define TD_PROTSTATUS 18U
#define TD_EJECT 19U
#define TD_REMOVE 20U
#define TD_ADDCHANGEINT 21U
#define TD_REMCHANGEINT 22U
#define TD_GETDRIVETYPE 23U
#define TD_GETNUMTRACKS 24U
#define TD_GETGEOMETRY 25U
#define TDERR_DiskChanged -5
#define TDERR_NotSpecified -6
#define TDERR_WriteProt -7
#define DRIVE3_5 1U
#define DG_DIRECT_ACCESS 1U
#define DGF_REMOVABLE 1U

#endif
