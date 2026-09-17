#define _POSIX_C_SOURCE 200809L
#include "fujinet_nio_exchange_disk_adapter.h"
#include "fn_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Exec boundary double only. The production workflow, adapter, INFO codec and
 * checksum parser are linked unchanged below. */
static struct ordinary_disk_context context;
static unsigned writes, mounts, reads, flushes;
static int remote_exec, remote_fn, bad_slot, service_status, short_geometry;
static int clear_error, query_error, data_error;
static uint8_t media[512];
static UWORD last_command;

LONG DoIO(struct IORequest *request)
{
    struct IOStdReq *io = (struct IOStdReq *)request;
    request->io_Error = 0;
    if (request == &context.nio.fn_io) {
        struct FujiNetNIORequest *nio = &context.nio;
        uint8_t reply[20] = {0xfc, 5, 20, 0, 0, 1, 0,
                            1, 0, 0, 0, 8, 4, 0, 2, 0xe0, 6, 0, 0, 0};
        assert(nio->fn_struct_size == sizeof(*nio));
        assert(nio->fn_request_length == 8 && nio->fn_request_data[0] == 0xfc);
        assert(nio->fn_request_data[1] == 5 && nio->fn_request_data[7] == 8);
        assert(nio->fn_flags == 0 && nio->fn_pad[1] == 0);
        assert(nio->fn_io.io_Device == context.nio_open.io_Device);
        assert(nio->fn_io.io_Unit == context.nio_open.io_Unit);
        request->io_Error = (BYTE)remote_exec;
        nio->fn_nio_error = (UBYTE)remote_fn;
        if (remote_exec || remote_fn) {
            nio->fn_pad[1] = 4; nio->fn_pad[2] = 17;
            nio->fn_flags = (31U << 8) | 23U;
            return request->io_Error;
        }
        reply[6] = (uint8_t)service_status;
        if (bad_slot) reply[11] = 7;
        reply[4] = fn_calc_packet_checksum(reply, sizeof(reply));
        memcpy(nio->fn_response_data, reply, sizeof(reply));
        nio->fn_response_length = sizeof(reply);
        return 0;
    }
    io->io_Actual = 0;
    switch (request->io_Command) {
    case FUJINET_DISK_CMD_TRACE_CLEAR:
        request->io_Error = (BYTE)clear_error; io->io_Actual = clear_error ? 73 : 0; break;
    case TD_CHANGESTATE: io->io_Actual = 1; break;
    case FUJINET_DISK_CMD_MOUNT_WRITABLE:
        assert(strcmp(io->io_Data, "host:/fixture.adf") == 0); ++mounts; break;
    case TD_GETGEOMETRY: {
        struct DriveGeometry *g = io->io_Data;
        g->dg_SectorSize = 512; g->dg_TotalSectors = 1760;
        io->io_Actual = short_geometry ? sizeof(*g) - 1 : sizeof(*g); break;
    }
    case TD_CHANGENUM: io->io_Actual = 9; break;
    case ETD_WRITE:
    case ETD_READ:
        last_command = request->io_Command;
        assert(context.disk.iotd_Count == 9 && !context.disk.iotd_SecLabel);
        assert(io->io_Offset == 17 * 512 && io->io_Length == 512);
        if (request->io_Command == ETD_WRITE) {
            ++writes; memcpy(media, io->io_Data, 512);
        } else { ++reads; memcpy(io->io_Data, media, 512); }
        request->io_Error = (BYTE)data_error;
        io->io_Actual = data_error ? 19 : 512; break;
    case CMD_UPDATE: ++flushes; break;
    case FUJINET_DISK_CMD_TRACE: {
        struct fujinet_disk_trace *t = io->io_Data;
        request->io_Error = (BYTE)query_error;
        io->io_Actual = query_error ? 37 : sizeof(*t);
        if (query_error) break;
        memset(t, 0, sizeof(*t));
        t->count = 1; t->commands[0] = last_command;
        t->exchange_attempts[0] = 1;
        if (data_error) {
            t->exchange_results[0][0] = 6;
            t->exchange_causes[0][0] = 17;
            t->exchange_native_errors[0][0] = 23;
            t->exchange_statuses[0][0] = 31;
        }
        break;
    }
    default: assert(!"unexpected operation");
    }
    return request->io_Error;
}

