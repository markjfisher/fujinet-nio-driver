#include "fujinet_nio_exchange_opts.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_nio_endian.h"
#include "fujinet_nio_serial_config.h"
#include "fujinet_paula_uart.h"
#include "fn_protocol.h"

#define FN_NIO_EXCH_FILE_CMD_LIST 0x02
#define FN_NIO_EXCH_HOST_DEVICE 0xF0
#define FN_NIO_EXCH_HOST_GET_CURRENT 0x01
#define FN_NIO_EXCH_HOST_VERSION 1
#define FN_NIO_EXCH_DISK_READ 0x03
#define FN_NIO_EXCH_DISK_WRITE 0x04
#define FN_NIO_EXCH_DISK_REQUEST_SIZE 8
#define FN_NIO_EXCH_DISK_SECTOR_SIZE 512

static int parse_ulong(const char *text, unsigned long *out)
{
    char *end;

    if (text == NULL || text[0] == '\0') return -1;
    /* strtoul accepts a minus and negates modulo ULONG_MAX+1. Reject it
     * before conversion; retain legacy whitespace and explicit plus syntax. */
    while (isspace((unsigned char)*text)) ++text;
    if (*text == '-' || *text == '\0') return -1;
    errno = 0;
    *out = strtoul(text, &end, 10);
    if (*end != '\0' || errno == ERANGE) return -1;
    return 0;
}

static int allowed_baud(unsigned long baud)
{
    return baud >= FUJINET_SERIAL_BAUD_MIN && baud <= FUJINET_SERIAL_BAUD_MAX;
}

static int allowed_size(unsigned long size)
{
    return size == 8UL || size == 16UL || size == 32UL || size == 64UL ||
           size == 128UL || size == 256UL || size == 420UL || size == 512UL;
}

static int take_arg(int argc, char **argv, int *i, const char **out)
{
    if (*i + 1 >= argc) return -1;
    *i += 1;
    *out = argv[*i];
    return 0;
}

static int ordinary_disk_ok(const struct fn_nio_exchange_opts *o)
{
    return (o->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
            o->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) &&
        o->backend == FN_NIO_EXCHANGE_BACKEND_WARM && !o->provocation &&
        o->fixture_uri != NULL && o->fixture_uri[0] != '\0' &&
        strlen(o->fixture_uri) < 512 && o->disposable_fixture &&
        (o->type != FN_NIO_EXCHANGE_TYPE_DISK_WRITE || o->write_intent) &&
        (o->type != FN_NIO_EXCHANGE_TYPE_DISK_READ || !o->write_intent) &&
        o->has_slot && o->slot >= 1 && o->slot <= 8 && o->has_lba &&
        o->lba <= 0x7FFFFFUL && !o->baud && !o->serial_device &&
        !o->has_serial_unit && !o->serial_unit && !o->has_size && !o->size &&
        !o->uri && !o->has_list_flags && !o->list_flags &&
        o->trials >= 1 && o->trials <= 100000;
}

static int native_opts_ok(const struct fn_nio_exchange_opts *o)
{
    if (ordinary_disk_ok(o)) return 1;
    if (o->fixture_uri || o->disposable_fixture || o->write_intent) return 0;
    if (o->backend != FN_NIO_EXCHANGE_BACKEND_WARM || o->baud != 0 ||
        o->serial_device != NULL || o->has_serial_unit || o->serial_unit != 0 ||
        o->provocation || o->has_slot || o->slot || o->has_lba || o->lba ||
        o->trials < 1 || o->trials > 100000) return 0;
    if (o->type == FN_NIO_EXCHANGE_TYPE_CLOCK)
        return !o->has_size && !o->size && o->uri == NULL &&
               !o->has_list_flags && !o->list_flags;
    if (o->type == FN_NIO_EXCHANGE_TYPE_FILE_LIST)
        return o->has_size && allowed_size(o->size) && o->uri != NULL &&
               o->uri[0] != '\0' && o->list_flags <= 255 &&
               (o->has_list_flags || o->list_flags == 0);
    return 0;
}

