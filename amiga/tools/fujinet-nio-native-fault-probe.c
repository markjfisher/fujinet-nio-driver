#include <dos/dos.h>
#include <exec/io.h>
#include <exec/types.h>
#include <devices/trackdisk.h>
#include "fujinet_disk_device.h"
#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>
#include "fujinet_nio_device.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"
#include "fn_raw.h"
#include "fn_platform.h"

long __stack = 16384;
static struct FujiNetNIORequest opening, calls[2];
static unsigned char packets[2][526], replies[2][64], payload[520], reply[600];

static int control(const char *name, const void *data, LONG length)
{
    BPTR file = Open((CONST_STRPTR)name, MODE_NEWFILE);
    int ok;
    if (!file) return 0;
    ok = Write(file, (APTR)data, length) == length && Flush(file);
    Close(file);
    return ok;
}

static struct IOExtTD disk_opening, disk_call;
static struct fujinet_disk_trace disk_trace;
static uint8_t sector[512];

static LONG disk_command(UWORD command, void *data, ULONG length, ULONG offset)
{
    struct IOStdReq *io = &disk_call.iotd_Req;
    io->io_Command = command; io->io_Flags = 0; io->io_Error = 0;
    io->io_Data = data; io->io_Length = length; io->io_Offset = offset;
    io->io_Actual = 0;
    return DoIO((struct IORequest *)io);
}

static int resident_fault(const char *mode)
{
    struct MsgPort *port = CreatePort(NULL, 0);
    const char *uri = "host:/resident.adf";
    char token[33];
    BPTR file;
    unsigned i;
    LONG failure;
    ULONG actual;
    int ok = 1;
    if (!port) return 0;
    disk_opening.iotd_Req.io_Message.mn_Length = sizeof(disk_opening);
    disk_opening.iotd_Req.io_Message.mn_ReplyPort = port;
    if (OpenDevice((CONST_STRPTR)FUJINET_DISK_DEVICE_NAME, 5,
                   (struct IORequest *)&disk_opening, 0)) {
        DeletePort(port); return 0;
    }
    disk_call.iotd_Req.io_Device = disk_opening.iotd_Req.io_Device;
    disk_call.iotd_Req.io_Unit = disk_opening.iotd_Req.io_Unit;
    disk_call.iotd_Req.io_Message.mn_Length = sizeof(disk_call);
    disk_call.iotd_Req.io_Message.mn_ReplyPort = port;
    if (disk_command(FUJINET_DISK_CMD_MOUNT_WRITABLE, (void *)uri, strlen(uri) + 1, 0)) ok = 0;
    if (disk_command(FUJINET_DISK_CMD_TRACE_CLEAR, NULL, 0, 0)) ok = 0;
    for (i = 0; i < sizeof(sector); ++i) sector[i] = (uint8_t)(i + 0x91);
    if (!control("NATIVE:FAULT", strcmp(mode, "hold") ? "drop\n" : "hold\n", 5)) ok = 0;
    failure = disk_command(CMD_WRITE, sector, sizeof(sector), 17 * 512UL);
    actual = disk_call.iotd_Req.io_Actual;
    if (!failure || actual != 0) ok = 0;
    if (disk_command(FUJINET_DISK_CMD_TRACE, &disk_trace, sizeof(disk_trace), 0)) ok = 0;
    if (disk_trace.count != 1 || disk_trace.exchange_attempts[0] != FUJINET_DISK_RETRY_ATTEMPTS) ok = 0;
    for (i = 0; i < FUJINET_DISK_RETRY_ATTEMPTS; ++i)
        if (disk_trace.exchange_results[0][i] != FN_ERR_TRANSPORT) ok = 0;
    printf("RESIDENT retries=%u error=%ld actual=%lu isolated=%d\n",
        (unsigned)disk_trace.exchange_attempts[0], (long)failure, (unsigned long)actual, ok);
    /* The resident's real bounded retry loop has now finished. */
    file = Open((CONST_STRPTR)"NATIVE:CHALLENGE", MODE_OLDFILE);
    if (!file) ok = 0;
    else { if (Read(file, token, 33) != 33) ok = 0; Close(file); }
    if (ok && !control("NATIVE:RECOVER", token, 33)) ok = 0;
    if (ok && disk_command(CMD_UPDATE, NULL, 0, 0)) ok = 0;
    memset(sector, 0, sizeof(sector));
    if (ok && disk_command(CMD_READ, sector, sizeof(sector), 17 * 512UL)) ok = 0;
    if (ok) for (i = 0; i < sizeof(sector); ++i)
        if (sector[i] != (uint8_t)(i + 0x91)) ok = 0;
    CloseDevice((struct IORequest *)&disk_opening);
    DeletePort(port);
    printf("RESIDENT recovered=%d\n", ok);
    return ok;
}

