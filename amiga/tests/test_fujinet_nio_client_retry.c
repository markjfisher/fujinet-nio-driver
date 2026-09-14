#include <stdio.h>
#include <string.h>

#include "../../common/fujinet_disk_retry.h"
#include "../include/fujinet_disk_driver.h"
#include "../include/fujinet_nio_endian.h"
#include "fn_protocol.h"

enum {
    NIO_DISK_READ_SECTOR = 0x03,
    NIO_DISK_WRITE_SECTOR = 0x04,
    NIO_DISK_READ_REQUEST_SIZE = FN_HEADER_SIZE + 8,
    NIO_DISK_WRITE_REQUEST_SIZE = FN_HEADER_SIZE + 8 + FUJINET_DISK_BLOCK_SIZE
};

#define MAX_CALLS 8U
#define MAX_REQUEST_SIZE NIO_DISK_WRITE_REQUEST_SIZE

static unsigned failures;
static unsigned transport_calls;
static uint8_t scripted_results[MAX_CALLS];
static uint16_t scripted_failure_lengths[MAX_CALLS];
static uint16_t incoming_response_lengths[MAX_CALLS];
static uint16_t captured_request_lengths[MAX_CALLS];
static uint8_t captured_requests[MAX_CALLS][MAX_REQUEST_SIZE];
static uint8_t scripted_response[FN_DISK_CONTEXT_PACKET_SIZE];
static uint16_t scripted_response_length;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

uint8_t __wrap_fn_transport_exchange_buffers(
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t response_capacity,
    uint16_t *response_length)
{
    unsigned call = transport_calls++;
    uint8_t result;

    CHECK("transport call capacity", call < MAX_CALLS);
    if (call >= MAX_CALLS) return FN_ERR_INTERNAL;

    incoming_response_lengths[call] = response_length != NULL
                                               ? *response_length
                                               : 0xFFFFU;
    captured_request_lengths[call] = request_length;
    CHECK("captured request fits", request_length <= MAX_REQUEST_SIZE);
    if (request != NULL && request_length <= MAX_REQUEST_SIZE)
        memcpy(captured_requests[call], request, request_length);

    result = scripted_results[call];
    if (result == FN_OK) {
        CHECK("scripted response fits", scripted_response_length <= response_capacity);
        if (response != NULL && scripted_response_length <= response_capacity)
            memcpy(response, scripted_response, scripted_response_length);
        if (response_length != NULL)
            *response_length = scripted_response_length;
    } else {
        if (response != NULL && response_capacity != 0)
            response[0] = (uint8_t)(0xA0U + call);
        if (response_length != NULL)
            *response_length = scripted_failure_lengths[call];
    }
    return result;
}

static uint8_t scripted_transport(void *context, const uint8_t *request,
                                  uint16_t request_length, uint8_t *response,
                                  uint16_t response_capacity,
                                  uint16_t *response_length)
{
    (void)context;
    return __wrap_fn_transport_exchange_buffers(
        request, request_length, response, response_capacity, response_length);
}

static uint8_t retry_exchange(const uint8_t *request, uint16_t request_length,
                              uint8_t *response, uint16_t response_capacity,
                              uint16_t *response_length)
{
    return fujinet_disk_retry_exchange(
        scripted_transport, NULL, request, request_length, response,
        response_capacity, response_length, NULL, NULL, NULL);
}

static uint8_t context_exchange(void *context, const uint8_t *request,
                                uint16_t request_length, uint8_t *response,
                                uint16_t response_capacity,
                                uint16_t *response_length)
{
    return fujinet_disk_retry_exchange(
        scripted_transport, NULL, request, request_length, response,
        response_capacity, response_length,
        (fujinet_disk_retry_diagnostics_t *)context, NULL, NULL);
}

static void put_u16le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_u32le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void reset_harness(void)
{
    transport_calls = 0;
    memset(scripted_results, 0, sizeof(scripted_results));
    memset(scripted_failure_lengths, 0, sizeof(scripted_failure_lengths));
    memset(incoming_response_lengths, 0xFF,
           sizeof(incoming_response_lengths));
    memset(captured_request_lengths, 0, sizeof(captured_request_lengths));
    memset(captured_requests, 0, sizeof(captured_requests));
    memset(scripted_response, 0, sizeof(scripted_response));
    scripted_response_length = 0;
}