int fn_nio_exchange_opts_parse(int argc, char **argv,
                               struct fn_nio_exchange_opts *out)
{
    int i;
    const char *value;
    unsigned long parsed;

    if (out == NULL || argv == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->trials = 1;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--type") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "clock") == 0)
                out->type = FN_NIO_EXCHANGE_TYPE_CLOCK;
            else if (strcmp(value, "host-get") == 0)
                out->type = FN_NIO_EXCHANGE_TYPE_HOST_GET;
            else if (strcmp(value, "file-list") == 0)
                out->type = FN_NIO_EXCHANGE_TYPE_FILE_LIST;
            else if (strcmp(value, "disk-read") == 0)
                out->type = FN_NIO_EXCHANGE_TYPE_DISK_READ;
            else if (strcmp(value, "disk-write") == 0)
                out->type = FN_NIO_EXCHANGE_TYPE_DISK_WRITE;
            else
                return -1;
        } else if (strcmp(argv[i], "--backend") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "cold") == 0)
                out->backend = FN_NIO_EXCHANGE_BACKEND_COLD;
            else if (strcmp(value, "warm") == 0)
                out->backend = FN_NIO_EXCHANGE_BACKEND_WARM;
            else
                return -1;
        } else if (strcmp(argv[i], "--installed-backend") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "serial") == 0)
                out->installed_backend = FN_NIO_EXCHANGE_INSTALLED_SERIAL;
            else if (strcmp(value, "native") == 0)
                out->installed_backend = FN_NIO_EXCHANGE_INSTALLED_NATIVE;
            else return -1;
        } else if (strcmp(argv[i], "--baud") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || !allowed_baud(parsed))
                return -1;
            out->baud = parsed;
        } else if (strcmp(argv[i], "--size") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || !allowed_size(parsed))
                return -1;
            out->has_size = 1;
            out->size = (unsigned)parsed;
        } else if (strcmp(argv[i], "--uri") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            out->uri = value;
        } else if (strcmp(argv[i], "--slot") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed < 1UL || parsed > 8UL)
                return -1;
            out->has_slot = 1;
            out->slot = (unsigned)parsed;
        } else if (strcmp(argv[i], "--lba") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed > 0xFFFFFFFFUL)
                return -1;
            out->has_lba = 1;
            out->lba = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--fixture-uri") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            out->fixture_uri = value;
        } else if (strcmp(argv[i], "--disposable-fixture") == 0) {
            out->disposable_fixture = 1;
        } else if (strcmp(argv[i], "--write-intent") == 0) {
            out->write_intent = 1;
        } else if (strcmp(argv[i], "--provocation") == 0) {
            out->provocation = 1;
        } else if (strcmp(argv[i], "--trials") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed < 1UL ||
                parsed > 100000UL)
                return -1;
            out->trials = (unsigned)parsed;
        } else if (strcmp(argv[i], "--serial-device") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (!fujinet_nio_serial_name_ok(value)) return -1;
            out->serial_device = value;
        } else if (strcmp(argv[i], "--serial-unit") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed > 255UL)
                return -1;
            out->serial_unit = parsed;
            out->has_serial_unit = 1;
        } else if (strcmp(argv[i], "--list-flags") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed > 255UL)
                return -1;
            out->has_list_flags = 1;
            out->list_flags = (unsigned)parsed;
        } else {
            return -1;
        }
    }

    if (out->type == 0 || out->backend == 0) return -1;
    if (out->installed_backend == FN_NIO_EXCHANGE_INSTALLED_NATIVE &&
        !native_opts_ok(out)) return -1;
    if (out->has_serial_unit && out->serial_device == NULL) return -1;
    if (out->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
        out->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) {
        if (!out->provocation) return ordinary_disk_ok(out) ? 0 : -1;
        if (out->fixture_uri || out->disposable_fixture || out->write_intent)
            return -1;
        if (!out->provocation || out->backend != FN_NIO_EXCHANGE_BACKEND_COLD ||
            out->baud == 0UL || out->slot == 0) return -1;
        if (out->has_size || out->uri != NULL) return -1;
    } else if (out->provocation || out->slot != 0 || out->lba != 0) {
        return -1;
    }
    if (out->fixture_uri || out->disposable_fixture || out->write_intent) return -1;
    if (out->has_list_flags && out->type != FN_NIO_EXCHANGE_TYPE_FILE_LIST)
        return -1;
    if (out->type == FN_NIO_EXCHANGE_TYPE_FILE_LIST) {
        if (!out->has_size || out->uri == NULL || out->uri[0] == '\0')
            return -1;
    } else if (out->has_size || out->uri != NULL) {
        return -1;
    }
    return 0;
}

