/* Real callers -> real Amiga transport -> real broker -> packet guard.
 * The peer deliberately does NOT reject sends while old work is pending. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/errors.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>
#include "fujinet_disk_driver.h"
#include "fujinet_nio_device.h"
#include "fujinet_nio_backend.h"
#include "fujinet_nio_packet_backend.h"
#include "fn_internal.h"
#include "fn_platform.h"
#include "fn_raw.h"

void fujinet_nio_native_test_reset(void);
void fujinet_nio_native_test_set_backend(uint8_t (*)(void), void (*)(void),
    uint8_t (*)(const uint8_t *, uint16_t, uint8_t *, uint16_t, uint16_t *,
               uint8_t *, uint8_t *, uint16_t *),
    uint8_t (*)(uint32_t), uint32_t (*)(void),
    uint8_t (*)(uint32_t, const char *), void (*)(uint32_t *, char *, uint16_t));
struct Device *fujinet_nio_native_test_open(struct IORequest *, ULONG);
BPTR fujinet_nio_native_test_close(struct IORequest *);
void fujinet_nio_native_test_begin_io(struct IORequest *);
LONG fujinet_nio_native_test_abort_io(struct IORequest *);
void fujinet_nio_native_test_worker_step(void);
BPTR fujinet_nio_native_test_expunge(void);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)
enum fault { GOOD, REJECT, UNAVAILABLE, DELIVERY, LOST, CORRUPT, OVERSIZE,
             TRUNCATED, WRONG_DEVICE, WRONG_COMMAND, BAD_DESCRIPTOR,
             BAD_LENGTH, SHORT_HEADER, MISSING_STATUS, TRUNCATED_U16, TRUNCATED_U32 };
enum path { DISK_READ, RAW, DISK_WRITE, PATHS };
static const char *path_name[] = {"disk read", "fn_raw_call", "disk write"};
static const char *fault_name[] = {"good", "rejected", "unavailable", "delivered",
    "lost", "corrupt", "oversize", "truncated", "device mismatch",
    "command mismatch", "descriptor", "encoded length", "short header", "missing status", "truncated U16", "truncated U32"};

static struct {
    enum fault fault;
    unsigned opens, closes, transfers, transmissions, effects;
    unsigned pending, max_pending, remote_replies, old_deliveries, proofs, resets;
    unsigned reject_count, open_failures;
    uint8_t proof_available, reset_fail, status;
    uint8_t descriptor_override, descriptor, descriptor_chain;
    uint8_t pending_packet[FN_MAX_PACKET_SIZE];
    uint16_t pending_length;
    uint8_t pending_effect;
    uint8_t late[FN_MAX_PACKET_SIZE];
    uint16_t late_length;
} peer;
static fn_packet_backend_t guard;
static uint8_t scratch[FN_MAX_PACKET_SIZE + 2];
static unsigned attempts, backend_entries, replies, ports;
static struct IORequest *active_request;
static uint8_t abort_queued, abort_active, probe_reentry, probe_lifecycle;
static unsigned lifecycle_probes;
static struct { struct IORequest *request; unsigned completions; } submitted[64];
static unsigned submissions;

static unsigned submit_io(struct IORequest *request)
{
    unsigned id = submissions++;
    CHECK(id < 64);
    submitted[id].request = request; submitted[id].completions = 0;
    fujinet_nio_native_test_begin_io(request);
    return id;
}
static fujinet_nio_disk_context_t disk;
static uint8_t request_log[64][FN_MAX_PACKET_SIZE];
static uint16_t request_lengths[64], failed_lengths[64];
static uint8_t broker_results[64];
static unsigned attempt_sends[64], attempt_effects[64];

/* Independent fixture checksum; never use the production builder for replies. */
static void seal(uint8_t *p, uint16_t n)
{
    unsigned sum = 0, i;
    p[2] = (uint8_t)n; p[3] = (uint8_t)(n >> 8); p[4] = 0;
    for (i = 0; i < n; ++i) { sum += p[i]; sum = (sum & 255) + (sum >> 8); }
    p[4] = (uint8_t)sum;
}

static uint16_t response_for(const uint8_t *request, uint8_t *p)
{
    uint16_t n = request[1] == 3 ? 530 : 18;
    memset(p, 0, n);
    p[0] = request[0]; p[1] = request[1]; p[5] = 1; p[6] = peer.status;
    p[7] = FN_DISK_PROTOCOL_VERSION; p[11] = request[7];
    memcpy(p + 12, request + 8, 4);
    p[16] = 0; p[17] = 2;
    if (n > 18) memset(p + 18, request[8], 512);
    if (peer.descriptor_override) {
        static const uint8_t widths[] = {0, 1, 2, 3, 4, 2, 4, 4};
        uint8_t fields = widths[peer.descriptor & 7];
        uint16_t offset = (uint16_t)(6 + peer.descriptor_chain + fields);
        memmove(p + offset, p + 7, n - 7);
        p[5] = peer.descriptor_chain ? 0x80 : peer.descriptor;
        if (peer.descriptor_chain) p[6] = peer.descriptor;
        memset(p + 6 + peer.descriptor_chain, 0, fields);
        n = (uint16_t)(n - 7 + offset);
    }
    seal(p, n);
    return n;
}

