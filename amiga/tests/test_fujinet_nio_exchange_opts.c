#include <limits.h>
#include "fujinet_nio_exchange_opts.h"
#include "fujinet_nio_serial_config.h"
#include "fujinet_nio_session_diag.h"

#include "fn_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define COMPLETION_URI "host:/amiga-e2e-complete/nio-broker-isolated"

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static void test_packet_checksum_modes(void)
{
    uint8_t data[] = {
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC
    };
    uint8_t checksum_byte = data[FN_CHECKSUM_OFFSET];

    CHECK("generic checksum includes byte 4", fn_calc_checksum(data, sizeof(data)) == 0x6C);
    CHECK("packet checksum skips byte 4",
          fn_calc_packet_checksum(data, sizeof(data)) == 0xD1);
    CHECK("packet checksum does not mutate",
          data[FN_CHECKSUM_OFFSET] == checksum_byte);
}

static void test_parse_file_list_cold(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "file-list",
        "--backend", "cold",
        "--size", "8",
        "--uri", "tnfs://example/dir",
        "--baud", "38400",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[4];
    int n;

    CHECK("parse file-list cold",
          fn_nio_exchange_opts_parse(11, argv, &opts) == 0);
    CHECK("file-list type", opts.type == FN_NIO_EXCHANGE_TYPE_FILE_LIST);
    CHECK("file-list backend cold",
          opts.backend == FN_NIO_EXCHANGE_BACKEND_COLD);
    CHECK("file-list size 8", opts.has_size && opts.size == 8);
    CHECK("file-list uri", opts.uri != NULL &&
          strcmp(opts.uri, "tnfs://example/dir") == 0);
    CHECK("file-list baud", opts.baud == 38400UL);
    CHECK("file-list default trials", opts.trials == 1);

    n = fn_nio_exchange_opts_plan(&opts, steps, 4);
    CHECK("cold plan count", n == 2);
    CHECK("cold SET_BAUD first", steps[0] == FN_NIO_EXCHANGE_STEP_SET_BAUD);
    CHECK("cold MEASURE last", steps[1] == FN_NIO_EXCHANGE_STEP_MEASURE);
}

static void test_list_max_payload_bytes(void)
{
    uint8_t buf[128];
    int len;
    unsigned uri_len = 5;
    unsigned max_off;

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 8, -1);
    CHECK("list build 8 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list device", buf[0] == FN_DEVICE_FILE);
    CHECK("list command", buf[1] == 0x02);
    max_off = FN_HEADER_SIZE + 1 + 2 + uri_len + 2;
    CHECK("list maxPayloadBytes lo 8", buf[max_off] == 8);
    CHECK("list maxPayloadBytes hi 8", buf[max_off + 1] == 0);

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 420, -1);
    CHECK("list build 420 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list maxPayloadBytes lo 420", buf[max_off] == 0xA4);
    CHECK("list maxPayloadBytes hi 420", buf[max_off + 1] == 0x01);

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 512, -1);
    CHECK("list build 512 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list maxPayloadBytes lo 512", buf[max_off] == 0x00);
    CHECK("list maxPayloadBytes hi 512", buf[max_off + 1] == 0x02);

    CHECK("list rejects tiny cap",
          fn_nio_exchange_build_file_list(buf, 8, "sd0:/", 8, -1) < 0);
}

static void test_completion_marker_packet(void)
{
    static const uint8_t prefix[] = {
        0xFE, 0x02, 0x39, 0x00, 0xF6, 0x00, 0x01, 0x2C, 0x00
    };
    static const uint8_t suffix[] = { 0x00, 0x00, 0x00, 0x01 };
    uint8_t buf[64];
    int len;

    len = fn_nio_exchange_build_file_list(
        buf, 57, COMPLETION_URI, 256, -1);
    CHECK("completion marker exact length", len == 57);
    CHECK("completion marker exact prefix",
          memcmp(buf, prefix, sizeof(prefix)) == 0);
    CHECK("completion marker exact URI",
          memcmp(buf + sizeof(prefix), COMPLETION_URI,
                 sizeof(COMPLETION_URI) - 1) == 0);
    CHECK("completion marker exact suffix",
          memcmp(buf + sizeof(prefix) + sizeof(COMPLETION_URI) - 1,
                 suffix, sizeof(suffix)) == 0);
    CHECK("completion marker rejects one-byte-short cap",
          fn_nio_exchange_build_file_list(
              buf, 56, COMPLETION_URI, 256, -1) < 0);
    CHECK("completion marker rejects null URI",
          fn_nio_exchange_build_file_list(buf, sizeof(buf), NULL, 256, -1) < 0);
}

