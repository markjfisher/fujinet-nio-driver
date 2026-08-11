#include <devices/trackdisk.h>
#include <dos/dostags.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_disk_device.h"
#define MAX_DRIVES 8

struct boundary_worker_state {
    ULONG started;
    ULONG done;
    LONG result;
    ULONG drive;
};

static struct boundary_worker_state boundary_worker;
static volatile ULONG boundary_causes;

static void boundary_interrupt(void)
{
    ++boundary_causes;
}

static void boundary_worker_entry(void)
{
    struct MsgPort *worker_port;
    struct IOExtTD *worker_request;
    static UBYTE data[512];

    worker_port = CreatePort(NULL, 0);
    if (worker_port == NULL) {
        boundary_worker.result = IOERR_OPENFAIL;
        boundary_worker.done = 1;
        return;
    }
    worker_request = (struct IOExtTD *)CreateExtIO(worker_port,
                                                    sizeof(*worker_request));
    if (worker_request == NULL ||
        OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME,
                   boundary_worker.drive,
                   (struct IORequest *)worker_request, 0) != 0) {
        if (worker_request != NULL) DeleteExtIO((struct IORequest *)worker_request);
        DeletePort(worker_port);
        boundary_worker.result = IOERR_OPENFAIL;
        boundary_worker.done = 1;
        return;
    }

    worker_request->iotd_Req.io_Command = CMD_READ;
    worker_request->iotd_Req.io_Data = data;
    worker_request->iotd_Req.io_Length = sizeof(data);
    worker_request->iotd_Req.io_Offset = 0;
    boundary_worker.started = 1;
    boundary_worker.result = DoIO((struct IORequest *)worker_request);
    CloseDevice((struct IORequest *)worker_request);
    DeleteExtIO((struct IORequest *)worker_request);
    DeletePort(worker_port);
    boundary_worker.done = 1;
}