void Disable(void) {}
void Enable(void) {}
APTR AllocMem(ULONG n, ULONG flags) { (void)flags; return calloc(1, n); }
void FreeMem(APTR p, ULONG n) { (void)n; free(p); }
struct MsgPort *CreatePort(CONST_STRPTR name, LONG priority)
{ (void)name; (void)priority; ++ports; return calloc(1, sizeof(struct MsgPort)); }
void DeletePort(struct MsgPort *p) { CHECK(ports > 0); --ports; free(p); }
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *req, ULONG flags)
{
    CHECK(strcmp(name, FUJINET_NIO_DEVICE_NAME) == 0 && flags == 0);
    return fujinet_nio_native_test_open(req, unit) != NULL ? 0 : -1;
}
void CloseDevice(struct IORequest *req) { (void)fujinet_nio_native_test_close(req); }
void ReplyMsg(struct Message *msg)
{
    CHECK(msg->mn_ReplyPort != NULL);
    unsigned id = submissions;
    while (id && submitted[id - 1].request != (struct IORequest *)msg) --id;
    CHECK(id != 0);
    CHECK(submitted[id - 1].completions == 0);
    ++submitted[id - 1].completions;
    ++msg->mn_ReplyPort->native_replies;
    ++replies;
    if (active_request == (struct IORequest *)msg) active_request = NULL;
}
LONG DoIO(struct IORequest *req)
{
    struct FujiNetNIORequest *nio = (struct FujiNetNIORequest *)req;
    unsigned id = attempts++, steps = 0, submission;
    unsigned sends_before = peer.transmissions, effects_before = peer.effects;
    CHECK(id < 64);
    request_lengths[id] = nio->fn_request_length;
    memcpy(request_log[id], nio->fn_request_data, nio->fn_request_length);
    active_request = req;
    submission = submit_io(req);
    if (abort_queued) {
        abort_queued = 0;
        CHECK(fujinet_nio_native_test_abort_io(req) == 0);
    }
    while (!submitted[submission].completions) {
        CHECK(++steps < 10);
        fujinet_nio_native_test_worker_step();
    }
    CHECK(submitted[submission].completions == 1);
    CHECK(memcmp(request_log[id], nio->fn_request_data, nio->fn_request_length) == 0);
    failed_lengths[id] = nio->fn_response_length;
    broker_results[id] = nio->fn_nio_error;
    attempt_sends[id] = peer.transmissions - sends_before;
    attempt_effects[id] = peer.effects - effects_before;
    if (nio->fn_nio_error != FN_OK) CHECK(nio->fn_response_length == 0);
    return req->io_Error;
}

static void check_lifecycle_reentry(void)
{
    uint8_t output[6], request[6] = {0}; uint16_t length = 9;
    unsigned opens = peer.opens, closes = peer.closes, resets = peer.resets;
    unsigned proofs = peer.proofs, transfers = peer.transfers;
    uint8_t opened = guard.opened;
    if (!probe_lifecycle) return;
    probe_lifecycle = 0; ++lifecycle_probes;
    CHECK(fn_packet_backend_open(&guard) == FN_ERR_BUSY);
    CHECK(fn_packet_backend_reset(&guard) == FN_ERR_BUSY);
    CHECK(fn_packet_backend_recover(&guard) == FN_ERR_BUSY);
    CHECK(fn_packet_backend_exchange(&guard, request, 6, output, 6, &length)
          == FN_ERR_BUSY && length == 0);
    fn_packet_backend_close(&guard);
    CHECK(guard.opened == opened && peer.opens == opens && peer.closes == closes);
    CHECK(peer.resets == resets && peer.proofs == proofs && peer.transfers == transfers);
}

static uint8_t peer_open(void *context)
{
    (void)context; ++peer.opens; check_lifecycle_reentry();
    if (peer.open_failures) { --peer.open_failures; return FN_ERR_TRANSPORT; }
    return FN_OK;
}
static void peer_close(void *context)
{ (void)context; ++peer.closes; check_lifecycle_reentry(); }
static uint8_t peer_reset(void *context)
{ (void)context; ++peer.resets; check_lifecycle_reentry(); return peer.reset_fail ? FN_ERR_TRANSPORT : FN_OK; }
static uint8_t peer_proof(void *context)
{
    (void)context; ++peer.proofs; check_lifecycle_reentry();
    return peer.proof_available && peer.pending == 0 && peer.late_length == 0
        ? FN_OK : FN_ERR_TRANSPORT;
}

