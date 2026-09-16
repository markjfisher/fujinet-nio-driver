#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/types.h>
#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

#include "fujinet_nio_device.h"
#include "fujinet_nio_endian.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

/* clib2. Default Shell STACK is 4096. Keep packet buffers in BSS and
 * never WaitIO an OpenDevice-only IORequest. */
long __stack = 8192;

static uint8_t request[FN_HEADER_SIZE];
static uint8_t response[64];
static char identity[32];

static uint8_t packet_checksum(const uint8_t *packet, uint16_t length)
{
    uint16_t sum = 0;
    uint16_t i;

    for (i = 0; i < length; ++i) {
        if (i == FN_CHECKSUM_OFFSET)
            continue;
        sum = (uint16_t)(sum + packet[i]);
        sum = (uint16_t)((sum >> 8) + (sum & 0xFFu));
    }
    return (uint8_t)sum;
}

static void build_clock_get(void)
{
    memset(request, 0, sizeof(request));
    request[0] = FN_DEVICE_CLOCK;
    request[1] = FN_CMD_CLOCK_GET;
    fujinet_nio_put_le16(request + 2, FN_HEADER_SIZE);
    request[5] = 0;
    request[FN_CHECKSUM_OFFSET] = packet_checksum(request, FN_HEADER_SIZE);
}

static int read_identity(void)
{
    BPTR fh;
    LONG got;

    memset(identity, 0, sizeof(identity));
    fh = Open((CONST_STRPTR)"NATIVE:IDENTITY", MODE_OLDFILE);
    if (fh == 0)
        return -1;
    got = Read(fh, identity, sizeof(identity) - 1U);
    Close(fh);
    if (got <= 0)
        return -1;
    if (got >= 1 && identity[got - 1] == '\n')
        identity[got - 1] = '\0';
    return 0;
}

static void fill_req(struct FujiNetNIORequest *req, struct MsgPort *port,
                     UWORD command, const uint8_t *req_data, UWORD req_len,
                     uint8_t *resp_data, UWORD resp_cap)
{
    struct Device *device = req->fn_io.io_Device;
    struct Unit *unit = req->fn_io.io_Unit;

    memset(req, 0, sizeof(*req));
    req->fn_io.io_Device = device;
    req->fn_io.io_Unit = unit;
    req->fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->fn_io.io_Message.mn_ReplyPort = port;
    req->fn_io.io_Message.mn_Length = sizeof(*req);
    req->fn_io.io_Command = command;
    req->fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
    req->fn_request_data = req_data;
    req->fn_request_length = req_len;
    req->fn_response_data = resp_data;
    req->fn_response_capacity = resp_cap;
}

int main(void)
{
    struct MsgPort *port;
    struct FujiNetNIORequest open_req;
    struct FujiNetNIORequest cmd;
    LONG open_rc;
    LONG io_rc;
    UBYTE set_baud_nio;
    UBYTE baud[4];

    if (read_identity() != 0) {
        printf("IDENTITY=missing\n");
        return RETURN_FAIL;
    }
    printf("IDENTITY=%s\n", identity);
    fflush(stdout);
    if (strcmp(identity, "native-test") != 0) {
        printf("FAIL identity\n");
        return RETURN_FAIL;
    }

    port = CreatePort(NULL, 0);
    if (port == NULL)
        return RETURN_FAIL;
    memset(&open_req, 0, sizeof(open_req));
    open_req.fn_io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    open_req.fn_io.io_Message.mn_ReplyPort = port;
    open_req.fn_io.io_Message.mn_Length = sizeof(open_req);
    open_rc = OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME,
                         FUJINET_NIO_DEVICE_UNIT, &open_req.fn_io, 0);
    if (open_rc != 0) {
        printf("OPEN io=%ld\n", (long)open_rc);
        DeletePort(port);
        return RETURN_FAIL;
    }

    fujinet_nio_put_le32(baud, 19200);
    fill_req(&cmd, port, FUJINET_NIO_CMD_SET_BAUD, baud, 4, NULL, 0);
    cmd.fn_io.io_Device = open_req.fn_io.io_Device;
    cmd.fn_io.io_Unit = open_req.fn_io.io_Unit;
    io_rc = DoIO(&cmd.fn_io);
    set_baud_nio = cmd.fn_nio_error;
    printf("SET_BAUD io=%ld nio=%u\n", (long)io_rc, (unsigned)set_baud_nio);
    fflush(stdout);

    build_clock_get();
    fill_req(&cmd, port, FUJINET_NIO_CMD_EXCHANGE, request, FN_HEADER_SIZE,
             response, sizeof(response));
    cmd.fn_io.io_Device = open_req.fn_io.io_Device;
    cmd.fn_io.io_Unit = open_req.fn_io.io_Unit;
    io_rc = DoIO(&cmd.fn_io);
    printf("EXCHANGE io=%ld nio=%u\n", (long)io_rc, (unsigned)cmd.fn_nio_error);
    fflush(stdout);
    if (io_rc == 0 && cmd.fn_nio_error == FN_OK &&
        cmd.fn_response_length >= FN_HEADER_SIZE) {
        printf("CLOCK cmd=%u len=%u status=%u\n",
               (unsigned)response[1], (unsigned)cmd.fn_response_length,
               (unsigned)(cmd.fn_response_length > FN_HEADER_SIZE
                              ? response[FN_HEADER_SIZE]
                              : 0));
    }

    CloseDevice(&open_req.fn_io);
    DeletePort(port);

    if (set_baud_nio == FN_ERR_UNSUPPORTED &&
        io_rc == 0 && cmd.fn_nio_error == FN_OK &&
        cmd.fn_response_length >= FN_HEADER_SIZE &&
        response[0] == FN_DEVICE_CLOCK &&
        response[1] == FN_CMD_CLOCK_GET) {
        BPTR complete = Open((CONST_STRPTR)"NATIVE:complete", MODE_NEWFILE);
        if (complete == 0 || Write(complete, "PASS\n", 5) != 5) {
            if (complete != 0)
                Close(complete);
            printf("FAIL complete\n");
            return RETURN_FAIL;
        }
        Close(complete);
        printf("PASS native-test-clock\n");
        return RETURN_OK;
    }
    printf("FAIL native-test-clock\n");
    return RETURN_FAIL;
}
