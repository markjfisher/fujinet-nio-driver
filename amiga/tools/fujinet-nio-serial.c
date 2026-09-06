#include <exec/io.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet-nio.h"
#include "fujinet_nio_device.h"
#include "fujinet_nio_serial_config.h"

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

static int print_current(struct FujiNetNIORequest *req, struct MsgPort *port,
                         const struct IORequest *open_request)
{
    UBYTE payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    char name[FUJINET_NIO_SERIAL_NAME_MAX + 1];
    uint32_t unit;

    init_request(req, port, FUJINET_NIO_CMD_GET_SERIAL);
    req->fn_io.io_Device = open_request->io_Device;
    req->fn_io.io_Unit = open_request->io_Unit;
    req->fn_response_data = payload;
    req->fn_response_capacity = sizeof(payload);
    if (DoIO(&req->fn_io) != 0 || req->fn_nio_error != 0 ||
        fujinet_nio_serial_decode(payload, req->fn_response_length, &unit,
                                  name, sizeof(name)) != FN_OK) {
        fprintf(stderr, "Cannot read serial device (%ld/%u)\n",
                (long)req->fn_io.io_Error, (unsigned)req->fn_nio_error);
        return RETURN_FAIL;
    }
    printf("FujiNet NIO serial: %s unit %lu\n", name, (unsigned long)unit);
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IORequest open_request;
    struct FujiNetNIORequest req;
    UBYTE payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    uint16_t payload_len;
    uint32_t unit = 0;
    char *end;
    int status;

    if (argc > 3) {
        fprintf(stderr, "Usage: fujinet-nio-serial [device [unit]]\n");
        return RETURN_ERROR;
    }
    if (argc >= 2 && !fujinet_nio_serial_name_ok(argv[1])) {
        fprintf(stderr,
                "Device name must be 1..%d printable characters with no :/\\\n",
                FUJINET_NIO_SERIAL_NAME_MAX);
        return RETURN_ERROR;
    }
    if (argc == 3) {
        unit = (uint32_t)strtoul(argv[2], &end, 10);
        if (*argv[2] == '\0' || *end != '\0' || unit > 255UL) {
            fprintf(stderr, "Unit must be 0..255\n");
            return RETURN_ERROR;
        }
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

    if (argc >= 2) {
        if (fujinet_nio_serial_encode(payload, sizeof(payload), &payload_len,
                                      unit, argv[1]) != FN_OK) {
            fprintf(stderr, "Cannot encode serial device\n");
            CloseDevice(&open_request);
            DeletePort(port);
            return RETURN_ERROR;
        }
        init_request(&req, port, FUJINET_NIO_CMD_SET_SERIAL);
        req.fn_io.io_Device = open_request.io_Device;
        req.fn_io.io_Unit = open_request.io_Unit;
        req.fn_request_data = payload;
        req.fn_request_length = payload_len;
        if (DoIO(&req.fn_io) != 0 || req.fn_nio_error != 0) {
            fprintf(stderr, "Cannot set serial device (%ld/%u)\n",
                    (long)req.fn_io.io_Error, (unsigned)req.fn_nio_error);
            CloseDevice(&open_request);
            DeletePort(port);
            return RETURN_FAIL;
        }
    }

    status = print_current(&req, port, &open_request);
    CloseDevice(&open_request);
    DeletePort(port);
    return status;
}
