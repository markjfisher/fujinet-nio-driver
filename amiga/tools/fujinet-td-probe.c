#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/exec.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

static volatile ULONG legacy_causes;

static void legacy_change_code(void)
{
    ++legacy_causes;
}

static void report_io(const char *label, struct IOExtTD *io)
{
    printf("%s flags=%u error=%d actual=%lu node=%u\n", label,
           (unsigned)io->iotd_Req.io_Flags,
           (int)io->iotd_Req.io_Error,
           (unsigned long)io->iotd_Req.io_Actual,
           (unsigned)io->iotd_Req.io_Message.mn_Node.ln_Type);
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOExtTD *io;
    struct Interrupt legacy;
    UBYTE buffer[TD_SECTOR];
    struct Message *reply;
    LONG result;

    if (argc != 2) {
        printf("usage: tdprobe DEVICE\n");
        return 10;
    }
    port = CreatePort(NULL, 0);
    if (port == NULL) return 20;
    io = (struct IOExtTD *)CreateExtIO(port, sizeof(*io));
    if (io == NULL) {
        DeletePort(port);
        return 20;
    }
    if (OpenDevice((CONST_STRPTR)argv[1], 0, (struct IORequest *)io, 0) != 0) {
        DeleteExtIO((struct IORequest *)io);
        DeletePort(port);
        return 20;
    }

    memset(&legacy, 0, sizeof(legacy));
    legacy.is_Code = (void (*)())legacy_change_code;
    io->iotd_Req.io_Command = TD_REMOVE;
    io->iotd_Req.io_Flags = IOF_QUICK;
    io->iotd_Req.io_Data = &legacy;
    io->iotd_Req.io_Length = sizeof(legacy);
    io->iotd_Req.io_Actual = 0xdeadbeefUL;
    legacy_causes = 0;
    BeginIO((struct IORequest *)io);
    report_io("remove-after-begin", io);
    reply = GetMsg(port);
    printf("remove reply=%s causes=%lu\n", reply ? "yes" : "no",
           (unsigned long)legacy_causes);

    memset(buffer, 0, sizeof(buffer));
    io->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->iotd_Req.io_Flags = 0;
    io->iotd_Req.io_Error = 0x7f;
    io->iotd_Req.io_Actual = 0xdeadbeefUL;
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data = buffer;
    io->iotd_Req.io_Length = sizeof(buffer);
    io->iotd_Req.io_Offset = 880UL * TD_SECTOR;
    report_io("read-before-send", io);
    SendIO((struct IORequest *)io);
    report_io("read-after-send", io);
    WaitPort(port);
    reply = GetMsg(port);
    printf("read reply=%s\n", reply ? "yes" : "no");
    report_io("read-after-getmsg", io);
    result = io->iotd_Req.io_Error;

    io->iotd_Req.io_Command = TD_REMOVE;
    io->iotd_Req.io_Flags = IOF_QUICK;
    io->iotd_Req.io_Data = NULL;
    BeginIO((struct IORequest *)io);
    report_io("remove-after-clear", io);

    CloseDevice((struct IORequest *)io);
    DeleteExtIO((struct IORequest *)io);
    DeletePort(port);
    return result == 0 ? 0 : 30;
}
