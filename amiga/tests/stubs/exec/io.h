#ifndef NATIVE_TEST_EXEC_IO_H
#define NATIVE_TEST_EXEC_IO_H

#include <exec/devices.h>

struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};

struct IOStdReq {
    struct IORequest io;
    APTR io_Data;
    ULONG io_Length;
    ULONG io_Offset;
    ULONG io_Actual;
};

struct IOExtTD { struct IOStdReq iotd_Req; ULONG iotd_Count; APTR iotd_SecLabel; };

#define IOF_QUICK 1U
#define CMD_RESET 1U
#define CMD_READ 2U
#define CMD_WRITE 3U
#define CMD_UPDATE 4U
#define CMD_CLEAR 5U
#define CMD_START 6U
#define CMD_STOP 7U
#define CMD_FLUSH 8U
#define CMD_NONSTD 9U

#endif