static void packet(unsigned index)
{
    unsigned i, sum = 0;
    unsigned char *p = packets[index];
    memset(p, 0, 526);
    p[0] = 0xfc; p[1] = 4; p[2] = 14; p[3] = 2;
    p[6] = 1; p[7] = 8; p[8] = 17; p[13] = 2;
    for (i = 14; i < 526; ++i) p[i] = (unsigned char)(i - 14 + 0x31 + index);
    for (i = 0; i < 526; ++i) { sum += p[i]; sum = (sum >> 8) + (sum & 255); }
    p[4] = (unsigned char)sum;
}

int main(int argc, char **argv)
{
    struct MsgPort *port = NULL;
    fn_raw_response_t result;
    const char *uri = "host:/fault.adf";
    static const uint8_t fresh_write_reply[11] = {1, 0, 0, 0, 8, 18, 0, 0, 0, 0, 2};
    char token[33];
    BPTR file;
    unsigned i, j, completions = 0;
    int ok = 1;
    unsigned char rc;
    if (argc != 2 || (strcmp(argv[1], "hold") && strcmp(argv[1], "drop"))) return 10;
    /* Real mount before fault injection. Every backing image is disposable. */
    memset(payload, 0, sizeof(payload));
    payload[0] = 1; payload[1] = 8; payload[6] = (unsigned char)strlen(uri);
    memcpy(payload + 8, uri, strlen(uri));
    rc = fn_raw_call(0xfc, 1, payload, (uint16_t)(8 + strlen(uri)), reply, sizeof(reply), &result);
    if (rc || result.status) { printf("FAIL mount %u\n", rc); return 20; }
    fn_transport_close();
    port = CreatePort(NULL, 0);
    if (!port) return 20;
    opening.fn_io.io_Message.mn_Length = sizeof(opening);
    opening.fn_io.io_Message.mn_ReplyPort = port;
    if (OpenDevice((CONST_STRPTR)FUJINET_NIO_DEVICE_NAME, 0, &opening.fn_io, 0)) {
        DeletePort(port); return 20;
    }
    if (!control("NATIVE:FAULT", strcmp(argv[1], "hold") ? "drop\n" : "hold\n", 5)) ok = 0;
    for (i = 0; i < 2; ++i) {
        packet(i);
        memset(replies[i], 0xa5 + i, sizeof(replies[i]));
        calls[i].fn_io.io_Device = opening.fn_io.io_Device;
        calls[i].fn_io.io_Unit = opening.fn_io.io_Unit;
        calls[i].fn_io.io_Message.mn_Length = sizeof(calls[i]);
        calls[i].fn_io.io_Message.mn_ReplyPort = port;
        calls[i].fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
        calls[i].fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
        calls[i].fn_request_data = packets[i];
        calls[i].fn_request_length = sizeof(packets[i]);
        calls[i].fn_response_data = replies[i];
        calls[i].fn_response_capacity = sizeof(replies[i]);
        SendIO(&calls[i].fn_io);
    }
    for (i = 0; i < 2; ++i) {
        WaitIO(&calls[i].fn_io); ++completions;
        if (calls[i].fn_nio_error != FN_ERR_TRANSPORT || calls[i].fn_response_length) ok = 0;
        for (j = 0; j < sizeof(replies[i]); ++j)
            if (replies[i][j] != 0xa5 + i) ok = 0;
    }
    if (GetMsg(port)) ok = 0;
    printf("QUEUED completions=%u isolated=%d\n", completions, ok);
    if (!strcmp(argv[1], "hold")) {
        BPTR late = 0;
        if (!control("NATIVE:RELEASE", "release\n", 8)) ok = 0;
        for (i = 0; i < 250; ++i) {
            late = Lock((CONST_STRPTR)"NATIVE:to-guest.pkt", ACCESS_READ);
            if (late) { UnLock(late); break; }
            Delay(1);
        }
        if (!late) ok = 0;
        printf("LATE actual-reply-visible=%d\n", late != 0);
    }
    /* Actual fn_raw_call retries through the same broker, still no new send. */
    if (fn_init() != FN_OK) ok = 0;
    rc = fn_raw_call(0xfc, 4, packets[1] + 6, 520, reply, sizeof(reply), &result);
    printf("RETRY rc=%u length=%u\n", rc, result.payload_length);
    if (rc != FN_ERR_TRANSPORT || result.payload_length) ok = 0;
    fn_transport_close();
    CloseDevice(&opening.fn_io);
    DeletePort(port);
    /* All submitted callers have completed. This isolated probe now acts as
     * the test operator, authorizing one fresh-challenge recovery. */
    file = Open((CONST_STRPTR)"NATIVE:CHALLENGE", MODE_OLDFILE);
    if (!file || Read(file, token, 33) != 33) return 20;
    Close(file);
    if (!control("NATIVE:RECOVER", token, 33)) return 20;
    if (fn_init() != FN_OK) return 20;
    payload[0] = 1; payload[1] = 8;
    rc = fn_raw_call(0xfc, 0x0e, payload, 2, reply, sizeof(reply), &result);
    if (rc || result.status) ok = 0;
    memset(payload, 0, 8); payload[0] = 1; payload[1] = 8; payload[2] = 17; payload[7] = 2;
    rc = fn_raw_call(0xfc, 3, payload, 8, reply, sizeof(reply), &result);
    if (rc || result.status || result.payload_length != 523) ok = 0;
    for (i = 0; i < 512; ++i) if (reply[11 + i] != (unsigned char)(i + 0x31)) ok = 0;
    printf("RECOVERY first-write-only=%d\n", ok);
    /* Same command as the held reply, but a distinct LBA and response body.
     * A stale WRITE(17) acknowledgment must not satisfy this fresh WRITE(18). */
    memset(payload, 0, sizeof(payload));
    payload[0] = 1; payload[1] = 8; payload[2] = 18; payload[7] = 2;
    for (i = 0; i < 512; ++i) payload[8 + i] = (unsigned char)(i + 0x71);
    rc = fn_raw_call(0xfc, 4, payload, 520, reply, sizeof(reply), &result);
    if (rc || result.status || result.payload_length != 11 ||
        memcmp(reply, fresh_write_reply, sizeof(fresh_write_reply))) ok = 0;
    payload[0] = 1; payload[1] = 8;
    rc = fn_raw_call(0xfc, 0x0e, payload, 2, reply, sizeof(reply), &result);
    if (rc || result.status) ok = 0;
    memset(payload, 0, 8); payload[0] = 1; payload[1] = 8; payload[2] = 18; payload[7] = 2;
    rc = fn_raw_call(0xfc, 3, payload, 8, reply, sizeof(reply), &result);
    if (rc || result.status || result.payload_length != 523) ok = 0;
    for (i = 0; i < 512; ++i) if (reply[11 + i] != (unsigned char)(i + 0x71)) ok = 0;
    fn_transport_close();
    printf("FRESH same-command-lba18=%d\n", ok);
    if (ok && !resident_fault(argv[1])) ok = 0;
    if (ok && control("NATIVE:complete", "PASS\n", 5)) { puts("PASS native-fault"); return 0; }
    puts("FAIL native-fault"); return 20;
}