static void test_warm_host_get_plan(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "host-get",
        "--backend", "warm",
        "--baud", "19200",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[4];
    int n;
    int i;
    int saw_set_baud = 0;
    int saw_warmup = 0;
    int saw_get_baud = 0;
    int saw_measure = 0;

    CHECK("parse host-get warm",
          fn_nio_exchange_opts_parse(7, argv, &opts) == 0);
    n = fn_nio_exchange_opts_plan(&opts, steps, 4);
    CHECK("warm plan count", n == 3);
    for (i = 0; i < n; ++i) {
        if (steps[i] == FN_NIO_EXCHANGE_STEP_SET_BAUD) saw_set_baud = 1;
        if (steps[i] == FN_NIO_EXCHANGE_STEP_WARMUP) saw_warmup = 1;
        if (steps[i] == FN_NIO_EXCHANGE_STEP_GET_BAUD) saw_get_baud = 1;
        if (steps[i] == FN_NIO_EXCHANGE_STEP_MEASURE) saw_measure = 1;
    }
    CHECK("warm has GET_BAUD", saw_get_baud);
    CHECK("warm has WARMUP", saw_warmup);
    CHECK("warm has MEASURE", saw_measure);
    CHECK("warm has no SET_BAUD", !saw_set_baud);
    CHECK("warm GET_BAUD before WARMUP",
          steps[0] == FN_NIO_EXCHANGE_STEP_GET_BAUD &&
          steps[1] == FN_NIO_EXCHANGE_STEP_WARMUP);
}

static void test_warm_without_baud_skips_get(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "warm",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[4];
    int n;

    CHECK("parse clock warm no baud",
          fn_nio_exchange_opts_parse(5, argv, &opts) == 0);
    n = fn_nio_exchange_opts_plan(&opts, steps, 4);
    CHECK("warm no-baud plan count", n == 2);
    CHECK("warm no-baud WARMUP first",
          steps[0] == FN_NIO_EXCHANGE_STEP_WARMUP);
    CHECK("warm no-baud MEASURE last",
          steps[1] == FN_NIO_EXCHANGE_STEP_MEASURE);
}

static void test_usage_errors(void)
{
    struct fn_nio_exchange_opts opts;
    char *baud_too_high[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--baud", "230401", NULL
    };
    char *size_on_clock[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--size", "8", NULL
    };
    char *list_no_uri[] = {
        "fujinet-nio-exchange", "--type", "file-list", "--backend", "cold",
        "--size", "8", NULL
    };
    char *size_on_host[] = {
        "fujinet-nio-exchange", "--type", "host-get", "--backend", "warm",
        "--size", "8", NULL
    };
    char *unknown_type[] = {
        "fujinet-nio-exchange", "--type", "echo", "--backend", "cold", NULL
    };
    char *bad_size[] = {
        "fujinet-nio-exchange", "--type", "file-list", "--backend", "cold",
        "--size", "7", "--uri", "sd0:/", NULL
    };
    char *too_many_trials[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--trials", "100001", NULL
    };
    char *disk_without_provocation[] = {
        "fujinet-nio-exchange", "--type", "disk-read", "--backend", "cold",
        "--baud", "38400", "--slot", "1", "--lba", "0", NULL
    };
    char *disk_warm[] = {
        "fujinet-nio-exchange", "--type", "disk-write", "--provocation",
        "--backend", "warm", "--baud", "38400", "--slot", "1", "--lba", "0", NULL
    };
    char *list_flags_on_clock[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--list-flags", "2", NULL
    };

    CHECK("230401 is usage error",
          fn_nio_exchange_opts_parse(7, baud_too_high, &opts) != 0);
    CHECK("size on clock is usage error",
          fn_nio_exchange_opts_parse(7, size_on_clock, &opts) != 0);
    CHECK("file-list without uri is usage error",
          fn_nio_exchange_opts_parse(7, list_no_uri, &opts) != 0);
    CHECK("size on host-get is usage error",
          fn_nio_exchange_opts_parse(7, size_on_host, &opts) != 0);
    CHECK("unknown type is usage error",
          fn_nio_exchange_opts_parse(5, unknown_type, &opts) != 0);
    CHECK("size 7 is usage error",
          fn_nio_exchange_opts_parse(9, bad_size, &opts) != 0);
    CHECK("trials 100001 is usage error",
          fn_nio_exchange_opts_parse(7, too_many_trials, &opts) != 0);
    CHECK("disk requires explicit provocation",
          fn_nio_exchange_opts_parse(11, disk_without_provocation, &opts) != 0);
    CHECK("disk provocation requires cold",
          fn_nio_exchange_opts_parse(12, disk_warm, &opts) != 0);
    CHECK("list-flags on clock is usage error",
          fn_nio_exchange_opts_parse(7, list_flags_on_clock, &opts) != 0);
}

static void test_disk_provocation_parse(void)
{
    char *read_ok[] = {
        "fujinet-nio-exchange", "--type", "disk-read", "--provocation",
        "--backend", "cold", "--baud", "38400", "--slot", "1", "--lba", "0",
        NULL
    };
    char *write_ok[] = {
        "fujinet-nio-exchange", "--type", "disk-write", "--provocation",
        "--backend", "cold", "--baud", "38400", "--slot", "2", "--lba", "9",
        NULL
    };
    char *read_57600[] = {
        "fujinet-nio-exchange", "--type", "disk-read", "--provocation",
        "--backend", "cold", "--baud", "57600", "--slot", "1", "--lba", "0",
        NULL
    };
    struct fn_nio_exchange_opts opts;

    CHECK("parse disk-read provocation",
          fn_nio_exchange_opts_parse(12, read_ok, &opts) == 0);
    CHECK("disk-read type", opts.type == FN_NIO_EXCHANGE_TYPE_DISK_READ);
    CHECK("disk-read slot", opts.slot == 1 && opts.lba == 0 &&
          opts.provocation && opts.baud == 38400UL);
    CHECK("parse disk-write provocation",
          fn_nio_exchange_opts_parse(12, write_ok, &opts) == 0);
    CHECK("disk-write type", opts.type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE);
    CHECK("disk-write slot", opts.slot == 2 && opts.lba == 9);
    CHECK("parse disk-read 57600 provocation",
          fn_nio_exchange_opts_parse(12, read_57600, &opts) == 0);
    CHECK("disk-read 57600 baud", opts.baud == 57600UL &&
          opts.type == FN_NIO_EXCHANGE_TYPE_DISK_READ);
}

