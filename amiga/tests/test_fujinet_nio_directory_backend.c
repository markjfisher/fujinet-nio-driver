#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <exec/errors.h>
#include <exec/io.h>

#include "fujinet_nio_backend.h"
#include "fujinet_nio_device.h"
#include "fujinet_nio_directory_backend.h"
#include "fujinet_nio_serial_config.h"
#include "fujinet_nio_packet_backend.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

void fujinet_nio_native_test_reset(void);
void fujinet_nio_native_test_set_backend(
    uint8_t (*)(void), void (*)(void),
    uint8_t (*)(const uint8_t *, uint16_t, uint8_t *, uint16_t, uint16_t *,
                uint8_t *, uint8_t *, uint16_t *),
    uint8_t (*)(uint32_t), uint32_t (*)(void),
    uint8_t (*)(uint32_t, const char *),
    void (*)(uint32_t *, char *, uint16_t));
struct Device *fujinet_nio_native_test_open(struct IORequest *, ULONG);
void fujinet_nio_native_test_begin_io(struct IORequest *);
void fujinet_nio_native_test_worker_step(void);
BPTR fujinet_nio_native_test_close(struct IORequest *);
BPTR fujinet_nio_native_test_expunge(void);

static unsigned failures;
static unsigned replies;
static struct Message *reply_log[8];

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

void Disable(void) {}
void Enable(void) {}

void ReplyMsg(struct Message *message)
{
    if (replies < 8)
        reply_log[replies] = message;
    ++replies;
}

APTR AllocMem(ULONG bytes, ULONG flags)
{
    (void)flags;
    return calloc(1, bytes);
}

void FreeMem(APTR memory, ULONG bytes)
{
    (void)bytes;
    free(memory);
}

static char temp_dir[256];