static fn_packet_outcome_t peer_transfer(void *context, const uint8_t *request,
    uint16_t request_length, uint8_t *out, uint16_t capacity, uint16_t *length)
{
    uint8_t packet[FN_MAX_PACKET_SIZE];
    uint16_t n;
    (void)context; (void)request_length;
    ++peer.transfers;
    if (peer.fault == UNAVAILABLE || (peer.fault == REJECT && peer.reject_count)) {
        if (peer.reject_count) --peer.reject_count;
        return FN_PACKET_REJECTED;
    }
    /* No guard here: an unsafe backend would increase max_pending. */
    ++peer.transmissions; ++peer.pending;
    if (peer.pending > peer.max_pending) peer.max_pending = peer.pending;
    n = response_for(request, packet);
    if (probe_reentry) {
        uint8_t response[32]; uint16_t len = 99;
        probe_reentry = 0;
        CHECK(fn_packet_backend_recover(&guard) == FN_ERR_BUSY);
        CHECK(fn_packet_backend_reset(&guard) == FN_ERR_BUSY);
        CHECK(fn_packet_backend_open(&guard) == FN_ERR_BUSY);
        fn_packet_backend_close(&guard);
        CHECK(fn_packet_backend_exchange(&guard, request, request_length,
              response, sizeof(response), &len) == FN_ERR_BUSY && len == 0);
    }
    if (abort_active) {
        abort_active = 0;
        CHECK(active_request != NULL);
        CHECK(fujinet_nio_native_test_abort_io(active_request) == 0);
    }
    if (peer.fault != DELIVERY) ++peer.effects;
    if (peer.fault == DELIVERY || peer.fault == LOST) {
        memcpy(peer.pending_packet, packet, n); peer.pending_length = n;
        peer.pending_effect = peer.fault == DELIVERY;
        return FN_PACKET_UNKNOWN;
    }
    --peer.pending;
    ++peer.remote_replies;
    if (peer.late_length) {
        /* If guard wrongly clears quarantine, deliver the old same-command
         * packet in preference to the fresh one: expose stale attribution. */
        n = peer.late_length; memcpy(packet, peer.late, n); peer.late_length = 0;
        ++peer.old_deliveries;
    }
    if (peer.fault == CORRUPT) packet[4] ^= 1;
    if (peer.fault == WRONG_DEVICE) { packet[0] ^= 1; seal(packet, n); }
    if (peer.fault == WRONG_COMMAND) { packet[1] ^= 1; seal(packet, n); }
    if (peer.fault == BAD_DESCRIPTOR) { n = 6; packet[5] = 0x81; seal(packet, n); }
    if (peer.fault == MISSING_STATUS) { n = 6; packet[5] = 1; seal(packet, n); }
    if (peer.fault == TRUNCATED_U16) { n = 7; packet[5] = 5; seal(packet, n); }
    if (peer.fault == TRUNCATED_U32) { n = 9; packet[5] = 7; seal(packet, n); }
    if (peer.fault == BAD_LENGTH) packet[2] ^= 1;
    if (peer.fault == TRUNCATED) --n;
    if (peer.fault == SHORT_HEADER) n = 5;
    CHECK(n <= capacity); memcpy(out, packet, n);
    *length = peer.fault == OVERSIZE ? (uint16_t)(capacity + 1) : n;
    return FN_PACKET_COMPLETE;
}

/* Independent remote time: delivery/effect/reply can happen after local clear.
 * Recovery callback itself does not manufacture these events. */
static void peer_finish_late(void)
{
    CHECK(peer.pending == 1);
    if (peer.pending_effect) ++peer.effects;
    peer.pending_effect = 0; --peer.pending;
    memcpy(peer.late, peer.pending_packet, peer.pending_length);
    peer.late_length = peer.pending_length;
    ++peer.remote_replies;
}
static void peer_delivery_barrier(void)
{
    if (peer.pending) peer_finish_late();
    if (peer.late_length) { ++peer.old_deliveries; peer.late_length = 0; }
    peer.proof_available = 1;
}

static uint8_t guard_open(void) { return fn_packet_backend_open(&guard); }
static void guard_close(void) { fn_packet_backend_close(&guard); }
static uint8_t guard_exchange(const uint8_t *request, uint16_t request_len,
    uint8_t *response, uint16_t capacity, uint16_t *length,
    uint8_t *detail, uint8_t *native_error, uint16_t *native_status)
{
    ++backend_entries;
    *detail = 0; *native_error = 0; *native_status = 0;
    return fn_packet_backend_exchange(&guard, request, request_len,
                                      response, capacity, length);
}
static void reset_case(void)
{
    unsigned i;
    static const fn_packet_io_t io = {peer_open, peer_close, peer_transfer,
                                     peer_reset, peer_proof};
    fn_transport_close();
    (void)fujinet_nio_native_test_expunge();
    CHECK(ports == 0);
    for (i = 0; i < submissions; ++i) CHECK(submitted[i].completions == 1);
    submissions = 0;
    probe_lifecycle = 0; lifecycle_probes = 0;
    fujinet_nio_native_test_reset();
    memset(&peer, 0, sizeof(peer));
    memset(scratch, 0xE7, sizeof(scratch));
    CHECK(fn_packet_backend_init(&guard, &io, &peer, scratch + 1,
                                 FN_MAX_PACKET_SIZE) == FN_OK);
    CHECK(guard.quarantined && !guard.opened);
    {
        uint8_t request[6] = {0xFC, 4, 6, 0, 0, 0}, output[6];
        uint16_t length = 77;
        seal(request, sizeof(request));
        CHECK(fn_packet_backend_open(&guard) == FN_OK && guard.opened);
        CHECK(fn_packet_backend_exchange(&guard, request, 6, output, 6, &length)
              == FN_ERR_TRANSPORT && length == 0);
        CHECK(peer.transfers == 0 && peer.transmissions == 0 && guard.quarantined);
        fn_packet_backend_close(&guard);
        CHECK(!guard.opened && peer.opens == 1 && peer.closes == 1);
        peer.opens = peer.closes = 0;
    }
    peer.proof_available = 1;
    CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    fujinet_nio_native_test_set_backend(guard_open, guard_close, guard_exchange,
                                       NULL, NULL, NULL, NULL);
    attempts = backend_entries = replies = 0;
    abort_active = abort_queued = probe_reentry = 0;
    _fn_initialized = 0;
    CHECK(fn_init() == FN_OK);
    CHECK(fujinet_nio_disk_context_init(&disk) == FN_OK);
}

