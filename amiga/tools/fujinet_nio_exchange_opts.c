#include "fujinet_nio_exchange_opts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_nio_endian.h"
#include "fujinet_nio_serial_config.h"
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
    *out = strtoul(text, &end, 10);
    if (*end != '\0') return -1;
    return 0;
}

static int allowed_baud(unsigned long baud)
{
    return baud == 9600UL || baud == 19200UL || baud == 38400UL;
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
            out->slot = (unsigned)parsed;
        } else if (strcmp(argv[i], "--lba") == 0) {
            if (take_arg(argc, argv, &i, &value) != 0) return -1;
            if (parse_ulong(value, &parsed) != 0 || parsed > 0xFFFFFFFFUL)
                return -1;
            out->lba = (uint32_t)parsed;
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
        } else {
            return -1;
        }
    }

    if (out->type == 0 || out->backend == 0) return -1;
    if (out->has_serial_unit && out->serial_device == NULL) return -1;
    if (out->type == FN_NIO_EXCHANGE_TYPE_DISK_READ ||
        out->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) {
        if (!out->provocation || out->backend != FN_NIO_EXCHANGE_BACKEND_COLD ||
            out->baud != 38400UL || out->slot == 0) return -1;
        if (out->has_size || out->uri != NULL) return -1;
    } else if (out->provocation || out->slot != 0 || out->lba != 0) {
        return -1;
    }
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
                                    const char *uri, unsigned max_payload_bytes)
{
    uint16_t uri_len;
    uint16_t payload;
    uint16_t total;
    uint16_t offset = 0;
    size_t n;

    if (buf == NULL || uri == NULL) return -1;
    n = strlen(uri);
    if (n > 0xFFFFUL) return -1;
    uri_len = (uint16_t)n;
    payload = (uint16_t)(1 + 2 + uri_len + 2 + 2);
    total = (uint16_t)(FN_HEADER_SIZE + payload);
    if ((unsigned)total > cap ||
        (size_t)FN_HEADER_SIZE + 1U + 2U + n + 2U + 2U > cap)
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
    buf[FN_CHECKSUM_OFFSET] = fn_calc_packet_checksum(buf, offset);
    return (int)offset;
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