static void finish_packet(uint8_t *packet, uint16_t length)
{
    put_u16le(packet + 2, length);
    packet[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(packet, length);
}

static void build_read_request(uint8_t *packet, uint8_t slot, uint32_t lba,
                               uint16_t capacity)
{
    memset(packet, 0, NIO_DISK_READ_REQUEST_SIZE);
    packet[0] = FN_DEVICE_DISK;
    packet[1] = NIO_DISK_READ_SECTOR;
    packet[6] = FN_DISK_PROTOCOL_VERSION;
    packet[7] = slot;
    put_u32le(packet + 8, lba);
    put_u16le(packet + 12, capacity);
    finish_packet(packet, NIO_DISK_READ_REQUEST_SIZE);
}

static void build_write_request(uint8_t *packet, uint8_t slot, uint32_t lba,
                                uint16_t body_length)
{
    uint16_t i;

    memset(packet, 0, NIO_DISK_WRITE_REQUEST_SIZE);
    packet[0] = FN_DEVICE_DISK;
    packet[1] = NIO_DISK_WRITE_SECTOR;
    packet[6] = FN_DISK_PROTOCOL_VERSION;
    packet[7] = slot;
    put_u32le(packet + 8, lba);
    put_u16le(packet + 12, body_length);
    for (i = 0; i < FUJINET_DISK_BLOCK_SIZE; ++i)
        packet[14 + i] = (uint8_t)(i ^ 0x5AU);
    finish_packet(packet, NIO_DISK_WRITE_REQUEST_SIZE);
}

static void build_success_response(uint8_t command, const uint8_t *payload,
                                   uint16_t payload_length)
{
    scripted_response_length = (uint16_t)(FN_HEADER_SIZE + payload_length);
    memset(scripted_response, 0, scripted_response_length);
    scripted_response[0] = FN_DEVICE_DISK;
    scripted_response[1] = command;
    if (payload_length != 0)
        memcpy(scripted_response + FN_HEADER_SIZE, payload, payload_length);
    finish_packet(scripted_response, scripted_response_length);
}

static void build_status_response(uint8_t command, uint8_t status)
{
    scripted_response_length = FN_HEADER_SIZE + 1;
    memset(scripted_response, 0, scripted_response_length);
    scripted_response[0] = FN_DEVICE_DISK;
    scripted_response[1] = command;
    scripted_response[5] = 1;
    scripted_response[6] = status;
    finish_packet(scripted_response, scripted_response_length);
}

static void test_recovered_read(void)
{
    fn_disk_client_context_t context;
    fujinet_disk_retry_diagnostics_t diagnostics;
    uint8_t payload[11 + FUJINET_DISK_BLOCK_SIZE];
    uint8_t expected[FUJINET_DISK_BLOCK_SIZE];
    uint8_t actual[FUJINET_DISK_BLOCK_SIZE];
    uint16_t actual_length = 0;
    uint16_t i;
    uint8_t result;

    reset_harness();
    for (i = 0; i < FUJINET_DISK_BLOCK_SIZE; ++i)
        expected[i] = (uint8_t)(i ^ 0xC3U);
    memset(payload, 0, sizeof(payload));
    payload[0] = FN_DISK_PROTOCOL_VERSION;
    payload[4] = 2;
    put_u32le(payload + 5, 0x12345678UL);
    put_u16le(payload + 9, FUJINET_DISK_BLOCK_SIZE);
    memcpy(payload + 11, expected, sizeof(expected));
    build_success_response(NIO_DISK_READ_SECTOR, payload, sizeof(payload));
    scripted_results[0] = FN_ERR_TRANSPORT;
    scripted_failure_lengths[0] = 37;

    CHECK("read context init", fn_disk_context_init(&context, context_exchange,
                                                    &diagnostics) == FN_OK);
    result = fn_disk_read_sector_context(
        &context, 2, 0x12345678UL, actual, sizeof(actual), &actual_length);

    CHECK("recovered read result", result == FN_OK);
    CHECK("recovered read length", actual_length == FUJINET_DISK_BLOCK_SIZE);
    CHECK("recovered read body", memcmp(actual, expected, sizeof(actual)) == 0);
    CHECK("recovered read attempts", transport_calls == 2);
    CHECK("recovered read first length clear", incoming_response_lengths[0] == 0);
    CHECK("recovered read second length clear", incoming_response_lengths[1] == 0);
    CHECK("recovered read request length",
          captured_request_lengths[0] == NIO_DISK_READ_REQUEST_SIZE);
    CHECK("recovered read identical replay",
          captured_request_lengths[0] == captured_request_lengths[1] &&
          memcmp(captured_requests[0], captured_requests[1],
                 captured_request_lengths[0]) == 0);
}

static void test_recovered_write(void)
{
    fn_disk_client_context_t context;
    fujinet_disk_retry_diagnostics_t diagnostics;
    uint8_t payload[11];
    uint8_t data[FUJINET_DISK_BLOCK_SIZE];
    uint16_t i;
    uint8_t result;

    reset_harness();
    for (i = 0; i < FUJINET_DISK_BLOCK_SIZE; ++i)
        data[i] = (uint8_t)(i ^ 0x5AU);
    memset(payload, 0, sizeof(payload));
    payload[0] = FN_DISK_PROTOCOL_VERSION;
    payload[4] = 8;
    put_u32le(payload + 5, 0x89ABCDEFUL);
    put_u16le(payload + 9, FUJINET_DISK_BLOCK_SIZE);
    build_success_response(NIO_DISK_WRITE_SECTOR, payload, sizeof(payload));
    scripted_results[0] = FN_ERR_TIMEOUT;
    scripted_results[1] = FN_ERR_TRANSPORT;
    scripted_failure_lengths[0] = 11;
    scripted_failure_lengths[1] = 22;

    CHECK("write context init", fn_disk_context_init(&context, context_exchange,
                                                     &diagnostics) == FN_OK);
    result = fn_disk_write_sector_context(
        &context, 8, 0x89ABCDEFUL, data, sizeof(data));

    CHECK("recovered write result", result == FN_OK);
    CHECK("recovered write attempts", transport_calls == 3);
    CHECK("recovered write length clear 1", incoming_response_lengths[0] == 0);
    CHECK("recovered write length clear 2", incoming_response_lengths[1] == 0);
    CHECK("recovered write length clear 3", incoming_response_lengths[2] == 0);
    CHECK("recovered write request length",
          captured_request_lengths[0] == NIO_DISK_WRITE_REQUEST_SIZE);
    CHECK("recovered write identical replay 2",
          memcmp(captured_requests[0], captured_requests[1],
                 NIO_DISK_WRITE_REQUEST_SIZE) == 0);
    CHECK("recovered write identical replay 3",
                 memcmp(captured_requests[0], captured_requests[2],
                 NIO_DISK_WRITE_REQUEST_SIZE) == 0);
    CHECK("recovered write body",
          memcmp(captured_requests[0] + 14, data, sizeof(data)) == 0);
}

static void test_context_diagnostics_records_retry_attempts(void)
{
    fn_disk_client_context_t context;
    fujinet_disk_retry_diagnostics_t diagnostics;
    uint8_t payload[11 + FUJINET_DISK_BLOCK_SIZE];
    uint8_t expected[FUJINET_DISK_BLOCK_SIZE];
    uint8_t actual[FUJINET_DISK_BLOCK_SIZE];
    uint16_t actual_length = 0;
    uint16_t i;
    uint8_t result;
    uint16_t success_length;
    uint16_t expected_response_length;

    reset_harness();
    for (i = 0; i < FUJINET_DISK_BLOCK_SIZE; ++i)
        expected[i] = (uint8_t)(i + 0x10U);
    memset(payload, 0, sizeof(payload));
    payload[0] = FN_DISK_PROTOCOL_VERSION;
    payload[4] = 1;
    put_u32le(payload + 5, 0x01020304UL);
    put_u16le(payload + 9, FUJINET_DISK_BLOCK_SIZE);
    memcpy(payload + 11, expected, sizeof(expected));
    build_success_response(NIO_DISK_READ_SECTOR, payload, sizeof(payload));
    scripted_results[0] = FN_ERR_TRANSPORT;
    scripted_failure_lengths[0] = 31;
    scripted_results[1] = FN_ERR_TIMEOUT;
    scripted_failure_lengths[1] = 45;
    scripted_results[2] = FN_OK;

    success_length = (uint16_t)(FN_HEADER_SIZE + sizeof(payload));
    expected_response_length = FUJINET_DISK_BLOCK_SIZE;
    CHECK("context diagnostics init",
          fn_disk_context_init(&context, context_exchange, &diagnostics) == FN_OK);
    result = fn_disk_read_sector_context(
        &context, 1, 0x01020304UL, actual, sizeof(actual), &actual_length);

    CHECK("diagnostics read result", result == FN_OK);
    CHECK("diagnostics read length", actual_length == expected_response_length);
    CHECK("diagnostics read body", memcmp(actual, expected, sizeof(actual)) == 0);
    CHECK("diagnostics attempts", diagnostics.attempts == 3);
    CHECK("diagnostics attempt 1", diagnostics.results[0] == FN_ERR_TRANSPORT);
    CHECK("diagnostics attempt 2", diagnostics.results[1] == FN_ERR_TIMEOUT);
    CHECK("diagnostics attempt 3", diagnostics.results[2] == FN_OK);
    CHECK("diagnostics attempt 1 response len", diagnostics.response_lengths[0] == 31);
    CHECK("diagnostics attempt 2 response len", diagnostics.response_lengths[1] == 45);
    CHECK("diagnostics attempt 3 response len", diagnostics.response_lengths[2] == success_length);
    CHECK("diagnostics attempt 1 inbound len", incoming_response_lengths[0] == 0);
    CHECK("diagnostics attempt 2 inbound len", incoming_response_lengths[1] == 0);
    CHECK("diagnostics attempt 3 inbound len", incoming_response_lengths[2] == 0);
    CHECK("diagnostics request replayed", captured_request_lengths[0] == captured_request_lengths[1] &&
                                            captured_request_lengths[1] == captured_request_lengths[2]);
    CHECK("diagnostics request immutable", memcmp(captured_requests[0], captured_requests[2],
                                                  captured_request_lengths[0]) == 0);
}

static void test_retry_outputs_are_request_local_per_call(void)
{
    fn_disk_client_context_t context;
    fujinet_disk_retry_diagnostics_t diagnostics;
    uint8_t first_request[NIO_DISK_READ_REQUEST_SIZE];
    uint8_t second_request[NIO_DISK_READ_REQUEST_SIZE];
    uint8_t first_payload[11 + FUJINET_DISK_BLOCK_SIZE];
    uint8_t second_payload[11 + FUJINET_DISK_BLOCK_SIZE];
    uint8_t first_expected[FUJINET_DISK_BLOCK_SIZE];
    uint8_t second_expected[FUJINET_DISK_BLOCK_SIZE];
    uint8_t first_storage[FUJINET_DISK_BLOCK_SIZE + 2];
    uint8_t *first_response = first_storage + 1;
    uint8_t second_storage[FUJINET_DISK_BLOCK_SIZE + 2];
    uint8_t *second_response = second_storage + 1;
    uint16_t first_length = 0;
    uint16_t second_length = 0;
    uint16_t i;
    uint8_t result;

    reset_harness();
    memset(first_storage, 0xC7, sizeof(first_storage));
    memset(second_storage, 0xD8, sizeof(second_storage));
    for (i = 0; i < FUJINET_DISK_BLOCK_SIZE; ++i) {
        first_expected[i] = (uint8_t)(0xAAU - i);
        second_expected[i] = (uint8_t)(0x55U + i);
    }
    build_read_request(first_request, 2, 0x11111111UL, FUJINET_DISK_BLOCK_SIZE);
    build_read_request(second_request, 3, 0x22222222UL, FUJINET_DISK_BLOCK_SIZE);

    memset(first_payload, 0, sizeof(first_payload));
    first_payload[0] = FN_DISK_PROTOCOL_VERSION;
    first_payload[4] = 2;
    put_u32le(first_payload + 5, 0x11111111UL);
    put_u16le(first_payload + 9, FUJINET_DISK_BLOCK_SIZE);
    memcpy(first_payload + 11, first_expected, sizeof(first_expected));
    memset(second_payload, 0, sizeof(second_payload));
    second_payload[0] = FN_DISK_PROTOCOL_VERSION;
    second_payload[4] = 3;
    put_u32le(second_payload + 5, 0x22222222UL);
    put_u16le(second_payload + 9, FUJINET_DISK_BLOCK_SIZE);
    memcpy(second_payload + 11, second_expected, sizeof(second_expected));

    CHECK("request-local context init",
          fn_disk_context_init(&context, context_exchange, &diagnostics) == FN_OK);

    scripted_results[0] = FN_ERR_TRANSPORT;
    scripted_failure_lengths[0] = 12;
    scripted_results[1] = FN_OK;
    build_success_response(NIO_DISK_READ_SECTOR, first_payload, sizeof(first_payload));
    result = fn_disk_read_sector_context(
        &context, 2, 0x11111111UL, first_response, FUJINET_DISK_BLOCK_SIZE,
        &first_length);
    CHECK("first call request-local result", result == FN_OK);
    CHECK("first call own length", first_length == FUJINET_DISK_BLOCK_SIZE);
    CHECK("first call own body", memcmp(first_response, first_expected, sizeof(first_expected)) == 0);

    build_success_response(NIO_DISK_READ_SECTOR, second_payload, sizeof(second_payload));
    CHECK("first absolute attempts", transport_calls == 2 && diagnostics.attempts == 2);
    CHECK("first diagnostics", diagnostics.results[0] == FN_ERR_TRANSPORT && diagnostics.results[1] == FN_OK && diagnostics.response_lengths[0] == 12);
    scripted_results[2] = FN_ERR_TIMEOUT;
    scripted_failure_lengths[2] = 9;
    scripted_results[3] = FN_OK;
    result = fn_disk_read_sector_context(
        &context, 3, 0x22222222UL, second_response, FUJINET_DISK_BLOCK_SIZE,
        &second_length);
    CHECK("second call request-local result", result == FN_OK);
    CHECK("second call own length", second_length == FUJINET_DISK_BLOCK_SIZE);
    CHECK("second call owns own body", memcmp(second_response, second_expected, sizeof(second_expected)) == 0);
    CHECK("second call did not reuse first buffer", memcmp(first_response, second_response, FUJINET_DISK_BLOCK_SIZE) != 0);
    CHECK("second absolute attempts", transport_calls == 4 && diagnostics.attempts == 2);
    CHECK("second diagnostics", diagnostics.results[0] == FN_ERR_TIMEOUT && diagnostics.results[1] == FN_OK && diagnostics.response_lengths[0] == 9);
    CHECK("second replay bytes", memcmp(captured_requests[2], second_request, sizeof(second_request)) == 0 && memcmp(captured_requests[2], captured_requests[3], sizeof(second_request)) == 0);
    CHECK("first replay bytes", memcmp(captured_requests[0], first_request, sizeof(first_request)) == 0 && memcmp(captured_requests[0], captured_requests[1], sizeof(first_request)) == 0);
    CHECK("all attempt lengths reset", incoming_response_lengths[0] == 0 && incoming_response_lengths[1] == 0 && incoming_response_lengths[2] == 0 && incoming_response_lengths[3] == 0);
    CHECK("first buffer independently preserved", memcmp(first_response, first_expected, sizeof(first_expected)) == 0);
    CHECK("first sentinels", first_storage[0] == 0xC7 && first_storage[sizeof(first_storage)-1] == 0xC7);
    CHECK("second sentinels", second_storage[0] == 0xD8 && second_storage[sizeof(second_storage)-1] == 0xD8);
}

static void test_persistent_fault(void)
{
    uint8_t request[NIO_DISK_READ_REQUEST_SIZE];
    uint8_t response[8] = {0};
    uint16_t response_length = 0xFFFFU;
    uint8_t result;

    reset_harness();
    build_read_request(request, 1, 7, FUJINET_DISK_BLOCK_SIZE);
    scripted_results[0] = FN_ERR_TRANSPORT;
    scripted_results[1] = FN_ERR_TIMEOUT;
    scripted_results[2] = FN_ERR_TRANSPORT;
    scripted_failure_lengths[0] = 10;
    scripted_failure_lengths[1] = 20;
    scripted_failure_lengths[2] = 30;

    result = retry_exchange(request, sizeof(request), response,
                          sizeof(response), &response_length);

    CHECK("persistent result preserved", result == FN_ERR_TRANSPORT);
    CHECK("persistent attempts", transport_calls == 3);
    CHECK("persistent response hidden", response_length == 0);
    CHECK("persistent lengths clear",
          incoming_response_lengths[0] == 0 &&
          incoming_response_lengths[1] == 0 &&
          incoming_response_lengths[2] == 0);
    CHECK("persistent identical replay 2",
          memcmp(captured_requests[0], captured_requests[1],
                 sizeof(request)) == 0);
    CHECK("persistent identical replay 3",
          memcmp(captured_requests[0], captured_requests[2],
                 sizeof(request)) == 0);
}

static void test_non_retryable_result(void)
{
    uint8_t request[NIO_DISK_WRITE_REQUEST_SIZE];
    uint8_t response[8];
    uint16_t response_length = 55;
    uint8_t result;

    reset_harness();
    build_write_request(request, 4, 99, FUJINET_DISK_BLOCK_SIZE);
    scripted_results[0] = FN_ERR_BUSY;
    scripted_failure_lengths[0] = 44;
    result = retry_exchange(request, sizeof(request), response,
                          sizeof(response), &response_length);

    CHECK("non-retryable result", result == FN_ERR_BUSY);
    CHECK("non-retryable one attempt", transport_calls == 1);
    CHECK("non-retryable response hidden", response_length == 0);
}

static void test_excluded_commands(void)
{
    static const uint8_t commands[] = {0x01, 0x02, 0x05, 0x06, 0x0E, 0x0F};
    uint8_t request[NIO_DISK_READ_REQUEST_SIZE];
    uint8_t response[8];
    uint16_t response_length;
    unsigned i;

    for (i = 0; i < sizeof(commands); ++i) {
        reset_harness();
        memset(request, 0, sizeof(request));
        request[0] = FN_DEVICE_DISK;
        request[1] = commands[i];
        request[6] = FN_DISK_PROTOCOL_VERSION;
        request[7] = 1;
        finish_packet(request, sizeof(request));
        scripted_results[0] = FN_ERR_TRANSPORT;
        response_length = 99;

        CHECK("excluded result",
              retry_exchange(request, sizeof(request), response,
                           sizeof(response), &response_length) ==
                  FN_ERR_TRANSPORT);
        CHECK("excluded one attempt", transport_calls == 1);
        CHECK("excluded response hidden", response_length == 0);
    }

    reset_harness();
    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[0] = FN_DEVICE_NETWORK;
    finish_packet(request, NIO_DISK_READ_REQUEST_SIZE);
    scripted_results[0] = FN_ERR_TIMEOUT;
    response_length = 99;
    CHECK("network read excluded",
          retry_exchange(request, NIO_DISK_READ_REQUEST_SIZE, response,
                       sizeof(response), &response_length) == FN_ERR_TIMEOUT);
    CHECK("network read one attempt", transport_calls == 1);
}

static void expect_invalid_single_attempt(const char *name,
                                          const uint8_t *request,
                                          uint16_t request_length)
{
    uint8_t response[8];
    uint16_t response_length = 88;

    reset_harness();
    scripted_results[0] = FN_ERR_TRANSPORT;
    CHECK(name, retry_exchange(request, request_length, response,
                             sizeof(response), &response_length) ==
                    FN_ERR_TRANSPORT);
    CHECK("invalid one attempt", transport_calls == 1);
    CHECK("invalid response hidden", response_length == 0);
}

static void test_malformed_sector_packets(void)
{
    uint8_t request[NIO_DISK_WRITE_REQUEST_SIZE];

    build_write_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    finish_packet(request, NIO_DISK_WRITE_REQUEST_SIZE - 1);
    expect_invalid_single_attempt("truncated write", request,
                                  NIO_DISK_WRITE_REQUEST_SIZE - 1);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[14] = 0xAA;
    finish_packet(request, NIO_DISK_READ_REQUEST_SIZE + 1);
    expect_invalid_single_attempt("oversize read", request,
                                  NIO_DISK_READ_REQUEST_SIZE + 1);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    put_u16le(request + 2, NIO_DISK_READ_REQUEST_SIZE - 1);
    request[FN_CHECKSUM_OFFSET] =
        fn_calc_packet_checksum(request, NIO_DISK_READ_REQUEST_SIZE);
    expect_invalid_single_attempt("encoded length mismatch", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[0] = FN_DEVICE_NETWORK;
    finish_packet(request, NIO_DISK_READ_REQUEST_SIZE);
    expect_invalid_single_attempt("wrong device", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[5] = 1;
    finish_packet(request, NIO_DISK_READ_REQUEST_SIZE);
    expect_invalid_single_attempt("non-simple descriptor", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[4] ^= 1;
    expect_invalid_single_attempt("invalid checksum", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    request[6] = FN_DISK_PROTOCOL_VERSION + 1;
    finish_packet(request, NIO_DISK_READ_REQUEST_SIZE);
    expect_invalid_single_attempt("wrong disk version", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, 0, 0, FUJINET_DISK_BLOCK_SIZE);
    expect_invalid_single_attempt("slot below range", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_read_request(request, FUJINET_DISK_FIRST_SLOT +
                                    FUJINET_DISK_UNIT_COUNT,
                       0, FUJINET_DISK_BLOCK_SIZE);
    expect_invalid_single_attempt("slot above range", request,
                                  NIO_DISK_READ_REQUEST_SIZE);

    build_write_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE - 1);
    expect_invalid_single_attempt("wrong sector length", request,
                                  NIO_DISK_WRITE_REQUEST_SIZE);
}

static void test_remote_timeout_is_not_retried(void)
{
    fn_disk_client_context_t context;
    fujinet_disk_retry_diagnostics_t diagnostics;
    uint8_t data[FUJINET_DISK_BLOCK_SIZE];
    uint16_t data_length = 0;
    uint8_t result;

    reset_harness();
    build_status_response(NIO_DISK_READ_SECTOR, FN_ERR_TIMEOUT);
    CHECK("remote timeout context init",
          fn_disk_context_init(&context, context_exchange, &diagnostics) == FN_OK);
    result = fn_disk_read_sector_context(
        &context, 1, 0, data, sizeof(data), &data_length);

    CHECK("remote timeout result", result == FN_ERR_TIMEOUT);
    CHECK("remote timeout one transport attempt", transport_calls == 1);
}

static void test_null_response_length_is_rejected(void)
{
    uint8_t request[NIO_DISK_READ_REQUEST_SIZE];
    uint8_t response[8];

    reset_harness();
    build_read_request(request, 1, 0, FUJINET_DISK_BLOCK_SIZE);
    CHECK("null response length rejected",
          retry_exchange(request, sizeof(request), response,
                       sizeof(response), NULL) == FN_ERR_INVALID);
    CHECK("null response length does not call transport", transport_calls == 0);
}

int main(void)
{
    test_recovered_read();
    test_recovered_write();
    test_context_diagnostics_records_retry_attempts();
    test_retry_outputs_are_request_local_per_call();
    test_persistent_fault();
    test_non_retryable_result();
    test_excluded_commands();
    test_malformed_sector_packets();
    test_remote_timeout_is_not_retried();
    test_null_response_length_is_rejected();

    if (failures != 0) {
        fprintf(stderr, "%u retry test(s) failed\n", failures);
        return 1;
    }
    puts("fujinet_nio_client retry tests passed");
    return 0;
}
