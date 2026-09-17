#include "fujinet_nio_exchange_disk_adapter.h"
#include <proto/exec.h>
#include <exec/nodes.h>
#include <stdio.h>
#include <string.h>

uint8_t fn_exchange_disk_adapter_exchange(void *opaque, const uint8_t *request,
                                 uint16_t length, uint8_t *response,
                                 uint16_t capacity, uint16_t *response_length)
{
    struct ordinary_disk_context *c = opaque;
    memset(&c->nio, 0, sizeof(c->nio));
    c->nio.fn_io.io_Device = c->nio_open.io_Device;
    c->nio.fn_io.io_Unit = c->nio_open.io_Unit;
    c->nio.fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    c->nio.fn_io.io_Message.mn_ReplyPort = c->port;
    c->nio.fn_io.io_Message.mn_Length = sizeof(c->nio);
    c->nio.fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
    c->nio.fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
    c->nio.fn_request_data = request;
    c->nio.fn_request_length = length;
    c->nio.fn_response_data = response;
    c->nio.fn_response_capacity = capacity;
    (void)DoIO(&c->nio.fn_io);
    *response_length = c->nio.fn_response_length;
    printf("ordinary remote-info io_Error=%d nio=%u result=%u cause=%u "
           "native=%u status=%u\n", (int)c->nio.fn_io.io_Error,
           (unsigned)c->nio.fn_nio_error, (unsigned)c->nio.fn_pad[1],
           (unsigned)c->nio.fn_pad[2], (unsigned)(c->nio.fn_flags & 255),
           (unsigned)(c->nio.fn_flags >> 8));
    return c->nio.fn_io.io_Error ? FN_ERR_IO : c->nio.fn_nio_error;
}

int fn_exchange_disk_adapter_io(void *opaque, enum fn_exchange_disk_op op,
                        struct fn_exchange_disk_state *state, uint8_t *buffer)
{
    struct ordinary_disk_context *c = opaque;
    struct IOStdReq *io = &c->disk.iotd_Req;
    struct IORequest *request = (struct IORequest *)io;
    struct DriveGeometry geometry;
    fn_disk_info_t info;
    BYTE error;
    ULONG actual;
    UWORD command = 0;
    void *data = NULL;
    ULONG length = 0;
    const char *name = "unknown";
    unsigned index, attempt;
    uint8_t result;
    if (op == FN_EXDISK_REMOTE_INFO) {
        memset(&info, 0, sizeof(info));
        result = fn_disk_info_context(&c->codec, (uint8_t)c->opts->slot, &info);
        printf("ordinary remote-info codec_result=%u\n", (unsigned)result);
        if (result != FN_OK || info.slot != c->opts->slot) return -1;
        state->mounted = (info.flags & FN_DISK_FLAG_MOUNTED) != 0;
        return 0;
    }
    switch (op) {
    case FN_EXDISK_LOCAL_STATE: command = TD_CHANGESTATE; name = "local-state"; break;
    case FN_EXDISK_MOUNT:
        command = c->opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE ?
            FUJINET_DISK_CMD_MOUNT_WRITABLE : FUJINET_DISK_CMD_MOUNT;
        name = "mount"; data = (void *)c->opts->fixture_uri;
        length = strlen(c->opts->fixture_uri) + 1; break;
    case FN_EXDISK_GEOMETRY:
        command = TD_GETGEOMETRY; name = "geometry";
        memset(&geometry, 0, sizeof(geometry));
        data = &geometry; length = sizeof(geometry); break;
    case FN_EXDISK_CHANGE: command = TD_CHANGENUM; name = "change-count"; break;
    case FN_EXDISK_READ: command = ETD_READ; name = "read"; data = buffer; length = 512; break;
    case FN_EXDISK_WRITE: command = ETD_WRITE; name = "write"; data = buffer; length = 512; break;
    case FN_EXDISK_FLUSH: command = CMD_UPDATE; name = "flush"; break;
    default: return -1;
    }
    request->io_Command = FUJINET_DISK_CMD_TRACE_CLEAR;
    io->io_Data = NULL; io->io_Length = 0;
    if (DoIO((struct IORequest *)io) != 0) {
        printf("ordinary op=%s trial=%u trace_clear io_Error=%d io_Actual=%lu\n",
               name, state->trial, (int)request->io_Error, (unsigned long)io->io_Actual);
        return -1;
    }
    request->io_Command = command; io->io_Data = data; io->io_Length = length;
    io->io_Offset = (ULONG)c->opts->lba * 512UL;
    c->disk.iotd_Count = state->change;
    c->disk.iotd_SecLabel = NULL;
    (void)DoIO((struct IORequest *)io);
    /* TRACE overwrites these fields: capture before querying. */
    error = request->io_Error; actual = io->io_Actual;
    printf("ordinary op=%s slot=%u lba=%lu io_Error=%d io_Actual=%lu trial=%u\n",
           name, c->opts->slot, (unsigned long)c->opts->lba,
           (int)error, (unsigned long)actual, state->trial);
    if (op == FN_EXDISK_READ || op == FN_EXDISK_WRITE) {
        memset(&c->trace, 0, sizeof(c->trace));
        request->io_Command = FUJINET_DISK_CMD_TRACE;
        io->io_Data = &c->trace; io->io_Length = sizeof(c->trace);
        if (DoIO((struct IORequest *)io) != 0) {
            printf("ordinary op=%s trial=%u trace_query io_Error=%d io_Actual=%lu "
                   "data_io_Error=%d data_io_Actual=%lu\n", name, state->trial,
                   (int)request->io_Error, (unsigned long)io->io_Actual,
                   (int)error, (unsigned long)actual);
            return -1;
        }
        for (index = 0; index < c->trace.count && index < FUJINET_DISK_TRACE_CAPACITY; ++index) {
            for (attempt = 0; attempt < c->trace.exchange_attempts[index] &&
                 attempt < FUJINET_DISK_TRACE_ATTEMPTS; ++attempt)
                printf("ordinary op=%s attempt=%u result=%u cause=%u native=%u status=%u trial=%u\n",
                    name, attempt + 1,
                    (unsigned)c->trace.exchange_results[index][attempt],
                    (unsigned)c->trace.exchange_causes[index][attempt],
                    (unsigned)c->trace.exchange_native_errors[index][attempt],
                    (unsigned)c->trace.exchange_statuses[index][attempt], state->trial);
        }
    }
    if (error) return -1;
    if (op == FN_EXDISK_READ && actual == 512) {
        uint32_t digest = 2166136261UL;
        for (index = 0; index < 512; ++index)
            digest = (digest ^ buffer[index]) * 16777619UL;
        printf("ordinary read trial=%u checksum_fnv1a32=%08lx\n",
               state->trial, (unsigned long)digest);
    }
    state->actual = actual;
    if (op == FN_EXDISK_LOCAL_STATE) state->mounted = actual == 0;
    if (op == FN_EXDISK_CHANGE) state->change = actual;
    if (op == FN_EXDISK_GEOMETRY) {
        if (actual != sizeof(geometry)) return -1;
        state->sector_size = geometry.dg_SectorSize;
        state->sectors = geometry.dg_TotalSectors;
    }
    return 0;
}