int fn_nio_exchange_opts_plan(const struct fn_nio_exchange_opts *opts,
                              int *steps, int max_steps)
{
    int count = 0;

    if (opts == NULL || steps == NULL || max_steps < 1) return -1;
    if ((opts->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
         opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) && !opts->provocation) {
        if (!ordinary_disk_ok(opts) ||
            (opts->installed_backend != FN_NIO_EXCHANGE_INSTALLED_NATIVE &&
             opts->installed_backend != FN_NIO_EXCHANGE_INSTALLED_SERIAL)) return -1;
        steps[0] = FN_NIO_EXCHANGE_STEP_MEASURE;
        return 1;
    }
    if (opts->installed_backend == FN_NIO_EXCHANGE_INSTALLED_NATIVE) {
        if (!native_opts_ok(opts) || max_steps < 2) return -1;
        steps[0] = FN_NIO_EXCHANGE_STEP_WARMUP;
        steps[1] = FN_NIO_EXCHANGE_STEP_MEASURE;
        return 2;
    }
    if (opts->installed_backend != FN_NIO_EXCHANGE_INSTALLED_SERIAL) return -1;

    if (opts->serial_device != NULL) {
        if (count >= max_steps) return -1;
        if (opts->backend == FN_NIO_EXCHANGE_BACKEND_COLD ||
            opts->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
            opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE)
            steps[count++] = FN_NIO_EXCHANGE_STEP_SET_SERIAL;
        else if (opts->backend == FN_NIO_EXCHANGE_BACKEND_WARM)
            steps[count++] = FN_NIO_EXCHANGE_STEP_GET_SERIAL;
        else
            return -1;
    }

    if (opts->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
        opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) {
        if (count >= max_steps) return -1;
        steps[count++] = FN_NIO_EXCHANGE_STEP_SET_BAUD;
        if (count >= max_steps) return -1;
        steps[count++] = FN_NIO_EXCHANGE_STEP_MEASURE;
        return count;
    }
    if (opts->backend == FN_NIO_EXCHANGE_BACKEND_COLD) {
        if (count >= max_steps) return -1;
        steps[count++] = FN_NIO_EXCHANGE_STEP_SET_BAUD;
    } else if (opts->backend == FN_NIO_EXCHANGE_BACKEND_WARM) {
        if (opts->baud != 0UL) {
            if (count >= max_steps) return -1;
            steps[count++] = FN_NIO_EXCHANGE_STEP_GET_BAUD;
        }
        if (count >= max_steps) return -1;
        steps[count++] = FN_NIO_EXCHANGE_STEP_WARMUP;
    } else {
        return -1;
    }

    if (count >= max_steps) return -1;
    steps[count++] = FN_NIO_EXCHANGE_STEP_MEASURE;
    return count;
}

int fn_nio_exchange_warm_baud_ok(unsigned long want, unsigned long got)
{
    return want == got;
}