static uint8_t call(enum path path, uint8_t tag, uint8_t *out, uint16_t *length)
{
    uint8_t result;
    unsigned start = attempts, i;
    if (path == RAW) {
        uint8_t payload[520] = {1, 1, 0, 0, 0, 0, 0, 2};
        fn_raw_response_t response;
        payload[2] = tag;
        memset(payload + 8, tag, 512);
        result = fn_raw_call(FN_DEVICE_DISK, 4, payload, sizeof(payload),
                             out, 512, &response);
        *length = response.payload_length;
    } else if (path == DISK_READ) {
        *length = 0;
        result = fujinet_nio_disk_client.read_sector(&disk, 1, tag, out, 512, length);
    } else {
        uint8_t data[512]; memset(data, tag, sizeof(data));
        *length = 0;
        result = fujinet_nio_disk_client.write_sector(&disk, 1, tag, data, sizeof(data));
    }
    if (path != RAW) {
        CHECK(disk.retry.attempts == attempts - start);
        for (i = 0; i < disk.retry.attempts; ++i) {
            CHECK(disk.retry.results[i] == broker_results[start + i]);
            CHECK(disk.retry.response_lengths[i] == failed_lengths[start + i]);
            CHECK(disk.exchange_causes[i] == 0 || disk.exchange_causes[i] == FUJINET_NIO_DETAIL_BACKEND_OPEN);
            CHECK(disk.exchange_native_errors[i] == 0 && disk.exchange_statuses[i] == 0);
        }
    }
    CHECK(scratch[0] == 0xE7 && scratch[sizeof(scratch)-1] == 0xE7);
    return result;
}
static void expect_call(enum path path, uint8_t tag, uint8_t result, unsigned count)
{
    uint8_t out[514]; uint16_t len = 0xFFFF;
    unsigned start = attempts, i;
    memset(out, 0xA9, sizeof(out));
    CHECK(call(path, tag, out + 1, &len) == result);
    CHECK(attempts - start == count);
    CHECK(out[0] == 0xA9 && out[513] == 0xA9);
    if (result != FN_OK || path == DISK_WRITE) {
        CHECK(len == 0);
        for (i = 1; i < 513; ++i) CHECK(out[i] == 0xA9);
    } else if (path == DISK_READ) {
        CHECK(len == 512);
        for (i = 1; i < 513; ++i) CHECK(out[i] == tag);
    } else {
        CHECK(len == 11 && out[6] == tag);
        for (i = 12; i < 513; ++i) CHECK(out[i] == 0xA9);
    }
    CHECK(peer.max_pending <= 1);
    CHECK(replies == attempts && submissions == attempts);
    for (i = start + 1; i < attempts; ++i)
        CHECK(request_lengths[i] == request_lengths[start] &&
              memcmp(request_log[i], request_log[start], request_lengths[i]) == 0);
}

