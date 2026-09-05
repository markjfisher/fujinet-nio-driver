#include <stdio.h>
#include <string.h>

#include "../channels/rs232/fujinet_nio_client.c"

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
    packet[4] = 0;
    packet[4] = packet_checksum(packet, length, true);
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
    fujinet_nio_disk_context_t context;
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

    CHECK("read context init", fujinet_nio_disk_context_init(&context) == FN_OK);
    result = fujinet_nio_disk_client.read_sector(
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
    fujinet_nio_disk_context_t context;
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

    CHECK("write context init", fujinet_nio_disk_context_init(&context) == FN_OK);
    result = fujinet_nio_disk_client.write_sector(
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

    result = nio_exchange(NULL, request, sizeof(request), response,
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
    result = nio_exchange(NULL, request, sizeof(request), response,
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
              nio_exchange(NULL, request, sizeof(request), response,
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
          nio_exchange(NULL, request, NIO_DISK_READ_REQUEST_SIZE, response,
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
    CHECK(name, nio_exchange(NULL, request, request_length, response,
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
    request[4] = 0;
    request[4] = packet_checksum(request, NIO_DISK_READ_REQUEST_SIZE, true);
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
    fujinet_nio_disk_context_t context;
    uint8_t data[FUJINET_DISK_BLOCK_SIZE];
    uint16_t data_length = 0;
    uint8_t result;

    reset_harness();
    build_status_response(NIO_DISK_READ_SECTOR, FN_ERR_TIMEOUT);
    CHECK("remote timeout context init",
          fujinet_nio_disk_context_init(&context) == FN_OK);
    result = fujinet_nio_disk_client.read_sector(
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
          nio_exchange(NULL, request, sizeof(request), response,
                       sizeof(response), NULL) == FN_ERR_INVALID);
    CHECK("null response length does not call transport", transport_calls == 0);
}

int main(void)
{
    test_recovered_read();
    test_recovered_write();
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