static void test_disk_provocation_packets(void)
{
    uint8_t read_packet[32];
    uint8_t write_packet[544];
    int read_len = fn_nio_exchange_build_disk_read(read_packet, sizeof(read_packet),
                                                   3, 0x12345678UL);
    int write_len = fn_nio_exchange_build_disk_write(write_packet, sizeof(write_packet),
                                                     3, 0x12345678UL);
    CHECK("disk read packet length", read_len == 14);
    CHECK("disk read packet shape", read_packet[0] == FN_DEVICE_DISK &&
          read_packet[1] == 3 && read_packet[7] == 3 &&
          read_packet[12] == 0 && read_packet[13] == 2);
    CHECK("disk write packet length", write_len == 526);
    CHECK("disk write packet shape", write_packet[0] == FN_DEVICE_DISK &&
          write_packet[1] == 4 && write_packet[7] == 3 &&
          write_packet[12] == 0 && write_packet[13] == 2);
    CHECK("disk write deterministic body", write_packet[14] == 0x5A &&
          write_packet[14 + 511] == (uint8_t)(511 ^ 0x5A));
    CHECK("disk read checksum", read_packet[FN_CHECKSUM_OFFSET] ==
          fn_calc_packet_checksum(read_packet, read_len));
    CHECK("disk write checksum", write_packet[FN_CHECKSUM_OFFSET] ==
          fn_calc_packet_checksum(write_packet, write_len));
}

static void test_clock_cold_plan_and_packet(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "cold",
        "--baud", "38400",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[4];
    int n;
    uint8_t buf[16];
    int len;

    CHECK("parse clock cold",
          fn_nio_exchange_opts_parse(7, argv, &opts) == 0);
    CHECK("clock type", opts.type == FN_NIO_EXCHANGE_TYPE_CLOCK);
    CHECK("clock backend cold", opts.backend == FN_NIO_EXCHANGE_BACKEND_COLD);
    CHECK("clock baud 38400", opts.baud == 38400UL);

    n = fn_nio_exchange_opts_plan(&opts, steps, 4);
    CHECK("clock cold plan count", n == 2);
    CHECK("clock cold SET_BAUD first",
          steps[0] == FN_NIO_EXCHANGE_STEP_SET_BAUD);
    CHECK("clock cold MEASURE last",
          steps[1] == FN_NIO_EXCHANGE_STEP_MEASURE);

    len = fn_nio_exchange_build_clock_get(buf, sizeof(buf));
    CHECK("clock GET exact packet",
          len == (int)FN_HEADER_SIZE &&
          memcmp(buf, "\x45\x01\x06\x00\x4c\x00", FN_HEADER_SIZE) == 0);
    CHECK("clock GET rejects undersized cap",
          fn_nio_exchange_build_clock_get(buf, FN_HEADER_SIZE - 1) < 0);
    CHECK("clock GET rejects null buffer",
          fn_nio_exchange_build_clock_get(NULL, sizeof(buf)) < 0);
}

static void test_higher_test_bauds(void)
{
    struct fn_nio_exchange_opts opts;
    char *baud57600[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--baud", "57600", NULL
    };
    char *baud115200[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--baud", "115200", NULL
    };

    CHECK("parse 57600", fn_nio_exchange_opts_parse(7, baud57600, &opts) == 0);
    CHECK("57600 stored", opts.baud == 57600UL);
    CHECK("parse 115200", fn_nio_exchange_opts_parse(7, baud115200, &opts) == 0);
    CHECK("115200 stored", opts.baud == 115200UL);
}

static void test_clock_get_tz_packet(void)
{
    uint8_t buf[16];
    int len;

    len = fn_nio_exchange_build_clock_get_tz(buf, sizeof(buf));
    CHECK("clock GET_TZ exact packet",
          len == (int)FN_HEADER_SIZE &&
          memcmp(buf, "\x45\x04\x06\x00\x4f\x00", FN_HEADER_SIZE) == 0);
    CHECK("clock GET_TZ rejects undersized cap",
          fn_nio_exchange_build_clock_get_tz(buf, FN_HEADER_SIZE - 1) < 0);
    CHECK("clock GET_TZ rejects null buffer",
          fn_nio_exchange_build_clock_get_tz(NULL, sizeof(buf)) < 0);
}

static void test_host_get_packet(void)
{
    uint8_t buf[16];
    int len;

    len = fn_nio_exchange_build_host_get(buf, sizeof(buf));
    CHECK("host-get exact packet",
          len == (int)FN_HEADER_SIZE + 1 &&
          memcmp(buf, "\xf0\x01\x07\x00\xf9\x00\x01",
                 FN_HEADER_SIZE + 1) == 0);
    CHECK("host-get rejects undersized cap",
          fn_nio_exchange_build_host_get(buf, FN_HEADER_SIZE) < 0);
    CHECK("host-get rejects null buffer",
          fn_nio_exchange_build_host_get(NULL, sizeof(buf)) < 0);
}