static unsigned retries(enum path path) { return path == RAW ? 2 : 3; }
static void test_before_send(enum path path)
{
    reset_case(); peer.fault = REJECT; peer.reject_count = 1;
    expect_call(path, 0x21, FN_OK, 2);
    CHECK(peer.transfers == 2 && peer.transmissions == 1 && peer.effects == 1);
    CHECK(attempt_sends[0] == 0 && attempt_effects[0] == 0 && attempt_sends[1] == 1);
    CHECK(backend_entries == 2 && peer.remote_replies == 1 && !guard.quarantined);
    reset_case(); peer.open_failures = 1;
    expect_call(path, 0x22, FN_OK, 2);
    CHECK(peer.opens == 2 && backend_entries == 1 && peer.transfers == 1);
    CHECK(attempt_sends[0] == 0 && attempt_effects[0] == 0 && attempt_sends[1] == 1);
    CHECK(peer.transmissions == 1 && peer.effects == 1);
    reset_case(); peer.fault = UNAVAILABLE;
    expect_call(path, 0x23, FN_ERR_TRANSPORT, retries(path));
    CHECK(peer.transfers == retries(path) && peer.transmissions == 0 && peer.effects == 0);
    CHECK(!guard.quarantined);
    peer.fault = GOOD;
    expect_call(path, 0x24, FN_OK, 1);
}
static void test_fault_and_recovery(enum path path, enum fault fault)
{
    unsigned n = retries(path), entries;
    reset_case(); peer.fault = fault; probe_reentry = 1;
    expect_call(path, 0x31, FN_ERR_TRANSPORT, n);
    CHECK(backend_entries == n && peer.transfers == 1 && peer.transmissions == 1);
    CHECK(peer.effects == (fault == DELIVERY ? 0U : 1U));
    CHECK(peer.pending == ((fault == DELIVERY || fault == LOST) ? 1U : 0U));
    CHECK(guard.quarantined && !guard.opened);
    CHECK(peer.opens == n && peer.closes == n);
    if (peer.pending) {
        CHECK(peer.proof_available == 1 && peer.late_length == 0);
        CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
        CHECK(peer.pending == 1 && guard.quarantined);
    }
    entries = backend_entries;
    fn_transport_close(); CHECK(fn_transport_init() == FN_OK);
    CHECK(!guard.opened && peer.opens == n && peer.closes == n);
    peer.reset_fail = 1;
    CHECK(fn_packet_backend_reset(&guard) == FN_ERR_TRANSPORT);
    peer.reset_fail = 0;
    CHECK(fn_packet_backend_reset(&guard) == FN_OK && guard.quarantined);
    peer.proof_available = 0;
    CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT && guard.quarantined);
    peer.fault = GOOD;
    expect_call(path, 0x42, FN_ERR_TRANSPORT, n);
    CHECK(backend_entries == entries + n && peer.transfers == 1);
    CHECK(peer.opens == 2 * n && peer.closes == 2 * n && !guard.opened);
    if (peer.pending) {
        peer_finish_late();
        peer.proof_available = 1;
        CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
        expect_call(path, 0x53, FN_ERR_TRANSPORT, n);
        CHECK(peer.old_deliveries == 0 && peer.late_length != 0);
    }
    peer_delivery_barrier();
    /* Proof becoming available alone does not clear quarantine. */
    expect_call(path, 0x64, FN_ERR_TRANSPORT, n);
    CHECK(peer.transmissions == 1 && peer.effects == 1);
    CHECK(fn_packet_backend_recover(&guard) == FN_OK && !guard.quarantined);
    expect_call(path, 0x75, FN_OK, 1);
    CHECK(peer.transmissions == 2 && peer.effects == 2 && peer.pending == 0);
    CHECK(peer.remote_replies == 2);
    CHECK(guard.opened && peer.opens == peer.closes + 1);
    printf("PASS %s: %s, lifecycle, late/proof recovery\n", path_name[path], fault_name[fault]);
}

static void test_aborts(enum path path)
{
    reset_case(); abort_queued = 1;
    /* raw retries ABORTED once, disk does not. Reject its later attempt to
     * isolate the fact that the aborted queued IORequest transmitted nothing. */
    peer.fault = UNAVAILABLE;
    expect_call(path, 0x11, path == RAW ? FN_ERR_TRANSPORT : FN_ERR_ABORTED,
                path == RAW ? 2 : 1);
    CHECK(peer.transmissions == 0 && peer.effects == 0);
    CHECK(backend_entries == (path == RAW ? 1U : 0U));
    reset_case(); abort_active = 1;
    expect_call(path, 0x10, path == RAW ? FN_OK : FN_ERR_ABORTED,
                path == RAW ? 2 : 1);
    CHECK(peer.transmissions == (path == RAW ? 2U : 1U));
    CHECK(peer.effects == peer.transmissions && !guard.quarantined);
    reset_case(); peer.fault = DELIVERY; abort_active = 1;
    expect_call(path, 0x12, path == RAW ? FN_ERR_TRANSPORT : FN_ERR_ABORTED,
                path == RAW ? 2 : 1);
    CHECK(peer.transmissions == 1 && peer.pending == 1 && peer.effects == 0);
    peer_finish_late(); CHECK(peer.effects == 1);
    CHECK(fn_packet_backend_reset(&guard) == FN_OK);
    expect_call(path, 0x13, FN_ERR_TRANSPORT, retries(path));
    CHECK(peer.transmissions == 1);
    peer_delivery_barrier(); CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    peer.fault = GOOD; expect_call(path, 0x14, FN_OK, 1);
}

