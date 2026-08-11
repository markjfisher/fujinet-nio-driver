#include <devices/trackdisk.h>
#include <exec/io.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_disk_device.h"
#include "fujinet-nio.h"
#include "fn_platform.h"

#define MAX_DRIVES 8
#define CATALOG_BUFFER_SIZE 1024
#define MAPPINGS_SIZE 17

static UBYTE service_buffer[CATALOG_BUFFER_SIZE];

static int parse_drive(const char *text, ULONG *drive)
{
    char *end;
    ULONG value = strtoul(text, &end, 0);
    if (*text == '\0' || *end != '\0' || value >= MAX_DRIVES) return 0;
    *drive = value;
    return 1;
}

static int resolve_catalog_slot(ULONG slot, char *uri, size_t capacity)
{
    fn_slot_catalog_io_t io = {service_buffer, sizeof(service_buffer)};
    fn_slot_catalog_entry_t entry;
    uint8_t result;
    if (slot > 255 || fn_init() != FN_OK) return 0;
    result = fn_slot_catalog_get(&io, (uint8_t)slot, &entry);
    if (result != FN_OK || !(entry.flags & FN_SLOT_CATALOG_ENTRY_VALID) ||
        entry.uri_len == 0 || entry.uri_len >= capacity) {
        fn_transport_close();
        return 0;
    }
    memcpy(uri, entry.uri, entry.uri_len);
    uri[entry.uri_len] = '\0';
    fn_transport_close();
    return 1;
}

static int save_mapping(ULONG drive, ULONG slot, int writable, int valid)
{
    fn_appstore_io_t io = {service_buffer, sizeof(service_buffer)};
    fn_appstore_read_t read_result;
    fn_appstore_write_t write_result;
    UBYTE mappings[MAPPINGS_SIZE];
    uint8_t result;
    memset(mappings, 0, sizeof(mappings));
    mappings[0] = 1;
    if (fn_init() != FN_OK) return 0;
    result = fn_appstore_read(&io, "config-nio", "mappings", 0, mappings,
                              sizeof(mappings), &read_result);
    if (result != FN_OK) {
        fn_transport_close();
        return 0;
    }
    if (!(read_result.flags & FN_APPSTORE_READ_EXISTS)) {
        memset(mappings, 0, sizeof(mappings));
        mappings[0] = 1;
    } else if (read_result.bytes_read != sizeof(mappings) || mappings[0] != 1) {
        fn_transport_close();
        return 0;
    }
    mappings[1 + drive * 2] = valid ? (UBYTE)(1 | (writable ? 0 : 2)) : 0;
    mappings[2 + drive * 2] = valid ? (UBYTE)slot : 0;
    result = fn_appstore_write(&io, "config-nio", "mappings", 0, mappings,
                               sizeof(mappings), &write_result);
    fn_transport_close();
    return result == FN_OK && write_result.bytes_written == sizeof(mappings);
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
    int eject = 0;
    ULONG catalog_slot = 0;
    ULONG drive = 0;
    char catalog_uri[768];
    const char *mount_uri = NULL;

    if ((argc < 2 || argc > 5) || argv[1][0] == '\0') {
        fprintf(stderr,
                "Usage: fujinet-mount SLOT DRIVE [RW|RO]\n"
                "       fujinet-mount --uri DRIVE URI [RW|RO]\n"
                "       fujinet-mount URI | --writable URI\n"
                "       fujinet-mount --eject [DRIVE] | --trace | --read LBA [RESULT]\n");
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
        if (!resolve_catalog_slot(catalog_slot, catalog_uri,
                                  sizeof(catalog_uri))) {
            fprintf(stderr, "fujinet-mount: catalogue slot %lu is unavailable\n",
                    catalog_slot);
            return 20;
        }
        catalog_mount = 1;
        mount_uri = catalog_uri;
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
    } else if (strcmp(argv[1], "--eject") == 0 && argc == 3) {
        if (!parse_drive(argv[2], &drive)) return 10;
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

    if (strcmp(argv[1], "--eject") == 0) {
        eject = 1;
        request->iotd_Req.io_Command = TD_EJECT;
        request->iotd_Req.io_Data = NULL;
        request->iotd_Req.io_Length = 0;
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
        if (request->iotd_Req.io_Command == TD_GETGEOMETRY)
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
    if (result == 0 && catalog_mount &&
        !save_mapping(drive, catalog_slot, requested_writable, 1)) {
        fprintf(stderr, "fujinet-mount: mounted, but mapping was not persisted\n");
        return 20;
    }
    if (result == 0 && eject && !save_mapping(drive, 0, 0, 0)) {
        fprintf(stderr, "fujinet-mount: ejected, but mapping was not cleared\n");
        return 20;
    }
    return result == 0 ? 0 : 20;
}
