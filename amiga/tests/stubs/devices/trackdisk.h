#ifndef NATIVE_TEST_TRACKDISK_H
#define NATIVE_TEST_TRACKDISK_H

#include <exec/io.h>

struct DriveGeometry { ULONG dg_SectorSize, dg_TotalSectors, dg_CylSectors, dg_Cylinders, dg_Heads, dg_TrackSectors, dg_BufMemType, dg_DeviceType, dg_Flags; UBYTE dg_Reserved; };
#define TD_MOTOR 9U
#define TD_SEEK 10U
#define TD_FORMAT 11U
#define TD_REMOVE 12U
#define TD_CHANGENUM 13U
#define TD_CHANGESTATE 14U
#define TD_PROTSTATUS 15U
#define TD_RAWREAD 16U
#define TD_RAWWRITE 17U
#define TD_GETDRIVETYPE 18U
#define TD_GETNUMTRACKS 19U
#define TD_ADDCHANGEINT 20U
#define TD_REMCHANGEINT 21U
#define TD_GETGEOMETRY 22U
#define TD_EJECT 23U
#define TDF_EXTCOM (1U << 15)
#define ETD_READ (CMD_READ | TDF_EXTCOM)
#define ETD_WRITE (CMD_WRITE | TDF_EXTCOM)
#define ETD_UPDATE (CMD_UPDATE | TDF_EXTCOM)
#define ETD_CLEAR (CMD_CLEAR | TDF_EXTCOM)
#define TDERR_DiskChanged -5
#define TDERR_NotSpecified -6
#define TDERR_WriteProt -7
#define DRIVE3_5 1U
#define DG_DIRECT_ACCESS 1U
#define DGF_REMOVABLE 1U

#endif
