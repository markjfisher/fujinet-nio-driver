#ifndef NATIVE_TEST_NEWSTYLE_H
#define NATIVE_TEST_NEWSTYLE_H
#include <exec/types.h>
struct NSDeviceQueryResult { UWORD nsdqr_DevQueryFormat; UWORD nsdqr_SizeAvailable; ULONG nsdqr_DeviceType; ULONG nsdqr_DeviceSubType; APTR nsdqr_SupportedCommands; };
#define NSCMD_DEVICEQUERY 0x4000U
#define NSDEVTYPE_TRACKDISK 1U
#endif
