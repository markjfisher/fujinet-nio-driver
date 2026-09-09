#ifndef FUJINET_NIO_SESSION_DIAG_H
#define FUJINET_NIO_SESSION_DIAG_H

#include <string.h>
#include <stdint.h>

/*
 * Error-path payload in the EXCHANGE response buffer (fn_response_length
 * stays 0). Does not grow FujiNetNIORequest.
 *
 * 0     magic 0xA5
 * 1     version 1
 * 2-3   raw_len LE        full SLIP bytes the session stored
 * 4-5   decoded_len LE
 * 6-7   pkt_len LE        FujiBus length field, 0 if decoded_len < 4
 * 8     c0_count          capped 255
 * 9     last_byte         0 if raw_len == 0
 * 10-12 first 3 raw bytes
 * 13    leftover_len      ring bytes copied after the closed frame
 * 14-15 raw_copy_len LE   bytes of raw following this header
 * 16+   raw copy, then leftover
 */
#define FN_NIO_SESSION_DIAG_MAGIC 0xA5
#define FN_NIO_SESSION_DIAG_VER   1
#define FN_NIO_SESSION_DIAG_HDR   16

typedef struct fn_nio_session_diag {
    uint16_t raw_len;
    uint16_t decoded_len;
    uint16_t pkt_len;
    uint16_t raw_copy_len;
    uint8_t c0_count;
    uint8_t last_byte;
    uint8_t first3[3];
    uint8_t leftover_len;
    const uint8_t *raw;
    const uint8_t *leftover;
} fn_nio_session_diag_t;

static inline unsigned fn_nio_session_diag_fill(
    uint8_t *out, unsigned cap,
    const uint8_t *raw, unsigned raw_len,
    unsigned decoded_len, unsigned pkt_len,
    const uint8_t *leftover, unsigned leftover_len)
{
    unsigned i;
    unsigned c0;
    unsigned raw_copy;
    unsigned left_copy;
    unsigned room;

    if (out == NULL || cap < FN_NIO_SESSION_DIAG_HDR) return 0;
    if (raw == NULL) raw_len = 0;
    if (leftover == NULL) leftover_len = 0;
    if (decoded_len > 0xFFFFU) decoded_len = 0xFFFFU;
    if (pkt_len > 0xFFFFU) pkt_len = 0xFFFFU;
    if (raw_len > 0xFFFFU) raw_len = 0xFFFFU;

    room = cap - FN_NIO_SESSION_DIAG_HDR;
    raw_copy = raw_len;
    if (raw_copy > room) raw_copy = room;
    left_copy = leftover_len;
    if (left_copy > 255U) left_copy = 255U;
    if (left_copy > room - raw_copy) left_copy = room - raw_copy;

    c0 = 0;
    for (i = 0; i < raw_len; ++i) {
        if (raw[i] == 0xC0U && c0 < 255U) c0 += 1U;
    }

    memset(out, 0, FN_NIO_SESSION_DIAG_HDR);
    out[0] = FN_NIO_SESSION_DIAG_MAGIC;
    out[1] = FN_NIO_SESSION_DIAG_VER;
    out[2] = (uint8_t)(raw_len & 0xFFU);
    out[3] = (uint8_t)((raw_len >> 8) & 0xFFU);
    out[4] = (uint8_t)(decoded_len & 0xFFU);
    out[5] = (uint8_t)((decoded_len >> 8) & 0xFFU);
    out[6] = (uint8_t)(pkt_len & 0xFFU);
    out[7] = (uint8_t)((pkt_len >> 8) & 0xFFU);
    out[8] = (uint8_t)c0;
    out[9] = (raw_len != 0U) ? raw[raw_len - 1U] : 0;
    if (raw_len >= 1U) out[10] = raw[0];
    if (raw_len >= 2U) out[11] = raw[1];
    if (raw_len >= 3U) out[12] = raw[2];
    out[13] = (uint8_t)left_copy;
    out[14] = (uint8_t)(raw_copy & 0xFFU);
    out[15] = (uint8_t)((raw_copy >> 8) & 0xFFU);
    if (raw_copy != 0U) memcpy(out + FN_NIO_SESSION_DIAG_HDR, raw, raw_copy);
    if (left_copy != 0U) {
        memcpy(out + FN_NIO_SESSION_DIAG_HDR + raw_copy, leftover, left_copy);
    }
    return FN_NIO_SESSION_DIAG_HDR + raw_copy + left_copy;
}

static inline int fn_nio_session_diag_parse(const uint8_t *buf, unsigned cap,
                                            fn_nio_session_diag_t *out)
{
    unsigned raw_copy;
    unsigned left;
    unsigned need;

    if (buf == NULL || out == NULL || cap < FN_NIO_SESSION_DIAG_HDR)
        return -1;
    if (buf[0] != FN_NIO_SESSION_DIAG_MAGIC || buf[1] != FN_NIO_SESSION_DIAG_VER)
        return -1;
    memset(out, 0, sizeof(*out));
    out->raw_len = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    out->decoded_len = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
    out->pkt_len = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
    out->c0_count = buf[8];
    out->last_byte = buf[9];
    out->first3[0] = buf[10];
    out->first3[1] = buf[11];
    out->first3[2] = buf[12];
    left = buf[13];
    raw_copy = (unsigned)(buf[14] | ((unsigned)buf[15] << 8));
    need = FN_NIO_SESSION_DIAG_HDR + raw_copy + left;
    if (need > cap) return -1;
    out->raw_copy_len = (uint16_t)raw_copy;
    out->leftover_len = (uint8_t)left;
    out->raw = (raw_copy != 0U) ? (buf + FN_NIO_SESSION_DIAG_HDR) : 0;
    out->leftover = (left != 0U)
                        ? (buf + FN_NIO_SESSION_DIAG_HDR + raw_copy)
                        : 0;
    return 0;
}

/* First index where got != exp. If one side is shorter, that length.
 * -1 if the copied prefixes match and lengths match. */
static inline int fn_nio_session_diag_first_mismatch(const uint8_t *got,
                                                     unsigned got_len,
                                                     const uint8_t *exp,
                                                     unsigned exp_len)
{
    unsigned n;
    unsigned i;

    if (got == NULL) got_len = 0;
    if (exp == NULL) exp_len = 0;
    n = (got_len < exp_len) ? got_len : exp_len;
    for (i = 0; i < n; ++i) {
        if (got[i] != exp[i]) return (int)i;
    }
    if (got_len != exp_len) return (int)n;
    return -1;
}

/* If leftover is a prefix of expected[k..], return k. Else -1. */
static inline int fn_nio_session_diag_ring_align(const uint8_t *leftover,
                                                 unsigned leftover_len,
                                                 const uint8_t *exp,
                                                 unsigned exp_len)
{
    unsigned k;

    if (leftover == NULL || leftover_len == 0 || exp == NULL ||
        leftover_len > exp_len)
        return -1;
    for (k = 0; k + leftover_len <= exp_len; ++k) {
        if (memcmp(exp + k, leftover, leftover_len) == 0)
            return (int)k;
    }
    return -1;
}

static inline const char *fn_nio_session_diag_class(const fn_nio_session_diag_t *d)
{
    if (d == NULL) return "none";
    if (d->raw_len == 0) return "empty";
    if (d->first3[0] != 0xC0U) return "prefix";
    if (d->c0_count >= 3U) return "extra-c0";
    if (d->pkt_len != 0U && d->decoded_len != d->pkt_len) return "len-mismatch";
    return "other";
}

#endif