static void test_allowed_list_sizes(void)
{
    static char size_8[] = "8";
    static char size_16[] = "16";
    static char size_32[] = "32";
    static char size_64[] = "64";
    static char size_128[] = "128";
    static char size_256[] = "256";
    static char size_420[] = "420";
    static char size_512[] = "512";
    char *sizes[] = {
        size_8, size_16, size_32, size_64, size_128, size_256, size_420,
        size_512
    };
    unsigned i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        char *argv[] = {
            "fujinet-nio-exchange",
            "--type", "file-list",
            "--backend", "cold",
            "--size", sizes[i],
            "--uri", "sd0:/",
            NULL
        };
        struct fn_nio_exchange_opts opts;
        CHECK(sizes[i], fn_nio_exchange_opts_parse(9, argv, &opts) == 0);
        CHECK("size stored",
              opts.size == (unsigned)strtoul(sizes[i], NULL, 10));
    }
}

static void test_warm_baud_mismatch_before_warmup(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "host-get",
        "--backend", "warm",
        "--baud", "38400",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[4];
    int n;

    CHECK("parse warm baud",
          fn_nio_exchange_opts_parse(7, argv, &opts) == 0);
    n = fn_nio_exchange_opts_plan(&opts, steps, 4);
    CHECK("mismatch plan GET_BAUD before WARMUP",
          n >= 2 && steps[0] == FN_NIO_EXCHANGE_STEP_GET_BAUD &&
          steps[1] == FN_NIO_EXCHANGE_STEP_WARMUP);
    CHECK("matching baud ok", fn_nio_exchange_warm_baud_ok(38400UL, 38400UL));
    CHECK("mismatch baud not ok",
          !fn_nio_exchange_warm_baud_ok(38400UL, 19200UL));
    CHECK("GET_BAUD failure aborts",
          fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_GET_BAUD));
    CHECK("SET_BAUD failure aborts",
          fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_SET_BAUD));
    CHECK("WARMUP failure does not abort remaining trials",
          !fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_WARMUP));
    CHECK("MEASURE failure does not abort remaining log path",
          !fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_MEASURE));
    CHECK("SET_SERIAL failure aborts",
          fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_SET_SERIAL));
    CHECK("GET_SERIAL failure aborts",
          fn_nio_exchange_step_failure_aborts(FN_NIO_EXCHANGE_STEP_GET_SERIAL));
}