static void test_queued_behind_unknown(enum path path)
{
    struct FujiNetNIORequest prior;
    struct MsgPort port = {0};
    uint8_t request[14] = {0xFC, 3, 14, 0, 0, 0, 1, 1, 0x18, 0, 0, 0, 0, 2};
    uint8_t response[FN_MAX_PACKET_SIZE];
    unsigned i;
    reset_case(); seal(request, sizeof(request)); memset(response, 0xDA, sizeof(response));
    memset(&prior, 0, sizeof(prior));
    prior.fn_io.io_Message.mn_ReplyPort = &port;
    prior.fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
    prior.fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
    prior.fn_request_data = request; prior.fn_request_length = sizeof(request);
    prior.fn_response_data = response; prior.fn_response_capacity = sizeof(response);
    CHECK(fujinet_nio_native_test_open(&prior.fn_io, 0) != NULL);
    (void)submit_io(&prior.fn_io);
    /* Account this separately submitted IORequest in the completion oracle. */
    ++attempts;
    peer.fault = DELIVERY;
    expect_call(path, 0x19, FN_ERR_TRANSPORT, retries(path));
    CHECK(port.native_replies == 1 && prior.fn_response_length == 0);
    CHECK(prior.fn_nio_error == FN_ERR_TRANSPORT);
    CHECK(peer.transmissions == 1 && peer.effects == 0 && peer.pending == 1);
    for (i = 0; i < sizeof(response); ++i) CHECK(response[i] == 0xDA);
    CloseDevice(&prior.fn_io);
    peer_delivery_barrier(); CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    peer.fault = GOOD; expect_call(path, 0x20, FN_OK, 1);
}

static void test_independent_buffers(enum path path)
{
    uint8_t first[514], saved[514], second[514];
    uint16_t first_length = 0, second_length = 0;
    unsigned i;
    reset_case();
    memset(first, 0xA9, sizeof(first)); memset(second, 0xB7, sizeof(second));
    CHECK(call(path, 0x41, first + 1, &first_length) == FN_OK);
    memcpy(saved, first, sizeof(saved));
    peer.fault = LOST;
    CHECK(call(path, 0x42, second + 1, &second_length) == FN_ERR_TRANSPORT);
    CHECK(second_length == 0 && memcmp(first, saved, sizeof(first)) == 0);
    for (i = 0; i < sizeof(second); ++i) CHECK(second[i] == 0xB7);
    CHECK(peer.transmissions == 2 && peer.effects == 2 && peer.pending == 1);
    CHECK(fn_packet_backend_reset(&guard) == FN_OK);
    peer_finish_late();
    CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
    CHECK(memcmp(first, saved, sizeof(first)) == 0);
    for (i = 0; i < sizeof(second); ++i) CHECK(second[i] == 0xB7);
    peer_delivery_barrier(); CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    peer.fault = GOOD;
    expect_call(path, 0x43, FN_OK, 1);
    CHECK(memcmp(first, saved, sizeof(first)) == 0);
    for (i = 0; i < sizeof(second); ++i) CHECK(second[i] == 0xB7);
}

static void test_reset_from_healthy(enum path path)
{
    unsigned fail;
    for (fail = 0; fail < 2; ++fail) {
        reset_case();
        expect_call(path, 0x61, FN_OK, 1);
        CHECK(!guard.quarantined);
        peer.reset_fail = (uint8_t)fail;
        CHECK(fn_packet_backend_reset(&guard) == (fail ? FN_ERR_TRANSPORT : FN_OK));
        CHECK(guard.quarantined);
        expect_call(path, 0x62, FN_ERR_TRANSPORT, retries(path));
        CHECK(peer.transmissions == 1 && peer.effects == 1);
        peer.proof_available = 0;
        CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
        expect_call(path, 0x63, FN_ERR_TRANSPORT, retries(path));
        CHECK(peer.transmissions == 1);
        peer.proof_available = 1;
        CHECK(fn_packet_backend_recover(&guard) == FN_OK);
        expect_call(path, 0x64, FN_OK, 1);
        CHECK(peer.transmissions == 2 && peer.effects == 2);
    }
}

/* Direct guard boundary cases complement the real-caller matrix. */
static void test_guard_capacity_and_proof(void)
{
    uint8_t request[14] = {0xFC, 3, 14, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 2};
    uint8_t output[16]; uint16_t length = 99; unsigned i;
    reset_case(); seal(request, sizeof(request)); memset(output, 0xBC, sizeof(output));
    CHECK(guard_open() == FN_OK);
    CHECK(fn_packet_backend_exchange(&guard, request, sizeof(request),
          output, sizeof(output), &length) == FN_ERR_TRANSPORT);
    CHECK(length == 0 && guard.quarantined && peer.transmissions == 1);
    for (i = 0; i < sizeof(output); ++i) CHECK(output[i] == 0xBC);
    CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    guard.io.quiesce = NULL;
    CHECK(fn_packet_backend_reset(&guard) == FN_OK);
    CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
    CHECK(fn_packet_backend_exchange(&guard, request, sizeof(request),
          output, sizeof(output), &length) == FN_ERR_TRANSPORT);
    CHECK(peer.transmissions == 1 && length == 0);
    request[4] ^= 1;
    CHECK(fn_packet_backend_exchange(&guard, request, sizeof(request),
          output, sizeof(output), &length) == FN_ERR_INVALID);
    CHECK(peer.transmissions == 1 && length == 0);
    CHECK(fn_packet_backend_exchange(&guard, request, sizeof(request),
          output, sizeof(output), NULL) == FN_ERR_INVALID);
    guard.io.local_reset = NULL;
    CHECK(fn_packet_backend_reset(&guard) == FN_ERR_TRANSPORT && guard.quarantined);
}

