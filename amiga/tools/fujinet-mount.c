#include <devices/trackdisk.h>
#include <exec/io.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_disk_device.h"

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOExtTD *request;
    struct DriveGeometry geometry;
    struct fujinet_disk_trace trace;
    LONG result;

    if ((argc < 2 || argc > 4) || argv[1][0] == '\0') {
        fprintf(stderr,
                "Usage: fujinet-mount URI | --trace | --read LBA [RESULT]\n");
        return 10;
    }

    port = CreatePort(NULL, 0);
    if (port == NULL) {
        fprintf(stderr, "fujinet-mount: cannot create message port\n");
        return 20;
    }
    request = (struct IOExtTD *)CreateExtIO(port, sizeof(*request));
    if (request == NULL) {
        DeletePort(port);
        fprintf(stderr, "fujinet-mount: cannot create I/O request\n");
        return 20;
    }
    if (OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME, 0,
                   (struct IORequest *)request, 0) != 0) {
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        fprintf(stderr, "fujinet-mount: cannot open %s\n",
                FUJINET_DISK_DEVICE_NAME);
        return 20;
    }

    if (strcmp(argv[1], "--trace") == 0) {
        UWORD i;
        memset(&trace, 0, sizeof(trace));
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_TRACE;
        request->iotd_Req.io_Data = &trace;
        request->iotd_Req.io_Length = sizeof(trace);
        result = DoIO((struct IORequest *)request);
        if (result == 0) {
            printf("TRACE");
            for (i = 0; i < trace.count; ++i) {
                printf(" %04lx/%lu/%lu/%lu/%ld",
                       (ULONG)trace.commands[i], trace.offsets[i],
                       trace.lengths[i], trace.actuals[i],
                       (LONG)trace.errors[i]);
            }
            printf("\n");
        }
        CloseDevice((struct IORequest *)request);
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        return result == 0 ? 0 : 20;
    }

    if (strcmp(argv[1], "--read") == 0 && (argc == 3 || argc == 4)) {
        static UBYTE data[512];
        ULONG lba = strtoul(argv[2], NULL, 0);
        FILE *output = stdout;
        if (argc == 4) {
            output = fopen(argv[3], "w");
            if (output == NULL) result = 1;
        }
        request->iotd_Req.io_Command = CMD_READ;
        request->iotd_Req.io_Data = data;
        request->iotd_Req.io_Length = sizeof(data);
        request->iotd_Req.io_Offset = lba * sizeof(data);
        if (output != NULL) result = DoIO((struct IORequest *)request);
        if (result == 0) {
            fprintf(output, "READ OK lba=%lu actual=%lu\n", lba,
                    request->iotd_Req.io_Actual);
        }
        if (output != NULL && output != stdout) fclose(output);
        CloseDevice((struct IORequest *)request);
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        return result == 0 ? 0 : 20;
    }

    request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT;
    request->iotd_Req.io_Data = argv[1];
    request->iotd_Req.io_Length = (ULONG)strlen(argv[1]) + 1;
    result = DoIO((struct IORequest *)request);
    if (result == 0) {
        memset(&geometry, 0, sizeof(geometry));
        request->iotd_Req.io_Command = TD_GETGEOMETRY;
        request->iotd_Req.io_Data = &geometry;
        request->iotd_Req.io_Length = sizeof(geometry);
        result = DoIO((struct IORequest *)request);
    }

    if (result == 0) {
        printf("MOUNTED slot=1 readonly=1 sectorSize=%lu sectorCount=%lu\n",
               geometry.dg_SectorSize, geometry.dg_TotalSectors);
    } else {
        fprintf(stderr, "fujinet-mount: request failed (%ld)\n", result);
    }

    CloseDevice((struct IORequest *)request);
    DeleteExtIO((struct IORequest *)request);
    DeletePort(port);
    return result == 0 ? 0 : 20;
}
