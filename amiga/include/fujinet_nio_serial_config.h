#ifndef FUJINET_NIO_SERIAL_CONFIG_H
#define FUJINET_NIO_SERIAL_CONFIG_H

#include <stdint.h>
#include <string.h>

#include "fujinet-nio.h"
#include "fujinet_nio_endian.h"

#define FUJINET_NIO_SERIAL_NAME_MAX     30
#define FUJINET_NIO_SERIAL_PAYLOAD_MIN  (4 + 1 + 1)
#define FUJINET_NIO_SERIAL_PAYLOAD_MAX  (4 + FUJINET_NIO_SERIAL_NAME_MAX + 1)

static inline int fujinet_nio_serial_name_ok(const char *name)
{
    unsigned i;

    if (name == NULL || name[0] == '\0') return 0;
    for (i = 0; name[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)name[i];

        if (i >= FUJINET_NIO_SERIAL_NAME_MAX) return 0;
        if (c <= 32U || c == ':' || c == '/' || c == '\\' || c >= 127U)
            return 0;
    }
    return 1;
}

static inline uint8_t fujinet_nio_serial_encode(
    uint8_t *out, uint16_t cap, uint16_t *len, uint32_t unit, const char *name)
{
    unsigned n;

    if (out == NULL || len == NULL || !fujinet_nio_serial_name_ok(name))
        return FN_ERR_INVALID;
    n = (unsigned)strlen(name);
    if (cap < (uint16_t)(4U + n + 1U)) return FN_ERR_IO;
    fujinet_nio_put_le32(out, unit);
    memcpy(out + 4, name, n + 1U);
    *len = (uint16_t)(4U + n + 1U);
    return FN_OK;
}

static inline uint8_t fujinet_nio_serial_decode(
    const uint8_t *in, uint16_t len, uint32_t *unit, char *name,
    uint16_t name_cap)
{
    unsigned n;
    const char *src;

    if (in == NULL || unit == NULL || name == NULL) return FN_ERR_INVALID;
    if (len < FUJINET_NIO_SERIAL_PAYLOAD_MIN ||
        len > FUJINET_NIO_SERIAL_PAYLOAD_MAX)
        return FN_ERR_INVALID;
    if (in[len - 1U] != 0) return FN_ERR_INVALID;
    src = (const char *)(in + 4);
    n = (unsigned)strlen(src);
    if (n == 0U || n != (unsigned)(len - 5U) || n >= name_cap)
        return FN_ERR_INVALID;
    if (!fujinet_nio_serial_name_ok(src)) return FN_ERR_INVALID;
    *unit = fujinet_nio_get_le32(in);
    memcpy(name, src, n + 1U);
    return FN_OK;
}

#endif