static void test_descriptor_widths_and_chains(enum path path)
{
    unsigned descriptor, chain;
    for (descriptor = 0; descriptor < 8; ++descriptor) {
        for (chain = 0; chain < 2; ++chain) {
            reset_case();
            peer.descriptor_override = 1;
            peer.descriptor = (uint8_t)descriptor;
            peer.descriptor_chain = (uint8_t)chain;
            expect_call(path, 0x47, FN_OK, 1);
            CHECK(peer.transmissions == 1 && peer.effects == 1 && !guard.quarantined);
        }
    }
}

static void test_shared_port_identity(void)
{
    struct FujiNetNIORequest req[2];
    struct MsgPort port = {0};
    uint8_t requests[2][14] = {
        {0xFC, 3, 14, 0, 0, 0, 1, 1, 0x71, 0, 0, 0, 0, 2},
        {0xFC, 3, 14, 0, 0, 0, 1, 1, 0x72, 0, 0, 0, 0, 2}};
    uint8_t output[2][FN_MAX_PACKET_SIZE];
    unsigned ids[2], i, j;
    reset_case(); memset(req, 0, sizeof(req)); memset(output, 0xDA, sizeof(output));
    for (i = 0; i < 2; ++i) {
        seal(requests[i], sizeof(requests[i]));
        req[i].fn_io.io_Message.mn_ReplyPort = &port;
        req[i].fn_io.io_Command = FUJINET_NIO_CMD_EXCHANGE;
        req[i].fn_struct_size = FUJINET_NIO_REQUEST_SIZE;
        req[i].fn_request_data = requests[i]; req[i].fn_request_length = 14;
        req[i].fn_response_data = output[i]; req[i].fn_response_capacity = sizeof(output[i]);
        CHECK(fujinet_nio_native_test_open(&req[i].fn_io, 0) != NULL);
        ids[i] = submit_io(&req[i].fn_io); ++attempts;
    }
    fujinet_nio_native_test_worker_step();
    CHECK(submitted[ids[0]].completions == 1 && submitted[ids[1]].completions == 0);
    CHECK(port.native_replies == 1);
    for (j = 0; j < sizeof(output[1]); ++j) CHECK(output[1][j] == 0xDA);
    fujinet_nio_native_test_worker_step();
    fujinet_nio_native_test_worker_step();
    CHECK(submitted[ids[0]].completions == 1 && submitted[ids[1]].completions == 1);
    CHECK(port.native_replies == 2 && replies == 2 && attempts == 2);
    for (i = 0; i < 2; ++i) {
        CHECK(req[i].fn_nio_error == FN_OK && req[i].fn_response_length == 530);
        for (j = 18; j < 530; ++j) CHECK(output[i][j] == (uint8_t)(0x71 + i));
        for (j = 530; j < sizeof(output[i]); ++j) CHECK(output[i][j] == 0xDA);
        CloseDevice(&req[i].fn_io);
    }
    CHECK(peer.transmissions == 2 && peer.effects == 2 && peer.max_pending == 1);
}

static fn_packet_outcome_t echo_packet(void *context, const uint8_t *request,
    uint16_t request_length, uint8_t *response, uint16_t capacity, uint16_t *length)
{
    unsigned *calls = context;
    ++*calls; CHECK(request_length <= capacity);
    memcpy(response, request, request_length); *length = request_length;
    return FN_PACKET_COMPLETE;
}
static uint8_t simple_proof(void *context) { (void)context; return FN_OK; }

static void test_public_boundaries(void)
{
    fn_packet_backend_t b;
    fn_packet_io_t io = {NULL, NULL, echo_packet, NULL, simple_proof};
    static uint8_t request[65535], storage[65537], output[65537];
    static const uint16_t capacities[] = {6, 1024, 65535};
    unsigned calls = 0, i, j;
    uint16_t length = 33;
    CHECK(fn_packet_backend_init(NULL, &io, &calls, storage, 6) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_open(NULL) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_reset(NULL) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_recover(NULL) == FN_ERR_INVALID);
    fn_packet_backend_close(NULL);
    CHECK(fn_packet_backend_exchange(NULL, request, 6, output, 6, &length)
          == FN_ERR_INVALID && length == 0);
    CHECK(fn_packet_backend_exchange(NULL, request, 6, output, 6, NULL) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_init(&b, &io, &calls, storage, 5) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_init(&b, NULL, &calls, storage, 6) == FN_ERR_INVALID);
    CHECK(fn_packet_backend_init(&b, &io, &calls, NULL, 6) == FN_ERR_INVALID);
    for (i = 0; i < sizeof(capacities)/sizeof(capacities[0]); ++i) {
        uint16_t cap = capacities[i];
        memset(storage, 0xB9, sizeof(storage)); memset(output, 0xC8, sizeof(output));
        memset(request, 0, sizeof(request)); request[0] = 0xFC; request[1] = 4;
        seal(request, cap);
        /* An io table embedded in the destination survives initialization. */
        b.io = io;
        CHECK(fn_packet_backend_init(&b, &b.io, &calls, storage + 1, cap) == FN_OK);
        CHECK(b.io.transfer == echo_packet && b.io.quiesce == simple_proof);
        CHECK(fn_packet_backend_open(&b) == FN_OK);
        CHECK(fn_packet_backend_recover(&b) == FN_OK);
        CHECK(fn_packet_backend_exchange(&b, request, cap, output + 1, cap, &length) == FN_OK);
        CHECK(length == cap && calls == i + 1 && !b.quarantined);
        CHECK(memcmp(output + 1, request, cap) == 0);
        CHECK(output[0] == 0xC8 && storage[0] == 0xB9);
        for (j = (unsigned)cap + 1; j < sizeof(output); ++j)
            CHECK(output[j] == 0xC8 && storage[j] == 0xB9);
        if (cap < 65535) {
            seal(request, (uint16_t)(cap + 1));
            CHECK(fn_packet_backend_exchange(&b, request, (uint16_t)(cap + 1),
                  output + 1, cap, &length) == FN_ERR_INVALID && length == 0);
            CHECK(calls == i + 1 && !b.quarantined);
        }
        fn_packet_backend_close(&b);
    }
    /* A nonzero-width descriptor chain remains structurally legal. */
    memset(request, 0, 13); request[0] = 0xFC; request[1] = 4;
    request[5] = 0x85; request[6] = 7; seal(request, 13);
    CHECK(fn_packet_backend_open(&b) == FN_OK);
    CHECK(fn_packet_backend_exchange(&b, request, 13, output, 13, &length) == FN_OK);
    CHECK(length == 13 && !b.quarantined);
    fn_packet_backend_close(&b);
}