static int write_file(const char *dir, const char *name, const void *data,
                      size_t size)
{
    char path[300];
    FILE *fp;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    fp = fopen(path, "wb");
    if (fp == NULL)
        return -1;
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int file_exists_at(const char *dir, const char *name)
{
    char path[300];
    struct stat st;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return 0;
    return stat(path, &st) == 0;
}

static int read_file(const char *dir, const char *name, void *data,
                     size_t size)
{
    char path[300];
    FILE *fp;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;
    if (fread(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void make_temp_dir(void)
{
    char tmpl[] = "/tmp/fn-dir-be-XXXXXX";
    char *made;

    made = mkdtemp(tmpl);
    CHECK("mkdtemp", made != NULL);
    strncpy(temp_dir, made != NULL ? made : "/tmp/fn-dir-be-missing",
            sizeof(temp_dir) - 1U);
    temp_dir[sizeof(temp_dir) - 1U] = '\0';
    CHECK("setenv FN_NATIVE_TEST_DIR",
          setenv("FN_NATIVE_TEST_DIR", temp_dir, 1) == 0);
}

static void remove_path(const char *dir, const char *name)
{
    char path[300];

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return;
    (void)unlink(path);
}

static void rmdir_temp(void)
{
    remove_path(temp_dir, FN_DIRECTORY_IDENTITY_NAME);
    remove_path(temp_dir, FN_DIRECTORY_TO_HOST_NAME);
    remove_path(temp_dir, FN_DIRECTORY_TO_GUEST_NAME);
    remove_path(temp_dir, "to-host.pkt.tmp");
    remove_path(temp_dir, "to-guest.pkt.tmp");
    (void)rmdir(temp_dir);
}

static void write_identity(void)
{
    CHECK("write IDENTITY",
          write_file(temp_dir, FN_DIRECTORY_IDENTITY_NAME,
                     FN_DIRECTORY_IDENTITY_BODY,
                     sizeof(FN_DIRECTORY_IDENTITY_BODY) - 1U) == 0);
}

static void seal(uint8_t *packet, uint16_t length)
{
    packet[2] = (uint8_t)length;
    packet[3] = (uint8_t)(length >> 8);
    packet[4] = 0;
    packet[4] = fn_calc_packet_checksum(packet, length);
}

static void make_clock(uint8_t *packet)
{
    memset(packet, 0, FN_HEADER_SIZE);
    packet[0] = FN_DEVICE_CLOCK;
    packet[1] = FN_CMD_CLOCK_GET;
    packet[5] = 0;
    seal(packet, FN_HEADER_SIZE);
}

static void test_identity_missing(void)
{
    make_temp_dir();
    CHECK("missing IDENTITY is not timeout",
          backend_open() == FN_ERR_INVALID);
    CHECK("missing IDENTITY wrote no to-host",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("missing IDENTITY wrote no to-guest",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_GUEST_NAME));
    backend_close();
    rmdir_temp();
}

static void test_identity_wrong(void)
{
    make_temp_dir();
    CHECK("write wrong IDENTITY",
          write_file(temp_dir, FN_DIRECTORY_IDENTITY_NAME, "serial\n", 7) == 0);
    CHECK("wrong IDENTITY is not timeout",
          backend_open() == FN_ERR_INVALID);
    CHECK("wrong IDENTITY wrote no to-host",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("wrong IDENTITY is not TRANSPORT",
          backend_open() != FN_ERR_TRANSPORT);
    backend_close();
    rmdir_temp();
}

static void test_unsupported_controls(void)
{
    struct FujiNetNIORequest req;
    UBYTE baud[4] = {0, 0, 0, 0};
    UBYTE serial_payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    uint16_t serial_len = 0;

    make_temp_dir();
    write_identity();
    CHECK("direct SET_BAUD unsupported",
          backend_set_baud(19200) == FN_ERR_UNSUPPORTED);
    CHECK("direct SET_SERIAL unsupported",
          backend_set_serial(0, "serial.device") == FN_ERR_UNSUPPORTED);

    replies = 0;
    fujinet_nio_native_test_reset();
    fujinet_nio_native_test_set_backend(backend_open, backend_close,
                                        backend_exchange,
                                        NULL, NULL, NULL, NULL);
    memset(&req, 0, sizeof(req));
    req.fn_io.io_Command = FUJINET_NIO_CMD_SET_BAUD;
    req.fn_struct_size = (UWORD)FUJINET_NIO_REQUEST_SIZE;
    req.fn_request_data = baud;
    req.fn_request_length = 4;
    CHECK("open for SET_BAUD",
          fujinet_nio_native_test_open(&req.fn_io, FUJINET_NIO_DEVICE_UNIT) !=
          NULL);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("SET_BAUD device unsupported",
          req.fn_nio_error == FN_ERR_UNSUPPORTED);

    replies = 0;
    memset(&req, 0, sizeof(req));
    req.fn_io.io_Command = FUJINET_NIO_CMD_GET_BAUD;
    req.fn_struct_size = (UWORD)FUJINET_NIO_REQUEST_SIZE;
    req.fn_response_data = baud;
    req.fn_response_capacity = 4;
    CHECK("open for GET_BAUD",
          fujinet_nio_native_test_open(&req.fn_io, FUJINET_NIO_DEVICE_UNIT) !=
          NULL);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("GET_BAUD device unsupported",
          req.fn_nio_error == FN_ERR_UNSUPPORTED);

    CHECK("encode SET_SERIAL payload",
          fujinet_nio_serial_encode(serial_payload, sizeof(serial_payload),
                                    &serial_len, 0, "serial.device") == FN_OK);
    replies = 0;
    memset(&req, 0, sizeof(req));
    req.fn_io.io_Command = FUJINET_NIO_CMD_SET_SERIAL;
    req.fn_struct_size = (UWORD)FUJINET_NIO_REQUEST_SIZE;
    req.fn_request_data = serial_payload;
    req.fn_request_length = serial_len;
    CHECK("open for SET_SERIAL",
          fujinet_nio_native_test_open(&req.fn_io, FUJINET_NIO_DEVICE_UNIT) !=
          NULL);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("SET_SERIAL device unsupported",
          req.fn_nio_error == FN_ERR_UNSUPPORTED);

    replies = 0;
    memset(&req, 0, sizeof(req));
    req.fn_io.io_Command = FUJINET_NIO_CMD_GET_SERIAL;
    req.fn_struct_size = (UWORD)FUJINET_NIO_REQUEST_SIZE;
    req.fn_response_data = serial_payload;
    req.fn_response_capacity = sizeof(serial_payload);
    CHECK("open for GET_SERIAL",
          fujinet_nio_native_test_open(&req.fn_io, FUJINET_NIO_DEVICE_UNIT) !=
          NULL);
    fujinet_nio_native_test_begin_io(&req.fn_io);
    fujinet_nio_native_test_worker_step();
    CHECK("GET_SERIAL device unsupported",
          req.fn_nio_error == FN_ERR_UNSUPPORTED);

    (void)fujinet_nio_native_test_close(&req.fn_io);
    (void)fujinet_nio_native_test_expunge();
    backend_close();
    rmdir_temp();
}

static void test_stale_record(void)
{
    uint8_t leftover[FN_HEADER_SIZE];
    uint8_t request[FN_HEADER_SIZE];
    uint8_t response[FN_HEADER_SIZE];
    uint16_t length = 99;
    uint8_t detail = 0xFF;
    uint8_t native_err = 0xFF;
    uint16_t native_status = 0xFFFF;

    make_temp_dir();
    write_identity();
    make_clock(leftover);
    leftover[1] = FN_CMD_CLOCK_GET_TZ;
    seal(leftover, FN_HEADER_SIZE);
    CHECK("plant stale to-guest",
          write_file(temp_dir, FN_DIRECTORY_TO_GUEST_NAME, leftover,
                     sizeof(leftover)) == 0);
    CHECK("open discards stale", backend_open() == FN_OK);
    CHECK("stale to-guest gone",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_GUEST_NAME));
    make_clock(request);
    CHECK("stale cannot complete EXCHANGE",
          backend_exchange(request, FN_HEADER_SIZE, response,
                           sizeof(response), &length, &detail, &native_err,
                           &native_status) == FN_ERR_TRANSPORT);
    CHECK("stale exchange length zero", length == 0);
    CHECK("stale exchange not timeout",
          backend_exchange(request, FN_HEADER_SIZE, response,
                           sizeof(response), &length, &detail, &native_err,
                           &native_status) != FN_ERR_TIMEOUT);
    backend_close();
    rmdir_temp();
}

static void test_backpressure(void)
{
    uint8_t existing[4] = {9, 8, 7, 6};
    uint8_t packet[FN_HEADER_SIZE];
    uint8_t after[4];

    make_temp_dir();
    write_identity();
    CHECK("plant to-host",
          write_file(temp_dir, FN_DIRECTORY_TO_HOST_NAME, existing,
                     sizeof(existing)) == 0);
    make_clock(packet);
    CHECK("send rejected on occupied to-host",
          fujinet_nio_directory_client_send(packet, FN_HEADER_SIZE) ==
          FN_DIR_BACKPRESSURE);
    CHECK("to-host not overwritten",
          read_file(temp_dir, FN_DIRECTORY_TO_HOST_NAME, after,
                    sizeof(after)) == 0);
    CHECK("to-host bytes unchanged", memcmp(existing, after, sizeof(after)) == 0);
    rmdir_temp();
}

static void test_oversized(void)
{
    uint8_t byte = 1;
    uint8_t *huge;
    uint8_t buffer[8];
    uint16_t length = 99;

    make_temp_dir();
    write_identity();
    CHECK("send oversized rejected",
          fujinet_nio_directory_client_send(&byte, 65536U) == FN_DIR_OVERSIZED);
    CHECK("oversized send wrote no prefix",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));

    huge = malloc(65536U);
    CHECK("alloc oversized record", huge != NULL);
    if (huge != NULL) {
        memset(huge, 0x5A, 65536U);
        CHECK("plant oversized to-guest",
              write_file(temp_dir, FN_DIRECTORY_TO_GUEST_NAME, huge,
                         65536U) == 0);
        CHECK("receive oversized",
              fujinet_nio_directory_client_receive(buffer, sizeof(buffer),
                                                   &length) == FN_DIR_OVERSIZED);
        CHECK("oversized consumed",
              !file_exists_at(temp_dir, FN_DIRECTORY_TO_GUEST_NAME));
        CHECK("oversized delivered no prefix", length == 0);
        free(huge);
    }
    rmdir_temp();
}

static void test_missing_peer(void)
{
    uint8_t packet[FN_HEADER_SIZE];

    CHECK("missing dir is TRANSPORT not timeout",
          setenv("FN_NATIVE_TEST_DIR", "/tmp/fn-dir-be-does-not-exist", 1) == 0);
    CHECK("missing dir open TRANSPORT", backend_open() == FN_ERR_TRANSPORT);
    CHECK("missing dir not timeout", backend_open() != FN_ERR_TIMEOUT);
    make_clock(packet);
    CHECK("missing dir send unavailable",
          fujinet_nio_directory_client_send(packet, FN_HEADER_SIZE) ==
          FN_DIR_UNAVAILABLE);
}

static void test_timeout_discards_to_host(void)
{
    uint8_t request[FN_HEADER_SIZE];
    uint8_t response[FN_HEADER_SIZE];
    uint16_t length = 99;
    uint8_t detail = 0xFF;
    uint8_t native_err = 0xFF;
    uint16_t native_status = 0xFFFF;

    make_temp_dir();
    write_identity();
    CHECK("open for timeout discard", backend_open() == FN_OK);
    make_clock(request);
    CHECK("timeout exchange is TRANSPORT",
          backend_exchange(request, FN_HEADER_SIZE, response,
                           sizeof(response), &length, &detail, &native_err,
                           &native_status) == FN_ERR_TRANSPORT);
    CHECK("timeout discarded to-host",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("timeout discarded to-host tmp",
          !file_exists_at(temp_dir, "to-host.pkt.tmp"));
    backend_close();
    rmdir_temp();
}

static void test_teardown(void)
{
    uint8_t packet[FN_HEADER_SIZE];

    make_temp_dir();
    write_identity();
    CHECK("open for teardown", backend_open() == FN_OK);
    make_clock(packet);
    CHECK("publish request",
          fujinet_nio_directory_client_send(packet, FN_HEADER_SIZE) ==
          FN_DIR_OK);
    CHECK("to-host present before close",
          file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    backend_close();
    CHECK("close discarded leftovers",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("close discarded guest leftovers",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_GUEST_NAME));
    CHECK("tmp leftovers inert",
          !file_exists_at(temp_dir, "to-host.pkt.tmp") &&
          !file_exists_at(temp_dir, "to-guest.pkt.tmp"));
    rmdir_temp();
}

int main(void)
{
    test_identity_missing();
    test_identity_wrong();
    test_unsupported_controls();
    test_stale_record();
    test_backpressure();
    test_oversized();
    test_missing_peer();
    test_timeout_discards_to_host();
    test_teardown();
    if (failures != 0) {
        fprintf(stderr, "%u directory-backend checks failed\n", failures);
        return 1;
    }
    puts("PASS test_fujinet_nio_directory_backend");
    return 0;
}