static char output[8192];
static struct fn_exchange_disk_result result;
static struct fn_nio_exchange_opts opts;
static void reset(void)
{
    memset(&context, 0, sizeof(context));
    writes = mounts = reads = flushes = 0;
    remote_exec = remote_fn = bad_slot = service_status = short_geometry = 0;
    clear_error = query_error = data_error = 0;
    context.opts = &opts;
    context.nio_open.io_Device = (struct Device *)&context;
    context.nio_open.io_Unit = (struct Unit *)&opts;
    fn_disk_context_init(&context.codec, fn_exchange_disk_adapter_exchange, &context);
}
static int run(void)
{
    FILE *capture = tmpfile();
    int saved, status;
    size_t length;
    assert(capture);
    fflush(stdout); saved = dup(STDOUT_FILENO); assert(saved >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    status = fn_exchange_disk_run(&opts, fn_exchange_disk_adapter_io, &context,
                                 context.write_buffer, context.read_buffer, &result);
    fflush(stdout); assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved);
    rewind(capture); length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = 0; fclose(capture);
    return status;
}
int main(void)
{
    char *argv[] = {"tool", "--type", "disk-write", "--backend", "warm",
        "--slot", "8", "--lba", "17", "--fixture-uri", "host:/fixture.adf",
        "--disposable-fixture", "--write-intent", "--trials", "3"};
    assert(fn_nio_exchange_opts_parse(15, argv, &opts) == 0);
    reset(); assert(run() == 0 && writes == 3 && reads == 3 && flushes == 3);
    assert(strstr(output, "trial=3 checksum_fnv1a32="));
    reset(); remote_exec = -7; remote_fn = 6;
    assert(run() != 0 && !writes && !mounts);
    assert(strstr(output, "io_Error=-7 nio=6 result=4 cause=17 native=23 status=31"));
    reset(); remote_fn = 6;
    assert(run() != 0 && !writes && !mounts);
    assert(strstr(output, "io_Error=0 nio=6 result=4 cause=17 native=23 status=31"));
    reset(); service_status = 2;
    assert(run() != 0 && !writes && !mounts);
    assert(strstr(output, "io_Error=0 nio=0 result=0 cause=0 native=0 status=0"));
    assert(strstr(output, "codec_result=2"));
    reset(); bad_slot = 1;
    assert(run() != 0 && !writes && !mounts && !strcmp(result.failure, "remote-info"));
    reset(); short_geometry = 1;
    assert(run() != 0 && !writes && mounts == 1 && result.fixture_mounted);
    reset(); clear_error = -8;
    assert(run() != 0 && !writes && !mounts);
    assert(strstr(output, "trace_clear io_Error=-8 io_Actual=73"));
    reset(); data_error = -6;
    assert(run() != 0 && writes == 1 && !reads && !flushes);
    assert(strstr(output, "io_Error=-6 io_Actual=19 trial=1"));
    assert(strstr(output, "result=6 cause=17 native=23 status=31 trial=1"));
    reset(); data_error = -6; query_error = -9;
    assert(run() != 0 && writes == 1 && !reads && !flushes);
    assert(strstr(output, "trace_query io_Error=-9 io_Actual=37 data_io_Error=-6 data_io_Actual=19"));
    reset(); query_error = -9;
    assert(run() != 0 && writes == 1 && !reads && !flushes);
    assert(strstr(output, "data_io_Error=0 data_io_Actual=512"));
    puts("ordinary disk actual adapter tests passed");
    return 0;
}
