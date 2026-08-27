#include <exec/io.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_nio_device.h"

static void write_le32(UBYTE *data, ULONG value)
{
    data[0] = (UBYTE)value;
    data[1] = (UBYTE)(value >> 8);
    data[2] = (UBYTE)(value >> 16);
    data[3] = (UBYTE)(value >> 24);
}

static ULONG read_le32(const UBYTE *data)
{
    return (ULONG)data[0] | ((ULONG)data[1] << 8) |
           ((ULONG)data[2] << 16) | ((ULONG)data[3] << 24);
}

static void init_request(struct FujiNetNIORequest *req, struct MsgPort *port,
                         UWORD command)
{
    memset(req, 0, sizeof(*req));
    req->fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->fn_io.io_Message.mn_ReplyPort = port;
    req->fn_io.io_Message.mn_Length = sizeof(*req);
    req->fn_io.io_Command = command;
    req->fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IORequest open_request;
    struct FujiNetNIORequest req;
    UBYTE data[4];
    ULONG baud;
    char *end;

    if (argc > 2) {
        fprintf(stderr, "Usage: fujinet-nio-baud [300..230400]\n");
        return RETURN_ERROR;
    }
    port = CreatePort(NULL, 0);
    if (port == NULL) return RETURN_FAIL;
    memset(&open_request, 0, sizeof(open_request));
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                   FUJINET_NIO_DEVICE_UNIT, &open_request, 0) != 0) {
        DeletePort(port);
        fprintf(stderr, "Cannot open %s\n", FUJINET_NIO_DEVICE_NAME);
        return RETURN_FAIL;
    }

    if (argc == 2) {
        baud = strtoul(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || baud < 300UL || baud > 230400UL) {
            fprintf(stderr, "Baud must be 300..230400\n");
            CloseDevice(&open_request);
            DeletePort(port);
            return RETURN_ERROR;
        }
        init_request(&req, port, FUJINET_NIO_CMD_SET_BAUD);
        req.fn_io.io_Device = open_request.io_Device;
        req.fn_io.io_Unit = open_request.io_Unit;
        write_le32(data, baud);
        req.fn_request_data = data;
        req.fn_request_length = sizeof(data);
        if (DoIO(&req.fn_io) != 0 || req.fn_nio_error != 0) {
            fprintf(stderr, "Cannot set baud (%ld/%u)\n", (long)req.fn_io.io_Error,
                    (unsigned)req.fn_nio_error);
            CloseDevice(&open_request);
            DeletePort(port);
            return RETURN_FAIL;
        }
    }

    init_request(&req, port, FUJINET_NIO_CMD_GET_BAUD);
    req.fn_io.io_Device = open_request.io_Device;
    req.fn_io.io_Unit = open_request.io_Unit;
    req.fn_response_data = data;
    req.fn_response_capacity = sizeof(data);
    if (DoIO(&req.fn_io) != 0 || req.fn_nio_error != 0 ||
        req.fn_response_length != sizeof(data)) {
        fprintf(stderr, "Cannot read baud (%ld/%u)\n", (long)req.fn_io.io_Error,
                (unsigned)req.fn_nio_error);
        CloseDevice(&open_request);
        DeletePort(port);
        return RETURN_FAIL;
    }
    printf("FujiNet NIO baud: %lu\n", (unsigned long)read_le32(data));
    CloseDevice(&open_request);
    DeletePort(port);
    return RETURN_OK;
}