static void test_lifecycle_callback_reentry(void)
{
    reset_case();
    probe_lifecycle = 1; CHECK(guard_open() == FN_OK && guard.opened);
    probe_lifecycle = 1; CHECK(fn_packet_backend_reset(&guard) == FN_OK);
    probe_lifecycle = 1; CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    probe_lifecycle = 1; guard_close();
    CHECK(!guard.opened && peer.opens == 1 && peer.closes == 1);
    probe_lifecycle = 1; CHECK(fn_packet_backend_reset(&guard) == FN_OK);
    peer.proof_available = 0;
    probe_lifecycle = 1; CHECK(fn_packet_backend_recover(&guard) == FN_ERR_TRANSPORT);
    CHECK(!guard.opened && guard.quarantined);
    peer.proof_available = 1;
    probe_lifecycle = 1; CHECK(fn_packet_backend_recover(&guard) == FN_OK);
    CHECK(!guard.opened && !guard.quarantined && lifecycle_probes == 7);
}

static void test_known_completion_policy(void)
{
    uint8_t payload[520] = {1, 1, 0x45, 0, 0, 0, 0, 2};
    uint8_t output[13]; fn_raw_response_t response;
    static const uint8_t canonical[] = {0xFC, 4, 18, 0, 0x5D, 1, 0,
                                       1, 0, 0, 0, 1, 0x45, 0, 0, 0, 0, 2};
    unsigned i;
    static const uint8_t statuses[] = {FN_ERR_TIMEOUT, FN_ERR_IO, 0xE9};
    reset_case(); memset(output, 0xBA, sizeof(output));
    CHECK(fn_raw_call(FN_DEVICE_DISK, 4, payload, sizeof(payload), output + 1,
                      10, &response) == FN_ERR_IO);
    CHECK(response.payload_length == 0 && attempts == 2 && replies == 2);
    CHECK(peer.transmissions == 2 && peer.effects == 2 && peer.remote_replies == 2);
    CHECK(peer.max_pending == 1 && !guard.quarantined);
    CHECK(memcmp(scratch + 1, canonical, sizeof(canonical)) == 0);
    for (i = 0; i < sizeof(output); ++i) CHECK(output[i] == 0xBA);
    for (i = 0; i < sizeof(statuses); ++i) {
        reset_case(); peer.status = statuses[i];
        CHECK(fn_raw_call(FN_DEVICE_DISK, 4, payload, sizeof(payload), output + 1,
                          11, &response) == FN_OK);
        CHECK(response.status == statuses[i] && response.payload_length == 11);
        CHECK(attempts == 1 && peer.effects == 1 && !guard.quarantined);
        CHECK(output[0] == 0xBA && output[12] == 0xBA);
    }
}

int main(void)
{
    enum path path; enum fault fault;
    for (path = DISK_READ; path < PATHS; ++path) {
        test_descriptor_widths_and_chains(path);
        test_before_send(path);
        for (fault = DELIVERY; fault <= TRUNCATED_U32; ++fault)
            test_fault_and_recovery(path, fault);
        test_independent_buffers(path);
        test_reset_from_healthy(path);
        test_aborts(path);
        test_queued_behind_unknown(path);
        printf("PASS %s: pre-send, reset-from-healthy, queued/active abort, FIFO ownership\n", path_name[path]);
    }
    test_shared_port_identity();
    test_public_boundaries();
    test_lifecycle_callback_reentry();
    test_guard_capacity_and_proof();
    test_known_completion_policy();
    fn_transport_close(); (void)fujinet_nio_native_test_expunge(); CHECK(ports == 0);
    puts("packet backend: both real callers, six fault rows and ownership passed");
    return 0;
}