int fn_nio_exchange_step_failure_aborts(int step)
{
    /* GET_BAUD / SET_BAUD / GET_SERIAL / SET_SERIAL failures are setup
     * errors. WARMUP and MEASURE failures are logged and the remaining
     * trials continue so a warm overrun does not abort the whole command. */
    return step == FN_NIO_EXCHANGE_STEP_GET_BAUD ||
           step == FN_NIO_EXCHANGE_STEP_SET_BAUD ||
           step == FN_NIO_EXCHANGE_STEP_GET_SERIAL ||
           step == FN_NIO_EXCHANGE_STEP_SET_SERIAL;
}

int fn_nio_exchange_format_elapsed(int timer_ok, unsigned long us,
                                   char *buf, unsigned cap)
{
    int n;

    if (buf == NULL || cap < 2) return -1;
    if (!timer_ok) {
        buf[0] = '-';
        buf[1] = '\0';
        return 0;
    }
    n = snprintf(buf, cap, "%lu", us);
    if (n < 0 || (unsigned)n >= cap) return -1;
    return 0;
}

int fn_nio_exchange_format_trial_log(char *buf, unsigned cap,
                                     unsigned req_len, unsigned resp_len,
                                     const char *elapsed, unsigned result,
                                     unsigned cause, unsigned native,
                                     unsigned status, int backend)
{
    const char *backend_name;
    int n;

    if (buf == NULL || elapsed == NULL || cap < 1) return -1;
    backend_name = backend == FN_NIO_EXCHANGE_BACKEND_COLD ? "cold" : "warm";
    n = snprintf(
        buf, cap,
        "req_len=%u resp_len=%u elapsed_us=%s ttfb_us=- result=%u cause=%u "
        "native=%u status=%u backend=%s",
        req_len, resp_len, elapsed, result, cause, native, status,
        backend_name);
    if (n < 0 || (unsigned)n >= cap) return -1;
    return 0;
}