static void test_serial_device_opts(void)
{
    char *cold[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "cold",
        "--baud", "38400",
        "--serial-device", "fujinet-serial.device",
        NULL
    };
    char *warm[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "warm",
        "--baud", "38400",
        "--serial-device", "fujinet-serial.device",
        "--serial-unit", "0",
        NULL
    };
    char *unit_only[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "cold",
        "--serial-unit", "0",
        NULL
    };
    char *bad_name[] = {
        "fujinet-nio-exchange",
        "--type", "clock",
        "--backend", "cold",
        "--serial-device", "DEVS:fujinet-serial.device",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    int steps[5];
    int n;
    uint8_t payload[FUJINET_NIO_SERIAL_PAYLOAD_MAX];
    uint16_t payload_len;
    char name[FUJINET_NIO_SERIAL_NAME_MAX + 1];
    uint32_t unit;

    CHECK("parse serial-device cold",
          fn_nio_exchange_opts_parse(9, cold, &opts) == 0);
    CHECK("serial-device stored",
          opts.serial_device != NULL &&
          strcmp(opts.serial_device, "fujinet-serial.device") == 0);
    CHECK("serial-unit default 0", opts.serial_unit == 0UL && !opts.has_serial_unit);
    n = fn_nio_exchange_opts_plan(&opts, steps, 5);
    CHECK("cold serial plan count", n == 3);
    CHECK("cold SET_SERIAL first",
          steps[0] == FN_NIO_EXCHANGE_STEP_SET_SERIAL);
    CHECK("cold SET_BAUD second",
          steps[1] == FN_NIO_EXCHANGE_STEP_SET_BAUD);
    CHECK("cold MEASURE last",
          steps[2] == FN_NIO_EXCHANGE_STEP_MEASURE);

    CHECK("parse serial-device warm",
          fn_nio_exchange_opts_parse(11, warm, &opts) == 0);
    CHECK("serial-unit 0 stored", opts.has_serial_unit && opts.serial_unit == 0UL);
    n = fn_nio_exchange_opts_plan(&opts, steps, 5);
    CHECK("warm serial plan count", n == 4);
    CHECK("warm GET_SERIAL first",
          steps[0] == FN_NIO_EXCHANGE_STEP_GET_SERIAL);
    CHECK("warm GET_BAUD second",
          steps[1] == FN_NIO_EXCHANGE_STEP_GET_BAUD);
    CHECK("warm WARMUP third",
          steps[2] == FN_NIO_EXCHANGE_STEP_WARMUP);
    CHECK("warm MEASURE last",
          steps[3] == FN_NIO_EXCHANGE_STEP_MEASURE);

    CHECK("serial-unit without device is usage error",
          fn_nio_exchange_opts_parse(7, unit_only, &opts) != 0);
    CHECK("serial-device path is usage error",
          fn_nio_exchange_opts_parse(7, bad_name, &opts) != 0);

    CHECK("encode fujinet-serial",
          fujinet_nio_serial_encode(payload, sizeof(payload), &payload_len, 0,
                                    "fujinet-serial.device") == FN_OK);
    CHECK("decode fujinet-serial",
          fujinet_nio_serial_decode(payload, payload_len, &unit, name,
                                    sizeof(name)) == FN_OK &&
          unit == 0 && strcmp(name, "fujinet-serial.device") == 0);
    CHECK("name_ok serial.device",
          fujinet_nio_serial_name_ok("serial.device"));
    CHECK("name_ok rejects colon",
          !fujinet_nio_serial_name_ok("DEVS:serial.device"));
}

static void test_elapsed_and_trial_log(void)
{
    char elapsed[16];
    char line[192];

    CHECK("timer unavailable is dash",
          fn_nio_exchange_format_elapsed(0, 123UL, elapsed, sizeof(elapsed))
              == 0 &&
          strcmp(elapsed, "-") == 0);
    CHECK("timer available prints us",
          fn_nio_exchange_format_elapsed(1, 50UL, elapsed, sizeof(elapsed))
              == 0 &&
          strcmp(elapsed, "50") == 0);

    CHECK("trial log formats",
          fn_nio_exchange_format_trial_log(
              line, sizeof(line), 6, 12, "-", 0, 0, 0, 0,
              FN_NIO_EXCHANGE_BACKEND_COLD) == 0);
    CHECK("log req_len", strstr(line, "req_len=6") != NULL);
    CHECK("log resp_len", strstr(line, "resp_len=12") != NULL);
    CHECK("log elapsed_us dash", strstr(line, "elapsed_us=-") != NULL);
    CHECK("log ttfb_us dash", strstr(line, "ttfb_us=-") != NULL);
    CHECK("log result", strstr(line, "result=0") != NULL);
    CHECK("log cause", strstr(line, "cause=0") != NULL);
    CHECK("log native", strstr(line, "native=0") != NULL);
    CHECK("log status", strstr(line, "status=0") != NULL);
    CHECK("log backend cold", strstr(line, "backend=cold") != NULL);

    CHECK("warm log backend",
          fn_nio_exchange_format_trial_log(
              line, sizeof(line), 7, 8, "10", 1, 2, 3, 4,
              FN_NIO_EXCHANGE_BACKEND_WARM) == 0 &&
          strstr(line, "backend=warm") != NULL &&
          strstr(line, "elapsed_us=10") != NULL);
    CHECK("warm log result", strstr(line, "result=1") != NULL);
    CHECK("warm log cause", strstr(line, "cause=2") != NULL);
    CHECK("warm log native", strstr(line, "native=3") != NULL);
    CHECK("warm log status", strstr(line, "status=4") != NULL);
}

static void test_argc1_is_not_matrix(void)
{
    char *argv[] = { "fujinet-nio-exchange", NULL };
    struct fn_nio_exchange_opts opts;

    CHECK("argc 1 is not matrix flags",
          fn_nio_exchange_opts_parse(1, argv, &opts) != 0);
}

static void test_list_flags_packet(void)
{
    char *argv[] = {
        "fujinet-nio-exchange",
        "--type", "file-list",
        "--backend", "cold",
        "--size", "420",
        "--uri", "sd0:/",
        "--list-flags", "2",
        NULL
    };
    struct fn_nio_exchange_opts opts;
    uint8_t plain[128];
    uint8_t flagged[128];
    int plain_len;
    int flagged_len;
    unsigned flags_off;

    CHECK("parse list-flags 2",
          fn_nio_exchange_opts_parse(11, argv, &opts) == 0);
    CHECK("list-flags stored", opts.has_list_flags && opts.list_flags == 2);

    plain_len = fn_nio_exchange_build_file_list(
        plain, sizeof(plain), "sd0:/", 420, -1);
    flagged_len = fn_nio_exchange_build_file_list(
        flagged, sizeof(flagged), "sd0:/", 420, 2);
    CHECK("flagged list is one byte longer",
          plain_len > 0 && flagged_len == plain_len + 1);
    flags_off = (unsigned)flagged_len - 1;
    CHECK("FLS SORT_BY_NAME flag byte", flagged[flags_off] == 2);
    CHECK("flagged length field",
          (unsigned)(flagged[2] | (flagged[3] << 8)) ==
              (unsigned)flagged_len);
    CHECK("flagged checksum",
          flagged[FN_CHECKSUM_OFFSET] ==
              fn_calc_packet_checksum(flagged, (uint16_t)flagged_len));
}

static void test_verify_fujibus(void)
{
    uint8_t req[16];
    uint8_t resp[16];
    int len;

    len = fn_nio_exchange_build_host_get(req, sizeof(req));
    CHECK("host-get request built", len == (int)FN_HEADER_SIZE + 1);
    memcpy(resp, req, (unsigned)len);
    CHECK("echoed host-get verifies",
          fn_nio_exchange_verify_fujibus(req, (unsigned)len, resp,
                                         (unsigned)len) == 0);
    resp[1] = 0x02;
    CHECK("command mismatch fails verify",
          fn_nio_exchange_verify_fujibus(req, (unsigned)len, resp,
                                         (unsigned)len) != 0);
    memcpy(resp, req, (unsigned)len);
    resp[FN_CHECKSUM_OFFSET] ^= 1;
    CHECK("checksum mismatch fails verify",
          fn_nio_exchange_verify_fujibus(req, (unsigned)len, resp,
                                         (unsigned)len) != 0);
    memcpy(resp, req, (unsigned)len);
    CHECK("short response fails verify",
          fn_nio_exchange_verify_fujibus(req, (unsigned)len, resp, 5) != 0);
}

static void test_session_diag(void)
{
    uint8_t raw[8] = { 0xC0, 0xFE, 0x02, 0x01, 0x02, 0x00, 0x00, 0xC0 };
    uint8_t leftover[3] = { 0xAE, 0x01, 0x01 };
    uint8_t buf[64];
    uint8_t prefix[8] = { 0x02, 0x01, 0x02, 0xAE, 0x01, 0x00, 0x01, 0xC0 };
    unsigned n;
    fn_nio_session_diag_t d;

    n = fn_nio_session_diag_fill(buf, sizeof(buf), raw, sizeof(raw), 6, 513,
                                 leftover, sizeof(leftover));
    CHECK("fill size", n == FN_NIO_SESSION_DIAG_HDR + sizeof(raw) +
                               sizeof(leftover));
    CHECK("parse diag", fn_nio_session_diag_parse(buf, sizeof(buf), &d) == 0);
    CHECK("raw_len", d.raw_len == 8 && d.decoded_len == 6 && d.pkt_len == 513);
    CHECK("c0 count", d.c0_count == 2 && d.last_byte == 0xC0 &&
          d.first3[0] == 0xC0 && d.first3[1] == 0xFE);
    CHECK("class header", strcmp(fn_nio_session_diag_class(&d),
                                 "len-mismatch") == 0);
    CHECK("mismatch at 0 vs prefix",
          fn_nio_session_diag_first_mismatch(prefix, sizeof(prefix), raw,
                                             sizeof(raw)) == 0);
    CHECK("mismatch none",
          fn_nio_session_diag_first_mismatch(raw, sizeof(raw), raw,
                                             sizeof(raw)) == -1);
    CHECK("mismatch shorter",
          fn_nio_session_diag_first_mismatch(raw, 3, raw, sizeof(raw)) == 3);
    CHECK("ring align",
          fn_nio_session_diag_ring_align(leftover, sizeof(leftover), raw,
                                         sizeof(raw)) == -1);
    CHECK("ring align hit",
          fn_nio_session_diag_ring_align(raw + 1, 3, raw, sizeof(raw)) == 1);
    CHECK("old peek is not diag",
          fn_nio_session_diag_parse(prefix, sizeof(prefix), &d) != 0);
    {
        uint8_t eaten[8] = { 0x02, 0x01, 0x02, 0xAE, 0x01, 0x00, 0x01, 0xC0 };
        n = fn_nio_session_diag_fill(buf, sizeof(buf), eaten, sizeof(eaten),
                                     6, 0xAE02, NULL, 0);
        CHECK("prefix fill", n != 0 &&
              fn_nio_session_diag_parse(buf, sizeof(buf), &d) == 0);
        CHECK("class prefix", strcmp(fn_nio_session_diag_class(&d),
                                     "prefix") == 0);
    }
}

static void test_native_context(void)
{
    char *base[] = { "tool", "--type", "clock", "--backend", "warm",
                     "--installed-backend", "native", NULL, NULL, NULL,
                     NULL, NULL, NULL, NULL, NULL };
    const char *bad[][2] = {
        {"--baud", "38400"}, {"--serial-device", "serial.device"},
        {"--serial-unit", "0"}, {"--slot", "1"}, {"--slot", "0"},
        {"--lba", "0"}, {"--lba", "1"}, {"--provocation", NULL},
        {"--trials", "0"}, {"--trials", "-1"},
        {"--trials", "2x"},
        {"--trials", "99999999999999999999999999999999999999"},
        {"--backend", "cold"}, {"--backend", "unknown"},
        {"--type", "host-get"}, {"--type", "disk-read"},
        {"--type", "disk-write"}, {"--type", "unknown"},
        {"--installed-backend", "unknown"}, {"--unknown", "value"},
        {"--list-flags", "0"}, {"--type", "file-list"}
    };
    struct fn_nio_exchange_opts opts, valid;
    int steps[3];
    unsigned i;
    CHECK("native clock parse", fn_nio_exchange_opts_parse(7, base, &opts) == 0);
    valid = opts;
    CHECK("native clock plan", fn_nio_exchange_opts_plan(&opts, steps, 3) == 2 &&
          steps[0] == FN_NIO_EXCHANGE_STEP_WARMUP &&
          steps[1] == FN_NIO_EXCHANGE_STEP_MEASURE);
    steps[0] = steps[1] = 99;
    CHECK("native short plan leaves buffer untouched",
          fn_nio_exchange_opts_plan(&opts, steps, 1) < 0 && steps[0] == 99);
    CHECK("native zero capacity", fn_nio_exchange_opts_plan(&opts, steps, 0) < 0);
    CHECK("native null plan", fn_nio_exchange_opts_plan(&opts, NULL, 3) < 0);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char *first[] = {"tool", (char *)bad[i][0], (char *)bad[i][1],
                         "--type", "clock", "--backend", "warm",
                         "--installed-backend", "native"};
        base[7] = (char *)bad[i][0];
        base[8] = (char *)bad[i][1];
        CHECK(bad[i][0], fn_nio_exchange_opts_parse(bad[i][1] ? 9 : 8, base, &opts) < 0);
        /* Incompatible serial/disk flags reject even before context is supplied. */
        if (i < 7)
            CHECK("native option order", fn_nio_exchange_opts_parse(9, first, &opts) < 0);
    }
    base[7] = "--trials";
    CHECK("missing numeric value", fn_nio_exchange_opts_parse(8, base, &opts) < 0);
    base[7] = "--installed-backend";
    CHECK("missing context", fn_nio_exchange_opts_parse(8, base, &opts) < 0);
    base[2] = "file-list";
    base[7] = "--size"; base[8] = "128";
    CHECK("missing list URI", fn_nio_exchange_opts_parse(9, base, &opts) < 0);
    base[9] = "--uri"; base[10] = "host:/";
    base[11] = "--trials"; base[12] = "2";
    CHECK("native list parse", fn_nio_exchange_opts_parse(13, base, &opts) == 0);
    CHECK("native list plan", fn_nio_exchange_opts_plan(&opts, steps, 3) == 2 &&
          steps[0] == FN_NIO_EXCHANGE_STEP_WARMUP &&
          steps[1] == FN_NIO_EXCHANGE_STEP_MEASURE);
    {
        struct fn_nio_exchange_opts list = opts;
        steps[0] = 99;
        CHECK("native list bounded plan", fn_nio_exchange_opts_plan(&list, steps, 1) < 0 && steps[0] == 99);
        list.size = 1;
        CHECK("native list invalid size", fn_nio_exchange_opts_plan(&list, steps, 3) < 0);
        list = opts; list.has_size = 0;
        CHECK("native list absent size", fn_nio_exchange_opts_plan(&list, steps, 3) < 0);
        list = opts; list.uri = "";
        CHECK("native list empty URI", fn_nio_exchange_opts_plan(&list, steps, 3) < 0);
        list = opts; list.has_list_flags = 1; list.list_flags = 256;
        CHECK("native list invalid flags", fn_nio_exchange_opts_plan(&list, steps, 3) < 0);
        list.list_flags = 2;
        CHECK("native list valid flags", fn_nio_exchange_opts_plan(&list, steps, 3) == 2);
        list.has_list_flags = 0;
        CHECK("native list inconsistent flags", fn_nio_exchange_opts_plan(&list, steps, 3) < 0);
    }
    base[10] = "";
    CHECK("empty list URI", fn_nio_exchange_opts_parse(13, base, &opts) < 0);
#define BAD_NATIVE(field, value) do { \
    opts = valid; opts.field = value; steps[0] = 99; \
    CHECK("native struct " #field, fn_nio_exchange_opts_plan(&opts, steps, 3) < 0 && steps[0] == 99); \
} while (0)
    BAD_NATIVE(backend, FN_NIO_EXCHANGE_BACKEND_COLD);
    BAD_NATIVE(type, FN_NIO_EXCHANGE_TYPE_DISK_READ);
    BAD_NATIVE(type, FN_NIO_EXCHANGE_TYPE_DISK_WRITE);
    BAD_NATIVE(type, FN_NIO_EXCHANGE_TYPE_HOST_GET);
    BAD_NATIVE(type, FN_NIO_EXCHANGE_TYPE_FILE_LIST);
    BAD_NATIVE(type, 99);
    BAD_NATIVE(baud, 38400);
    BAD_NATIVE(serial_device, "serial.device");
    BAD_NATIVE(has_serial_unit, 1);
    BAD_NATIVE(serial_unit, 1);
    BAD_NATIVE(has_lba, 1);
    BAD_NATIVE(lba, 1);
    BAD_NATIVE(has_slot, 1);
    BAD_NATIVE(slot, 1);
    BAD_NATIVE(provocation, 1);
    BAD_NATIVE(trials, 0);
    BAD_NATIVE(trials, 100001);
    BAD_NATIVE(size, 128);
    BAD_NATIVE(list_flags, 2);
    BAD_NATIVE(has_size, 1);
    BAD_NATIVE(uri, "host:/");
    BAD_NATIVE(has_list_flags, 1);
    BAD_NATIVE(installed_backend, 99);
#undef BAD_NATIVE
    base[2] = "clock"; base[6] = "serial";
    CHECK("explicit serial context", fn_nio_exchange_opts_parse(7, base, &opts) == 0 &&
          opts.installed_backend == FN_NIO_EXCHANGE_INSTALLED_SERIAL);
    CHECK("explicit serial warm plan", fn_nio_exchange_opts_plan(&opts, steps, 3) == 2);
    base[7] = "--trials"; base[8] = "+2";
    CHECK("serial signed positive trials", fn_nio_exchange_opts_parse(9, base, &opts) == 0 && opts.trials == 2);
    base[8] = " 2";
    CHECK("serial whitespace trials", fn_nio_exchange_opts_parse(9, base, &opts) == 0 && opts.trials == 2);
    base[4] = "cold";
    CHECK("explicit serial cold parse", fn_nio_exchange_opts_parse(7, base, &opts) == 0);
    CHECK("explicit serial cold plan", fn_nio_exchange_opts_plan(&opts, steps, 3) == 2 &&
          steps[0] == FN_NIO_EXCHANGE_STEP_SET_BAUD);
}

