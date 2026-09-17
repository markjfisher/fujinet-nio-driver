#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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
static pid_t peer;
static const char *self_path;

static int write_file(const char *dir, const char *name, const void *data,
                      size_t size)
{
    char path[300], tmp[310];
    FILE *fp;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.peer-tmp", path);
    fp = fopen(tmp, "wb");
    if (fp == NULL)
        return -1;
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) return -1;
    return rename(tmp, path);
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
    if (peer > 0) { kill(peer, SIGTERM); waitpid(peer, NULL, 0); peer = 0; }
    remove_path(temp_dir, "CHALLENGE");
    remove_path(temp_dir, "ACK");
    remove_path(temp_dir, "BARRIER");
    remove_path(temp_dir, "AMBIGUOUS");
    remove_path(temp_dir, "RECOVER");
    remove_path(temp_dir, FN_DIRECTORY_IDENTITY_NAME);
    remove_path(temp_dir, FN_DIRECTORY_TO_HOST_NAME);
    remove_path(temp_dir, FN_DIRECTORY_TO_GUEST_NAME);
    remove_path(temp_dir, "to-host.pkt.tmp");
    remove_path(temp_dir, "to-guest.pkt.tmp");
    (void)rmdir(temp_dir);
}

static void write_identity(void)
{
    const char initial[] = "00000000000000000000000000000000\n";
    CHECK("initial challenge", write_file(temp_dir, "CHALLENGE", initial, 33) == 0);
    peer = fork();
    CHECK("fork independent peer", peer >= 0);
    if (peer == 0) {
        unsigned generation = 1;
        char request[33], next[34], barrier[41];
        memcpy(next, initial, 33);
        next[33] = '\0';
        for (;;) {
            snprintf(barrier, sizeof(barrier), "BARRIER.%.32s", next);
            if (read_file(temp_dir, barrier, request, 33) == 0) {
                remove_path(temp_dir, barrier);
                remove_path(temp_dir, FN_DIRECTORY_TO_HOST_NAME);
                remove_path(temp_dir, FN_DIRECTORY_TO_GUEST_NAME);
                snprintf(next, sizeof(next), "%032x\n", generation++);
                write_file(temp_dir, "CHALLENGE", next, 33);
                write_file(temp_dir, "ACK", request, 33);
            }
            usleep(1000);
        }
    }

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
    CHECK("ambiguity persists", file_exists_at(temp_dir, "AMBIGUOUS"));
    backend_close();
    CHECK("automatic reopen cannot recover", backend_open() == FN_ERR_TRANSPORT);
    CHECK("automatic reopen retains ambiguity", file_exists_at(temp_dir, "AMBIGUOUS"));
    {
        pid_t fresh = fork();
        int exit_status = -1;
        CHECK("fork fresh adapter process", fresh >= 0);
        if (fresh == 0) {
            execl(self_path, self_path, "--expect-quarantine", (char *)NULL);
            _exit(90);
        }
        if (fresh > 0) {
            CHECK("wait fresh adapter", waitpid(fresh, &exit_status, 0) == fresh);
            CHECK("reload cannot erase quarantine", WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0);
        }
    }
    {
        char token[33];
        CHECK("read recovery challenge", read_file(temp_dir, "CHALLENGE", token, 33) == 0);
        CHECK("explicit operator authorization", write_file(temp_dir, "RECOVER", token, 33) == 0);
        CHECK("independent barrier permits explicit recovery", backend_open() == FN_OK);
        CHECK("authorization consumed", !file_exists_at(temp_dir, "RECOVER"));
        CHECK("proven recovery clears ambiguity", !file_exists_at(temp_dir, "AMBIGUOUS"));
    }
    CHECK("timeout discarded to-host",
          !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("timeout discarded to-host tmp",
          !file_exists_at(temp_dir, "to-host.pkt.tmp"));
    backend_close();
    rmdir_temp();
}

static void test_late_reply_cannot_complete_next_call(void)
{
    uint8_t request[FN_HEADER_SIZE], response[16], stale[8] = {0x45, 1, 8, 0, 0, 1, 0, 0xa1};
    uint16_t length, status;
    uint8_t detail, native;
    char token[33];
    pid_t delivery;
    int exit_status = -1;
    unsigned i;
    make_temp_dir();
    write_identity();
    CHECK("open before late reply", backend_open() == FN_OK);
    make_clock(request);
    CHECK("unknown first call", backend_exchange(request, sizeof(request), response,
        sizeof(response), &length, &detail, &native, &status) == FN_ERR_TRANSPORT);
    backend_close();
    CHECK("reopen blocked", backend_open() == FN_ERR_TRANSPORT);
    seal(stale, sizeof(stale));
    CHECK("independent late same-command response", write_file(temp_dir, FN_DIRECTORY_TO_GUEST_NAME, stale, sizeof(stale)) == 0);
    memset(response, 0x5a, sizeof(response));
    CHECK("late response cannot satisfy later call", backend_exchange(request, sizeof(request), response,
        sizeof(response), &length, &detail, &native, &status) == FN_ERR_TRANSPORT);
    CHECK("blocked response length zero", length == 0);
    for (i = 0; i < sizeof(response); ++i) CHECK("blocked buffer unchanged", response[i] == 0x5a);
    CHECK("blocked caller did not send", !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    CHECK("fresh operator challenge", read_file(temp_dir, "CHALLENGE", token, 33) == 0);
    CHECK("fresh operator consent", write_file(temp_dir, "RECOVER", token, 33) == 0);
    CHECK("barrier recovery", backend_open() == FN_OK);
    CHECK("old reply independently drained", !file_exists_at(temp_dir, FN_DIRECTORY_TO_GUEST_NAME));
    delivery = fork();
    CHECK("fork new delivery", delivery >= 0);
    if (delivery == 0) {
        for (i = 0; i < 2000; ++i) {
            if (file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME)) break;
            usleep(1000);
        }
        if (i == 2000) _exit(2);
        remove_path(temp_dir, FN_DIRECTORY_TO_HOST_NAME);
        stale[7] = 0xb2;
        seal(stale, sizeof(stale));
        _exit(write_file(temp_dir, FN_DIRECTORY_TO_GUEST_NAME, stale, sizeof(stale)) != 0);
    }
    CHECK("fresh exchange after proof", backend_exchange(request, sizeof(request), response,
        sizeof(response), &length, &detail, &native, &status) == FN_OK);
    CHECK("fresh response not stale", length == 8 && response[7] == 0xb2);
    if (delivery > 0) {
        CHECK("delivery reaped", waitpid(delivery, &exit_status, 0) == delivery);
        CHECK("delivery completed", WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0);
    }
    backend_close();
    rmdir_temp();
}

static void test_interrupted_recovery(void)
{
    char token[33], next[33];
    unsigned poll;
    int stopped;
    make_temp_dir();
    write_identity();
    CHECK("initial barrier", backend_open() == FN_OK);
    backend_close();
    CHECK("current challenge", read_file(temp_dir, "CHALLENGE", token, 33) == 0);
    CHECK("persist ambiguity", write_file(temp_dir, "AMBIGUOUS", token, 33) == 0);
    CHECK("stale permission", write_file(temp_dir, "RECOVER", "ffffffffffffffffffffffffffffffff\n", 33) == 0);
    CHECK("stale permission rejected", backend_open() == FN_ERR_TRANSPORT);
    CHECK("stop independent peer", kill(peer, SIGSTOP) == 0);
    CHECK("peer stopped", waitpid(peer, &stopped, WUNTRACED) == peer && WIFSTOPPED(stopped));
    CHECK("authorize current challenge", write_file(temp_dir, "RECOVER", token, 33) == 0);
    CHECK("missing ACK cannot recover", backend_open() == FN_ERR_TRANSPORT);
    CHECK("permission consumed on interruption", !file_exists_at(temp_dir, "RECOVER"));
    CHECK("interruption retains marker", file_exists_at(temp_dir, "AMBIGUOUS"));
    CHECK("resume independent peer", kill(peer, SIGCONT) == 0);
    for (poll = 0; poll < 2000; ++poll) {
        if (read_file(temp_dir, "CHALLENGE", next, 33) == 0 && memcmp(next, token, 33)) break;
        usleep(1000);
    }
    CHECK("late barrier rotates challenge", poll < 2000);
    CHECK("late ACK alone cannot recover", backend_open() == FN_ERR_TRANSPORT);
    CHECK("old consent still stale", write_file(temp_dir, "RECOVER", token, 33) == 0);
    CHECK("stale consent with late ACK rejected", backend_open() == FN_ERR_TRANSPORT);
    CHECK("new explicit consent", write_file(temp_dir, "RECOVER", next, 33) == 0);
    CHECK("fresh barrier recovers", backend_open() == FN_OK);
    CHECK("marker cleared only after proof", !file_exists_at(temp_dir, "AMBIGUOUS"));
    backend_close();
    rmdir_temp();
}

static void test_unreadable_marker(void)
{
    char path[300];
    make_temp_dir();
    write_identity();
    snprintf(path, sizeof(path), "%s/AMBIGUOUS", temp_dir);
    CHECK("dangling safety marker", symlink("missing-target", path) == 0);
    CHECK("non-readable safety marker is not absence", backend_open() == FN_ERR_TRANSPORT);
    CHECK("no packet with unreadable marker", !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    backend_close();
    rmdir_temp();
}

static void test_early_consent_cannot_recover_later_failure(void)
{
    uint8_t request[FN_HEADER_SIZE], response[16], detail, native;
    uint16_t length, status;
    char token[33];
    make_temp_dir(); write_identity();
    CHECK("open healthy", backend_open() == FN_OK);
    CHECK("healthy challenge", read_file(temp_dir, "CHALLENGE", token, 33) == 0);
    CHECK("premature permit", write_file(temp_dir, "RECOVER", token, 33) == 0);
    make_clock(request);
    CHECK("later unknown completion", backend_exchange(request, sizeof(request), response,
        sizeof(response), &length, &detail, &native, &status) == FN_ERR_TRANSPORT);
    CHECK("healthy traffic retired permit", !file_exists_at(temp_dir, "RECOVER"));
    backend_close();
    CHECK("early consent cannot authorize replay", backend_open() == FN_ERR_TRANSPORT);
    CHECK("ambiguity remains", file_exists_at(temp_dir, "AMBIGUOUS"));
    backend_close(); rmdir_temp();
}

static void test_completed_reply_cleanup_failure(void)
{
    uint8_t request[FN_HEADER_SIZE], response[16], detail, native;
    uint8_t expected[8] = {0x45, 1, 8, 0, 0, 1, 0, 0xc3};
    uint16_t length, status;
    pid_t delivery;
    int exit_status;
    unsigned i;
    char marker[300];
    make_temp_dir(); write_identity();
    CHECK("open cleanup failure", backend_open() == FN_OK);
    make_clock(request); seal(expected, sizeof(expected));
    snprintf(marker, sizeof(marker), "%s/AMBIGUOUS.tmp", temp_dir);
    delivery = fork();
    CHECK("fork cleanup fault peer", delivery >= 0);
    if (delivery == 0) {
        for (i = 0; i < 2000; ++i) {
            if (file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME)) break;
            usleep(1000);
        }
        if (i == 2000 || mkdir(marker, 0700) != 0) _exit(2);
        remove_path(temp_dir, FN_DIRECTORY_TO_HOST_NAME);
        _exit(write_file(temp_dir, FN_DIRECTORY_TO_GUEST_NAME, expected, sizeof(expected)) != 0);
    }
    CHECK("validated completion survives cleanup failure", backend_exchange(request, sizeof(request),
        response, sizeof(response), &length, &detail, &native, &status) == FN_OK);
    CHECK("exact successful reply retained", length == sizeof(expected) && !memcmp(response, expected, sizeof(expected)));
    if (delivery > 0) {
        CHECK("reap cleanup fault peer", waitpid(delivery, &exit_status, 0) == delivery);
        CHECK("cleanup fault delivered", WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0);
    }
    memset(response, 0x5a, sizeof(response));
    CHECK("future calls blocked", backend_exchange(request, sizeof(request), response,
        sizeof(response), &length, &detail, &native, &status) == FN_ERR_TRANSPORT);
    CHECK("blocked call zero length", length == 0);
    for (i = 0; i < sizeof(response); ++i) CHECK("blocked buffer preserved", response[i] == 0x5a);
    CHECK("no replay after known completion", !file_exists_at(temp_dir, FN_DIRECTORY_TO_HOST_NAME));
    backend_close();
    CHECK("cleanup quarantine survives reopen", backend_open() == FN_ERR_TRANSPORT);
    backend_close();
    CHECK("remove injected obstruction", rmdir(marker) == 0);
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

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--expect-quarantine") == 0)
        return backend_open() == FN_ERR_TRANSPORT ? 0 : 1;
    self_path = argv[0];
    test_identity_missing();
    test_identity_wrong();
    test_unsupported_controls();
    test_stale_record();
    test_backpressure();
    test_oversized();
    test_missing_peer();
    test_timeout_discards_to_host();
    test_teardown();
    test_late_reply_cannot_complete_next_call();
    test_interrupted_recovery();
    test_unreadable_marker();
    test_early_consent_cannot_recover_later_failure();
    test_completed_reply_cleanup_failure();
    if (failures != 0) {
        fprintf(stderr, "%u directory-backend checks failed\n", failures);
        return 1;
    }
    puts("PASS test_fujinet_nio_directory_backend");
    return 0;
}