static int build_clock_command(uint8_t *buf, unsigned cap, uint8_t command)
{
    if (buf == NULL || cap < FN_HEADER_SIZE) return -1;
    buf[0] = FN_DEVICE_CLOCK;
    buf[1] = command;
    fujinet_nio_put_le16(buf + 2, FN_HEADER_SIZE);
    buf[5] = 0;
    buf[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(buf, FN_HEADER_SIZE);
    return FN_HEADER_SIZE;
}

int fn_nio_exchange_build_clock_get(uint8_t *buf, unsigned cap)
{
    return build_clock_command(buf, cap, FN_CMD_CLOCK_GET);
}

int fn_nio_exchange_build_clock_get_tz(uint8_t *buf, unsigned cap)
{
    return build_clock_command(buf, cap, FN_CMD_CLOCK_GET_TZ);
}

int fn_nio_exchange_build_host_get(uint8_t *buf, unsigned cap)
{
    uint16_t total = (uint16_t)(FN_HEADER_SIZE + 1);

    if (buf == NULL || cap < total) return -1;
    buf[0] = FN_NIO_EXCH_HOST_DEVICE;
    buf[1] = FN_NIO_EXCH_HOST_GET_CURRENT;
    fujinet_nio_put_le16(buf + 2, total);
    buf[5] = 0;
    buf[6] = FN_NIO_EXCH_HOST_VERSION;
    buf[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(buf, total);
    return (int)total;
}

int fn_nio_exchange_build_file_list(uint8_t *buf, unsigned cap,
                                    const char *uri, unsigned max_payload_bytes,
                                    int list_flags)
{
    uint16_t uri_len;
    uint16_t payload;
    uint16_t total;
    uint16_t offset = 0;
    uint16_t extra = 0;
    size_t n;

    if (buf == NULL || uri == NULL) return -1;
    if (list_flags > 255) return -1;
    n = strlen(uri);
    if (n > 0xFFFFUL) return -1;
    uri_len = (uint16_t)n;
    if (list_flags >= 0) extra = 1;
    payload = (uint16_t)(1 + 2 + uri_len + 2 + 2 + extra);
    total = (uint16_t)(FN_HEADER_SIZE + payload);
    if ((unsigned)total > cap ||
        (size_t)FN_HEADER_SIZE + 1U + 2U + n + 2U + 2U + extra > cap)
        return -1;

    buf[offset++] = FN_DEVICE_FILE;
    buf[offset++] = FN_NIO_EXCH_FILE_CMD_LIST;
    fujinet_nio_put_le16(buf + offset, total);
    offset += 2;
    buf[offset++] = 0;
    buf[offset++] = 0;
    buf[offset++] = 1;
    fujinet_nio_put_le16(buf + offset, uri_len);
    offset += 2;
    memcpy(buf + offset, uri, uri_len);
    offset = (uint16_t)(offset + uri_len);
    fujinet_nio_put_le16(buf + offset, 0);
    offset += 2;
    fujinet_nio_put_le16(buf + offset, (uint16_t)max_payload_bytes);
    offset += 2;
    if (list_flags >= 0)
        buf[offset++] = (uint8_t)list_flags;
    buf[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(buf, offset);
    return (int)offset;
}

int fn_nio_exchange_verify_fujibus(const uint8_t *request, unsigned req_len,
                                   const uint8_t *response, unsigned resp_len)
{
    uint16_t pkt_len;

    if (request == NULL || response == NULL) return -1;
    if (req_len < FN_HEADER_SIZE || resp_len < FN_HEADER_SIZE) return -1;
    if (response[0] != request[0] || response[1] != request[1]) return -1;
    pkt_len = fujinet_nio_get_le16(response + 2);
    if ((unsigned)pkt_len != resp_len) return -1;
    if (fn_calc_packet_checksum(response, (uint16_t)resp_len) !=
        response[FN_CHECKSUM_OFFSET])
        return -1;
    return 0;
}

static int build_disk_sector(uint8_t *buf, unsigned cap, unsigned slot,
                             uint32_t lba, int write)
{
    uint16_t payload = (uint16_t)(FN_NIO_EXCH_DISK_REQUEST_SIZE +
                                  (write ? FN_NIO_EXCH_DISK_SECTOR_SIZE : 0));
    uint16_t total = (uint16_t)(FN_HEADER_SIZE + payload);
    uint16_t i;

    if (buf == NULL || cap < total || slot < 1 || slot > 8) return -1;
    memset(buf, 0, total);
    buf[0] = FN_DEVICE_DISK;
    buf[1] = write ? FN_NIO_EXCH_DISK_WRITE : FN_NIO_EXCH_DISK_READ;
    fujinet_nio_put_le16(buf + 2, total);
    buf[FN_HEADER_SIZE] = FN_DISK_PROTOCOL_VERSION;
    buf[FN_HEADER_SIZE + 1] = (uint8_t)slot;
    fujinet_nio_put_le32(buf + FN_HEADER_SIZE + 2, lba);
    fujinet_nio_put_le16(buf + FN_HEADER_SIZE + 6,
                         FN_NIO_EXCH_DISK_SECTOR_SIZE);
    if (write) {
        for (i = 0; i < FN_NIO_EXCH_DISK_SECTOR_SIZE; ++i)
            buf[FN_HEADER_SIZE + FN_NIO_EXCH_DISK_REQUEST_SIZE + i] =
                (uint8_t)(i ^ 0x5A);
    }
    buf[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(buf, total);
    return (int)total;
}

int fn_nio_exchange_build_disk_read(uint8_t *buf, unsigned cap,
                                    unsigned slot, uint32_t lba)
{ return build_disk_sector(buf, cap, slot, lba, 0); }

int fn_nio_exchange_build_disk_write(uint8_t *buf, unsigned cap,
                                     unsigned slot, uint32_t lba)
{ return build_disk_sector(buf, cap, slot, lba, 1); }