static int parse_drive(const char *text, ULONG *drive)
{
    char *end;
    ULONG value = strtoul(text, &end, 0);
    if (*text == '\0' || *end != '\0' || value >= MAX_DRIVES) return 0;
    *drive = value;
    return 1;
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOExtTD *request;
    struct DriveGeometry geometry;
    struct fujinet_disk_trace trace;
    LONG result;
    int requested_writable = 0;
    int catalog_mount = 0;
    int update = 0;
    int status = 0;
    ULONG catalog_slot = 0;
    ULONG drive = 0;
    const char *mount_uri = NULL;

    if ((argc < 2 || argc > 5) || argv[1][0] == '\0') {
        fprintf(stderr,
                "Usage: fujinet-mount SLOT DRIVE [RW|RO]\n"
                "       fujinet-mount --uri DRIVE URI [RW|RO]\n"
                "       fujinet-mount URI | --writable URI\n"
                "       fujinet-mount --eject [DRIVE] | --update DRIVE\n"
                "       fujinet-mount --status DRIVE | --trace | --read LBA [RESULT]\n"
                "       fujinet-mount --boundary | --malformed [DRIVE]\n");
        return 10;
    }

    if (argv[1][0] >= '0' && argv[1][0] <= '9' &&
        (argc == 3 || argc == 4)) {
        char *end;
        catalog_slot = strtoul(argv[1], &end, 0);
        if (*end != '\0' || catalog_slot > 255 || !parse_drive(argv[2], &drive))
            return 10;
        if (argc == 4) {
            if (strcmp(argv[3], "RW") == 0 || strcmp(argv[3], "rw") == 0)
                requested_writable = 1;
            else if (strcmp(argv[3], "RO") != 0 && strcmp(argv[3], "ro") != 0)
                return 10;
        }
        catalog_mount = 1;
    } else if (strcmp(argv[1], "--uri") == 0 && (argc == 4 || argc == 5)) {
        if (!parse_drive(argv[2], &drive)) return 10;
        mount_uri = argv[3];
        if (argc == 5) {
            if (strcmp(argv[4], "RW") == 0 || strcmp(argv[4], "rw") == 0)
                requested_writable = 1;
            else if (strcmp(argv[4], "RO") != 0 && strcmp(argv[4], "ro") != 0)
                return 10;
        }
    } else if (strcmp(argv[1], "--writable") == 0 && argc == 3) {
        requested_writable = 1;
        mount_uri = argv[2];
    } else if (argv[1][0] != '-' && argc == 2) {
        mount_uri = argv[1];
    } else if (strcmp(argv[1], "--eject") == 0 && (argc == 2 || argc == 3)) {
        if (argc == 3 && !parse_drive(argv[2], &drive)) return 10;
    } else if (strcmp(argv[1], "--update") == 0 && argc == 3) {
        if (!parse_drive(argv[2], &drive)) return 10;
        update = 1;
    } else if (strcmp(argv[1], "--status") == 0 && argc == 3) {
        if (!parse_drive(argv[2], &drive)) return 10;
        status = 1;
    } else if (strcmp(argv[1], "--boundary") == 0 && (argc == 2 || argc == 3)) {
        if (argc == 3 && !parse_drive(argv[2], &drive)) return 10;
    } else if (strcmp(argv[1], "--malformed") == 0 && (argc == 2 || argc == 3)) {
        if (argc == 3 && !parse_drive(argv[2], &drive)) return 10;
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
    if (OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME, drive,
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

    if (strcmp(argv[1], "--boundary") == 0) {
        static const UWORD commands[] = {
            TD_CHANGENUM, CMD_STOP, CMD_START, CMD_FLUSH, TD_REMCHANGEINT
        };
        UWORD i;
        LONG wait_result;
        ULONG spins;
        struct Process *worker_process;
        struct MsgPort *other_port;
        struct IOExtTD *other_request;
        struct MsgPort *event_port;
        struct IOExtTD *event_request;
        struct Interrupt event_interrupt;
        static const char worker_uri[] = "host:/standard.adf";
        static UBYTE parent_data[512];
        static UBYTE other_data[512];

        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT;
        request->iotd_Req.io_Data = (APTR)worker_uri;
        request->iotd_Req.io_Length = sizeof(worker_uri);
        result = DoIO((struct IORequest *)request);
        if (result != 0) {
            fprintf(stderr, "fujinet-mount: queue setup mount failed (%ld)\n", result);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }

        other_port = CreatePort(NULL, 0);
        other_request = other_port == NULL ? NULL :
            (struct IOExtTD *)CreateExtIO(other_port, sizeof(*other_request));
        if (other_request == NULL ||
            OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME,
                       (drive + 1) % MAX_DRIVES,
                       (struct IORequest *)other_request, 0) != 0) {
            if (other_request != NULL) DeleteExtIO((struct IORequest *)other_request);
            if (other_port != NULL) DeletePort(other_port);
            fprintf(stderr, "fujinet-mount: cannot open second boundary unit\n");
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }

        memset(&boundary_worker, 0, sizeof(boundary_worker));
        boundary_worker.drive = drive;
        worker_process = CreateNewProcTags(
            NP_Entry, (ULONG)boundary_worker_entry,
            NP_StackSize, 8192,
            NP_Name, (ULONG)"FujiNet boundary worker",
            TAG_DONE);
        if (worker_process == NULL) {
            fprintf(stderr, "fujinet-mount: cannot create boundary worker\n");
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }
        for (spins = 0; spins < 100 && !boundary_worker.started; ++spins)
            Delay(1);
        if (!boundary_worker.started) {
            fprintf(stderr, "fujinet-mount: boundary worker did not start\n");
            while (!boundary_worker.done) Delay(1);
            CloseDevice((struct IORequest *)other_request);
            DeleteExtIO((struct IORequest *)other_request);
            DeletePort(other_port);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }

        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = CMD_READ;
        request->iotd_Req.io_Data = parent_data;
        request->iotd_Req.io_Length = sizeof(parent_data);
        request->iotd_Req.io_Offset = 0;
        other_request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        other_request->iotd_Req.io_Flags = 0;
        other_request->iotd_Req.io_Error = 0;
        other_request->iotd_Req.io_Actual = 0;
        other_request->iotd_Req.io_Command = CMD_READ;
        other_request->iotd_Req.io_Data = other_data;
        other_request->iotd_Req.io_Length = sizeof(other_data);
        other_request->iotd_Req.io_Offset = 0;
        SendIO((struct IORequest *)other_request);
        SendIO((struct IORequest *)request);
        AbortIO((struct IORequest *)request);
        AbortIO((struct IORequest *)other_request);
        wait_result = WaitIO((struct IORequest *)request);
        {
            LONG other_wait_result = WaitIO((struct IORequest *)other_request);
            if (other_wait_result != IOERR_ABORTED) wait_result = other_wait_result;
        }
        while (!boundary_worker.done) Delay(1);
        if (wait_result != IOERR_ABORTED || boundary_worker.result != 0) {
            fprintf(stderr, "fujinet-mount: queued abort failed (%ld/%ld)\n",
                    wait_result, boundary_worker.result);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }
        CloseDevice((struct IORequest *)other_request);
        DeleteExtIO((struct IORequest *)other_request);
        DeletePort(other_port);

        event_port = CreatePort(NULL, 0);
        event_request = event_port == NULL ? NULL :
            (struct IOExtTD *)CreateExtIO(event_port, sizeof(*event_request));
        if (event_request == NULL ||
            OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME, drive,
                       (struct IORequest *)event_request, 0) != 0) {
            if (event_request != NULL) DeleteExtIO((struct IORequest *)event_request);
            if (event_port != NULL) DeletePort(event_port);
            fprintf(stderr, "fujinet-mount: cannot open notification event request\n");
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }
        memset(&event_interrupt, 0, sizeof(event_interrupt));
        event_interrupt.is_Code = boundary_interrupt;
        boundary_causes = 0;

        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_ADDCHANGEINT;
        request->iotd_Req.io_Data = &event_interrupt;
        request->iotd_Req.io_Length = 0;
        SendIO((struct IORequest *)request);
        event_request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        event_request->iotd_Req.io_Flags = 0;
        event_request->iotd_Req.io_Error = 0;
        event_request->iotd_Req.io_Actual = 0;
        event_request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT;
        event_request->iotd_Req.io_Data = (APTR)worker_uri;
        event_request->iotd_Req.io_Length = sizeof(worker_uri);
        result = DoIO((struct IORequest *)event_request);
        for (spins = 0; spins < 20 && boundary_causes == 0; ++spins)
            Delay(1);
        if (result != 0 || boundary_causes == 0) {
            fprintf(stderr, "fujinet-mount: notification cause failed (%ld/%lu)\n",
                    result, boundary_causes);
            CloseDevice((struct IORequest *)event_request);
            DeleteExtIO((struct IORequest *)event_request);
            DeletePort(event_port);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }
        request->iotd_Req.io_Command = TD_REMCHANGEINT;
        result = DoIO((struct IORequest *)request);
        if (result != 0) {
            fprintf(stderr, "fujinet-mount: change removal failed (%ld)\n", result);
            CloseDevice((struct IORequest *)event_request);
            DeleteExtIO((struct IORequest *)event_request);
            DeletePort(event_port);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }
        CloseDevice((struct IORequest *)event_request);
        DeleteExtIO((struct IORequest *)event_request);
        DeletePort(event_port);

        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_ADDCHANGEINT;
        request->iotd_Req.io_Data = NULL;
        request->iotd_Req.io_Length = 0;
        SendIO((struct IORequest *)request);
        AbortIO((struct IORequest *)request);
        result = 0;
        wait_result = WaitIO((struct IORequest *)request);
        if (result != 0 || wait_result != IOERR_ABORTED) {
            fprintf(stderr, "fujinet-mount: change abort failed (%ld/%ld)\n",
                    result, wait_result);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }

        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_REMOVE;
        request->iotd_Req.io_Data = NULL;
        request->iotd_Req.io_Length = 0;
        SendIO((struct IORequest *)request);
        AbortIO((struct IORequest *)request);
        wait_result = WaitIO((struct IORequest *)request);
        if (wait_result != IOERR_ABORTED) {
            fprintf(stderr, "fujinet-mount: remove abort failed (%ld)\n",
                    wait_result);
            CloseDevice((struct IORequest *)request);
            DeleteExtIO((struct IORequest *)request);
            DeletePort(port);
            return 20;
        }

        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
            request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
            request->iotd_Req.io_Flags = 0;
            request->iotd_Req.io_Error = 0;
            request->iotd_Req.io_Actual = 0;
            request->iotd_Req.io_Command = commands[i];
            request->iotd_Req.io_Data = NULL;
            request->iotd_Req.io_Length = 0;
            result = DoIO((struct IORequest *)request);
            if (result != 0) break;
        }
        if (result == 0) printf("EXEC BOUNDARY PASS commands=%lu notifications=3 remove=1 queue=1 multi=1 cause=1\n",
                                (ULONG)(sizeof(commands) / sizeof(commands[0])));
        else fprintf(stderr, "fujinet-mount: boundary command %lu failed (%ld)\n",
                     (ULONG)i, result);
        CloseDevice((struct IORequest *)request);
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        return result == 0 ? 0 : 20;
    }

    if (strcmp(argv[1], "--malformed") == 0) {
        static const char malformed_uri[] = "host:/standard.adf";
        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT;
        request->iotd_Req.io_Data = (APTR)malformed_uri;
        request->iotd_Req.io_Length = sizeof(malformed_uri) - 1;
        result = DoIO((struct IORequest *)request);
        if (result == IOERR_BADLENGTH)
            printf("MALFORMED URI REJECTED error=%ld\n", result);
        else
            fprintf(stderr, "fujinet-mount: malformed URI accepted (%ld)\n", result);
        CloseDevice((struct IORequest *)request);
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        return result == IOERR_BADLENGTH ? 0 : 20;
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

    if (status) {
        ULONG change_count, change_state, protected_state;
        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_CHANGENUM;
        result = DoIO((struct IORequest *)request);
        change_count = request->iotd_Req.io_Actual;
        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_CHANGESTATE;
        if (result == 0) result = DoIO((struct IORequest *)request);
        change_state = request->iotd_Req.io_Actual;
        request->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        request->iotd_Req.io_Flags = 0;
        request->iotd_Req.io_Error = 0;
        request->iotd_Req.io_Actual = 0;
        request->iotd_Req.io_Command = TD_PROTSTATUS;
        if (result == 0) result = DoIO((struct IORequest *)request);
        protected_state = request->iotd_Req.io_Actual;
        if (result == 0)
            printf("STATUS drive=%lu change=%lu absent=%lu protected=%lu\n",
                   drive, change_count, change_state, protected_state);
        CloseDevice((struct IORequest *)request);
        DeleteExtIO((struct IORequest *)request);
        DeletePort(port);
        return result == 0 ? 0 : 20;
    }

    if (update) {
        request->iotd_Req.io_Command = CMD_UPDATE;
        request->iotd_Req.io_Data = NULL;
        request->iotd_Req.io_Length = 0;
    } else if (strcmp(argv[1], "--eject") == 0) {
        request->iotd_Req.io_Command = TD_EJECT;
        request->iotd_Req.io_Data = NULL;
        request->iotd_Req.io_Length = 0;
    } else if (catalog_mount) {
        static struct fujinet_disk_catalog_mount catalog;
        catalog.catalog_slot = (UBYTE)catalog_slot;
        catalog.writable = requested_writable != 0;
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT_CATALOG;
        request->iotd_Req.io_Data = &catalog;
        request->iotd_Req.io_Length = sizeof(catalog);
    } else if (mount_uri != NULL && requested_writable) {
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT_WRITABLE;
        request->iotd_Req.io_Data = (APTR)mount_uri;
        request->iotd_Req.io_Length = (ULONG)strlen(mount_uri) + 1;
    } else {
        request->iotd_Req.io_Command = FUJINET_DISK_CMD_MOUNT;
        request->iotd_Req.io_Data = (APTR)mount_uri;
        request->iotd_Req.io_Length = (ULONG)strlen(mount_uri) + 1;
    }
    result = DoIO((struct IORequest *)request);
    if (result == 0 && request->iotd_Req.io_Command != TD_EJECT) {
        memset(&geometry, 0, sizeof(geometry));
        request->iotd_Req.io_Command = TD_GETGEOMETRY;
        request->iotd_Req.io_Data = &geometry;
        request->iotd_Req.io_Length = sizeof(geometry);
        result = DoIO((struct IORequest *)request);
    }

    if (result == 0) {
        if (update)
            printf("UPDATED drive=%lu slot=%lu\n", drive, drive + 1);
        else if (request->iotd_Req.io_Command == TD_GETGEOMETRY)
            printf("MOUNTED drive=%lu slot=%lu readonly=%d sectorSize=%lu sectorCount=%lu\n",
                   drive, drive + 1, requested_writable ? 0 : 1, geometry.dg_SectorSize,
                   geometry.dg_TotalSectors);
        else
            printf("EJECTED drive=%lu slot=%lu\n", drive, drive + 1);
    } else {
        fprintf(stderr, "fujinet-mount: request failed (%ld)\n", result);
    }

    CloseDevice((struct IORequest *)request);
    DeleteExtIO((struct IORequest *)request);
    DeletePort(port);
    return result == 0 ? 0 : 20;
}