static void test_negative_wraparound(void)
{
    char negative[80];
    char *args[] = {"tool", "--type", "clock", "--backend", "warm", "--trials", negative};
    struct fn_nio_exchange_opts opts;
    snprintf(negative, sizeof(negative), "-%lu", ULONG_MAX);
    CHECK("negative ULONG_MAX cannot become one", fn_nio_exchange_opts_parse(7, args, &opts) != 0);
    snprintf(negative, sizeof(negative), "  -%lu", ULONG_MAX - 16UL);
    CHECK("negative whitespace cannot wrap", fn_nio_exchange_opts_parse(7, args, &opts) != 0);
    strcpy(negative, " +2");
    CHECK("legacy positive whitespace", fn_nio_exchange_opts_parse(7, args, &opts) == 0 && opts.trials == 2);
}

static void test_ordinary_disk(void)
{
    char *args[] = {"tool", "--type", "disk-write", "--backend", "warm",
        "--slot", "8", "--lba", "17", "--fixture-uri", "host:/scratch.adf",
        "--disposable-fixture", "--write-intent", "--installed-backend", "native",
        "--trials", "2", "--baud", "9600"};
    struct fn_nio_exchange_opts opts, bad;
    int steps[3];
    char uri[513];
    CHECK("ordinary authorized", fn_nio_exchange_opts_parse(17, args, &opts) == 0);
    CHECK("ordinary plan no serial", fn_nio_exchange_opts_plan(&opts, steps, 3) == 1 &&
          steps[0] == FN_NIO_EXCHANGE_STEP_MEASURE);
    CHECK("ordinary serial rejected", fn_nio_exchange_opts_parse(19, args, &bad) != 0);
#define BAD_DISK(field, value) do { bad = opts; bad.field = value; \
    CHECK("ordinary invalid " #field, fn_nio_exchange_opts_plan(&bad, steps, 3) < 0); } while (0)
    BAD_DISK(fixture_uri, NULL); BAD_DISK(fixture_uri, "");
    BAD_DISK(disposable_fixture, 0); BAD_DISK(write_intent, 0);
    BAD_DISK(has_slot, 0); BAD_DISK(slot, 0); BAD_DISK(slot, 9);
    BAD_DISK(has_lba, 0); BAD_DISK(lba, 0x800000UL);
    BAD_DISK(backend, FN_NIO_EXCHANGE_BACKEND_COLD);
    BAD_DISK(baud, 9600); BAD_DISK(serial_device, "serial.device");
    BAD_DISK(has_serial_unit, 1); BAD_DISK(uri, "host:/");
    BAD_DISK(has_size, 1); BAD_DISK(has_list_flags, 1);
#undef BAD_DISK
    memset(uri, 'x', 512); uri[512] = '\0';
    args[10] = uri;
    CHECK("ordinary URI 512 parse rejected", fn_nio_exchange_opts_parse(17, args, &bad) != 0);
    bad = opts; bad.fixture_uri = uri;
    CHECK("ordinary URI 512 rejected", fn_nio_exchange_opts_plan(&bad, steps, 3) < 0);
    uri[511] = '\0';
    CHECK("ordinary URI 511 accepted", fn_nio_exchange_opts_plan(&bad, steps, 3) == 1);
    CHECK("ordinary URI 511 parse accepted", fn_nio_exchange_opts_parse(17, args, &bad) == 0);
    args[10] = "host:/scratch.adf";
    args[8] = "8388608";
    CHECK("ordinary offset overflow", fn_nio_exchange_opts_parse(17, args, &bad) != 0);
    args[8] = "17"; args[14] = "serial";
    CHECK("ordinary serial installed", fn_nio_exchange_opts_parse(17, args, &opts) == 0);
    args[2] = "disk-read";
    CHECK("ordinary read write conflict", fn_nio_exchange_opts_parse(17, args, &bad) != 0);
    CHECK("ordinary read authorized", fn_nio_exchange_opts_parse(12, args, &opts) == 0);
    CHECK("ordinary missing declaration", fn_nio_exchange_opts_parse(11, args, &bad) != 0);
}

int main(void)
{
    test_negative_wraparound();
    test_ordinary_disk();
    test_native_context();
    test_packet_checksum_modes();
    test_parse_file_list_cold();
    test_list_max_payload_bytes();
    test_completion_marker_packet();
    test_warm_host_get_plan();
    test_warm_without_baud_skips_get();
    test_usage_errors();
    test_disk_provocation_parse();
    test_disk_provocation_packets();
    test_clock_cold_plan_and_packet();
    test_higher_test_bauds();
    test_clock_get_tz_packet();
    test_host_get_packet();
    test_allowed_list_sizes();
    test_warm_baud_mismatch_before_warmup();
    test_serial_device_opts();
    test_elapsed_and_trial_log();
    test_argc1_is_not_matrix();
    test_list_flags_packet();
    test_verify_fujibus();
    test_session_diag();

    if (failures) {
        fprintf(stderr, "%u exchange-opts tests failed\n", failures);
        return 1;
    }
    return 0;
}
