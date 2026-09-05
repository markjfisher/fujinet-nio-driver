#include "fujinet_nio_exchange_opts.h"

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

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 8);
    CHECK("list build 8 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list device", buf[0] == FN_DEVICE_FILE);
    CHECK("list command", buf[1] == 0x02);
    max_off = FN_HEADER_SIZE + 1 + 2 + uri_len + 2;
    CHECK("list maxPayloadBytes lo 8", buf[max_off] == 8);
    CHECK("list maxPayloadBytes hi 8", buf[max_off + 1] == 0);

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 420);
    CHECK("list build 420 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list maxPayloadBytes lo 420", buf[max_off] == 0xA4);
    CHECK("list maxPayloadBytes hi 420", buf[max_off + 1] == 0x01);

    len = fn_nio_exchange_build_file_list(buf, sizeof(buf), "sd0:/", 512);
    CHECK("list build 512 fits", len > (int)FN_HEADER_SIZE);
    CHECK("list maxPayloadBytes lo 512", buf[max_off] == 0x00);
    CHECK("list maxPayloadBytes hi 512", buf[max_off + 1] == 0x02);

    CHECK("list rejects tiny cap",
          fn_nio_exchange_build_file_list(buf, 8, "sd0:/", 8) < 0);
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
        buf, 57, COMPLETION_URI, 256);
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
              buf, 56, COMPLETION_URI, 256) < 0);
    CHECK("completion marker rejects null URI",
          fn_nio_exchange_build_file_list(buf, sizeof(buf), NULL, 256) < 0);
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
    char *baud57600[] = {
        "fujinet-nio-exchange", "--type", "clock", "--backend", "cold",
        "--baud", "57600", NULL
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

    CHECK("57600 is usage error",
          fn_nio_exchange_opts_parse(7, baud57600, &opts) != 0);
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

int main(void)
{
    test_parse_file_list_cold();
    test_list_max_payload_bytes();
    test_completion_marker_packet();
    test_warm_host_get_plan();
    test_warm_without_baud_skips_get();
    test_usage_errors();
    test_clock_cold_plan_and_packet();
    test_clock_get_tz_packet();
    test_host_get_packet();
    test_allowed_list_sizes();
    test_warm_baud_mismatch_before_warmup();
    test_elapsed_and_trial_log();
    test_argc1_is_not_matrix();

    if (failures) {
        fprintf(stderr, "%u exchange-opts tests failed\n", failures);
        return 1;
    }
    return 0;
}
